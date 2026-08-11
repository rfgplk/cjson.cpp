//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// cjson -> disk. four ways to get a document into a file, measured against each other:
//
//   flash/sink        pipelined io_uring chunks, never materializes the whole document
//   flash/whole       emit into a warm wbuf, then one flash::write of the lot
//   posix/whole       emit into a warm wbuf, then one posix::write of the lot
//   emit-only         the same emit with no file at all -- the floor the others pay above
//
// the point of the last row is that it separates "how fast can we serialize" from "what
// does the write cost on top", which is the only way to see whether the io path is
// actually low-overhead or just riding a fast emitter.
//
// TARGET defaults to /tmp (usually tmpfs here, i.e. page cache speed, no device). pass a
// path on a real filesystem as argv[1] to measure an actual disk.
//   build: duck build benches/flash_bench.cpp -i ../micron -i ../micron/src --perf --fp --no-ssp --no-lto -f
//   run:   taskset -c 0 ./bin/flash_bench [dir]

#include "_bench_common.hpp"

#include "../src/cjson/cjson.hpp"
#include "../src/cjson/flash.hpp"

#include <micron/io/flash.hpp>
#include <micron/linux/io.hpp>
#include <micron/linux/io/ext.hpp>
#include <micron/vector.hpp>

namespace
{

namespace mio = micron::io;
namespace mf = micron::io::flash;
namespace px = micron::posix;

micron::vector<u8>
slurp(const char *path)
{
  micron::vector<u8> out;
  px::fd_t fd = px::open_read(path);
  if ( !fd.open() ) return out;
  micron::stat_t st{};
  if ( micron::fstat(fd, st) < 0 or st.st_size <= 0 ) {
    px::close_fd(fd);
    return out;
  }
  out.reserve(static_cast<usize>(st.st_size) + 1);
  u8 buf[65536];
  for ( ;; ) {
    const max_t n = px::read(fd, buf, sizeof(buf));
    if ( n <= 0 ) break;
    for ( max_t i = 0; i < n; i++ ) out.push_back(buf[i]);
  }
  px::close_fd(fd);
  return out;
}

char g_path[512];

void
make_path(const char *dir, const char *leaf)
{
  usize k = 0;
  for ( const char *p = dir; *p and k < 400; ++p ) g_path[k++] = *p;
  if ( k and g_path[k - 1] != '/' ) g_path[k++] = '/';
  for ( const char *p = leaf; *p and k < 500; ++p ) g_path[k++] = *p;
  g_path[k] = 0;
}

};      // namespace

int
main(int argc, char **argv)
{
  mb::pin_cpu0();
  const char *dir = argc > 1 ? argv[1] : "/tmp";
  make_path(dir, "cjson_flash_bench.json");

  if ( !mf::available() ) {
    micron::io::println("io_uring unavailable on this host -- nothing to measure");
    return 0;
  }
  micron::io::println("=== cjson -> disk === target: ", g_path);
  micron::io::println("");
  mb::print_header();

  const char *files[] = { "sample/twitter.json", "sample/1MB.json", "sample/5MB.json", "sample/large-file.json" };

  for ( const char *f : files ) {
    auto data = slurp(f);
    if ( data.size() == 0 ) continue;
    const usize n = data.size();
    auto r = cjson::parse(cjson::bytes{ data.cbegin(), n }, cjson::opts{ .with_write_bound = true });
    if ( r.is_second() ) continue;
    const cjson::doc &d = r.cast<cjson::doc>();
    const usize ob = cjson::write(d).size();      // GB/s is per emitted byte

    // the floor: serialize into a warm buffer, touch no file
    {
      cjson::wbuf wb;
      mb::print_row(mb::bench_one("emit-only", "cjson", n, ob, [&] { mb::sink_size(usize(cjson::write_into(d, wb))); }));
    }

    // pipelined chunked io_uring
    {
      mb::print_row(mb::bench_one("flash/sink", "cjson", n, ob, [&] {
        mio::path_t p(g_path);
        mf::file file(p, mio::modes::readwritecreate);
        cjson::flash::sink s(file.raw_fd(), cjson::flash::sink_opts{ .chunks = 4, .chunk = 256 * 1024 });
        (void)cjson::flash::write_to(d, s);
        mb::sink_size(usize(s.drain()));
      }));
    }

    // emit whole, then hand the lot to io_uring in one call
    {
      cjson::wbuf wb;
      mb::print_row(mb::bench_one("flash/whole", "cjson", n, ob, [&] {
        const max_t w = cjson::write_into(d, wb);
        mio::path_t p(g_path);
        mf::file file(p, mio::modes::readwritecreate);
        mb::sink_size(usize(mf::pwrite(file.raw_fd(), wb.data(), usize(w), 0)));
      }));
    }

    // same, over plain posix write
    {
      cjson::wbuf wb;
      mb::print_row(mb::bench_one("posix/whole", "cjson", n, ob, [&] {
        const max_t w = cjson::write_into(d, wb);
        px::fd_t fd = px::open_write(g_path);
        usize done = 0;
        while ( done < usize(w) ) {
          const max_t k = px::write(fd, wb.data() + done, usize(w) - done);
          if ( k <= 0 ) break;
          done += usize(k);
        }
        px::close_fd(fd);
        mb::sink_size(done);
      }));
    }
    micron::io::println("");
  }

  (void)mf::remove(mio::path_t(g_path));
  return 0;
}
