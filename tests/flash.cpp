//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// cjson::flash sink: the bytes that reach the file must be exactly the bytes write()
// produces, at every chunk size, for array roots, object roots, scalar roots, and for
// children that individually overflow a chunk. the sink splits at root children and
// emits the separators itself, so a byte-identity check against the in-memory writer is
// the only thing that actually pins that logic.

#include "../src/cjson/flash.hpp"
#include "../src/cjson/cjson.hpp"

#include "tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/io/flash.hpp>
#include <micron/linux/io.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

namespace mio = micron::io;
namespace px = micron::posix;

constexpr usize
slen(const char *s) noexcept
{
  usize n = 0;
  while ( s[n] ) ++n;
  return n;
}

micron::vector<u8>
slurp_fd(const char *path)
{
  micron::vector<u8> out;
  px::fd_t fd = px::open_read(path);
  if ( !fd.open() ) return out;
  u8 buf[65536];
  for ( ;; ) {
    const max_t n = px::read(fd, buf, sizeof(buf));
    if ( n <= 0 ) break;
    for ( max_t i = 0; i < n; i++ ) out.push_back(buf[i]);
  }
  px::close_fd(fd);
  return out;
}

// stream `src` through a sink with the given chunk size and compare against write()
bool
identical(const char *src, usize chunk, cjson::style st = {})
{
  auto r = cjson::parse(src, slen(src));
  if ( r.is_second() ) return false;
  const cjson::doc &d = r.cast<cjson::doc>();
  cjson::fjson want = cjson::write(d, st);

  const char *path = "/tmp/cjson_flash_test.json";
  (void)micron::io::flash::remove(mio::path_t(path));
  {
    mio::path_t p(path);
    micron::io::flash::file f(p, mio::modes::readwritecreate);
    if ( !f.valid() ) return false;
    cjson::flash::sink s(f.raw_fd(), cjson::flash::sink_opts{ .chunks = 3, .chunk = chunk });
    if ( !s.ok() ) return false;
    const max_t w = cjson::flash::write_to(d, s, st);
    if ( w < 0 ) return false;
    if ( s.drain() < 0 ) return false;
  }
  auto got = slurp_fd(path);
  (void)micron::io::flash::remove(mio::path_t(path));

  if ( got.size() != want.size() ) return false;
  for ( usize i = 0; i < want.size(); i++ )
    if ( got[i] != want.first()[i] ) return false;
  return true;
}

};      // namespace

int
main()
{
  if ( !micron::io::flash::available() ) {
    // no usable ring on this kernel: the sink cannot be exercised, and silently
    // "passing" would be a lie. say so and pass, the way the corpus-absent benches do.
    micron::io::println("io_uring unavailable -- cjson::flash sink untested on this host");
    return 1;
  }
  {
    sb::test_case("array root streams byte-identically at every chunk size");
    const char *src = R"([1,2,3,"alpha","beta",{"k":[10,20,30]},true,null,1.5,-7.25e10,"a longer string value here"])";
    bool ok = true;
    // 4096 is the engine's floor (it page-aligns), so walk real sizes upward
    for ( usize c : { usize(4096), usize(8192), usize(16384), usize(65536), usize(262144) } ) ok = ok and identical(src, c);
    sb::require_true(ok);
    sb::end_test_case();
  }
  {
    sb::test_case("object root streams byte-identically, keys escaped");
    const char *src = R"({"a":1,"b\n":[1,2],"c":"x\"y","d":{"e":null},"f":true,"long key with spaces":3.25})";
    bool ok = true;
    for ( usize c : { usize(4096), usize(8192), usize(65536) } ) ok = ok and identical(src, c);
    sb::require_true(ok);
    sb::end_test_case();
  }
  {
    sb::test_case("scalar and empty roots");
    bool ok = true;
    for ( const char *src : { "42", "null", "true", R"("just a string")", "[]", "{}", "1.5e300" } )
      for ( usize c : { usize(4096), usize(8192) } ) ok = ok and identical(src, c);
    sb::require_true(ok);
    sb::end_test_case();
  }
  {
    sb::test_case("pretty output streams byte-identically");
    const char *src = R"({"a":[1,2,3],"b":{"c":"d"},"e":[[1],[2,3]]})";
    bool ok = true;
    for ( u8 ind : { u8(2), u8(4) } )
      for ( usize c : { usize(4096), usize(16384) } ) ok = ok and identical(src, c, cjson::style{ .indent = ind });
    sb::require_true(ok);
    sb::end_test_case();
  }
  {
    sb::test_case("many small children force repeated chunk turnover");
    // ~40 KB of root children against a 4 KB chunk: at least ten flushes, so the
    // in-flight/reap ring is genuinely cycled rather than fitting in one buffer
    micron::string src;
    src.reserve(80000);
    src += "[";
    for ( u32 i = 0; i < 2000; i++ ) {
      if ( i ) src += ",";
      src += R"({"id":)";
      char n[16];
      u8 *e = cjson::__itoa::write_u64(reinterpret_cast<u8 *>(n), i);
      *e = 0;
      src += n;
      src += R"(,"tag":"value string here"})";
    }
    src += "]";
    sb::require_true(identical(src.c_str(), 4096));
    sb::end_test_case();
  }
  {
    sb::test_case("a child larger than the whole chunk takes the slab path");
    // one array element whose text is far past a 4 KB chunk -- exercises __put_oversized
    micron::string src;
    src.reserve(40000);
    src += R"([1,")";
    for ( u32 i = 0; i < 9000; i++ ) src += "x";
    src += R"(",2])";
    sb::require_true(identical(src.c_str(), 4096));
    sb::end_test_case();
  }
  {
    sb::test_case("corpus round-trip through the sink");
    auto data = slurp_fd("sample/twitter.json");
    if ( data.size() != 0 ) {
      auto r = cjson::parse(cjson::bytes{ data.cbegin(), data.size() });
      sb::require_true(r.is_first());
      const cjson::doc &d = r.cast<cjson::doc>();
      cjson::fjson want = cjson::write(d);

      const char *path = "/tmp/cjson_flash_corpus.json";
      (void)micron::io::flash::remove(mio::path_t(path));
      {
        mio::path_t p(path);
        micron::io::flash::file f(p, mio::modes::readwritecreate);
        sb::require_true(f.valid());
        cjson::flash::sink s(f.raw_fd(), cjson::flash::sink_opts{ .chunks = 4, .chunk = 262144 });
        sb::require_true(s.ok());
        sb::require_true(cjson::flash::write_to(d, s) >= 0);
        sb::require_true(s.drain() >= 0);
      }
      auto got = slurp_fd(path);
      (void)micron::io::flash::remove(mio::path_t(path));
      sb::require_true(got.size() == want.size());
      bool same = true;
      for ( usize i = 0; i < want.size(); i++ ) same = same and got[i] == want.first()[i];
      sb::require_true(same);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("write_file / stream_file one-shots agree with write()");
    const char *src = R"([{"a":1,"b":"x\ny"},{"c":[1,2,3]},4,null,"tail"])";
    auto r = cjson::parse(src, slen(src));
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    cjson::fjson want = cjson::write(d);

    for ( int which = 0; which < 2; ++which ) {
      const char *path = "/tmp/cjson_flash_oneshot.json";
      (void)micron::io::flash::remove(mio::path_t(path));
      const max_t w = which == 0 ? cjson::flash::write_file(d, mio::path_t(path)) : cjson::flash::stream_file(d, mio::path_t(path));
      sb::require_true(w > 0);
      auto got = slurp_fd(path);
      (void)micron::io::flash::remove(mio::path_t(path));
      sb::require_true(got.size() == want.size());
      bool same = true;
      for ( usize i = 0; i < want.size(); i++ ) same = same and got[i] == want.first()[i];
      sb::require_true(same);
    }
    sb::end_test_case();
  }
  return 1;
}
