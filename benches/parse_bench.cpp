//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// dom parse throughput per corpus file: owning parse, scratch-hot parse, validate-only
// and stage-1-only rows. cyc/op is a whole-document operation; bytes/cycle falls out of
// the size column. build via benches.duck; run pinned (the harness pins cpu 0)

#include "_bench_common.hpp"

#include "../src/cjson/cjson.hpp"

#include <micron/linux/io.hpp>
#include <micron/linux/io/ext.hpp>
#include <micron/vector.hpp>

namespace
{

micron::vector<u8>
slurp(const char *path)
{
  micron::vector<u8> out;
  micron::posix::fd_t fd = micron::posix::open_read(path);
  if ( !fd.open() ) return out;
  micron::stat_t st{};
  if ( micron::fstat(fd, st) < 0 or st.st_size <= 0 ) {
    micron::posix::close_fd(fd);
    return out;
  }
  out.reserve(static_cast<usize>(st.st_size) + 1);
  u8 buf[65536];
  for ( ;; ) {
    const max_t n = micron::posix::read(fd, buf, sizeof(buf));
    if ( n <= 0 ) break;
    for ( max_t i = 0; i < n; i++ ) out.push_back(buf[i]);
  }
  micron::posix::close_fd(fd);
  return out;
}

};      // namespace

int
main(int argc, char **argv)
{
  mb::pin_cpu0();
  mb::print_header();

  const bool only_s1 = argc > 1 and argv[1][0] == 's' and argv[1][1] == '1';
  const bool only_s2 = argc > 1 and argv[1][0] == 's' and argv[1][1] == '2';

  const char *files[] = { "sample/sample.json", "sample/64kb.json", "sample/128KB.json",     "sample/twitter.json",
                          "sample/1MB.json",    "sample/5MB.json",  "sample/large-file.json" };

  for ( const char *f : files ) {
    if ( only_s2 ) break;
    auto data = slurp(f);
    if ( data.size() == 0 ) continue;
    const cjson::bytes in{ data.cbegin(), data.size() };
    const cjson::opts nd{ .stop_when_done = true };

    if ( !only_s1 ) {
      // owning parse (alloc + copy + stage1 + build per op)
      mb::print_row(mb::bench_one("parse/owning", "cjson-scalar", data.size(), data.size(), [&] {
        auto r = cjson::parse(in, nd);
        mb::sink_bool(r.is_first());
      }));

      // scratch-hot parse (index buffer retained across ops)
      {
        cjson::scratch sc;
        mb::print_row(mb::bench_one("parse/scratch", "cjson-scalar", data.size(), data.size(), [&] {
          auto r = cjson::parse(in, nd, sc);
          mb::sink_bool(r.is_first());
        }));
      }

      // fully-warm parse
      {
        cjson::scratch sc;
        mb::print_row(mb::bench_one("parse/reuse", "cjson-scalar", data.size(), data.size(), [&] {
          auto r = cjson::parse_reuse(in, nd, sc);
          mb::sink_bool(r.is_first());
        }));
      }

      // validate only (no arena, no pool copy)
      mb::print_row(mb::bench_one("validate", "cjson-scalar", data.size(), data.size(),
                                  [&] { mb::sink_bool(cjson::validate(in, nd) == cjson::error::ok); }));
    }

    // stage 1 only: structural indexing over a padded copy (measures the sweep itself)
    {
      cjson::scratch sc;
      sc.ensure(data.size());
      micron::vector<u8> padded = data.clone();
      for ( usize i = 0; i < cjson::padding; i++ ) padded.push_back(i == 0 ? 0 : 0x20);
      mb::print_row(mb::bench_one("stage1", "cjson-scalar", data.size(), data.size(), [&] {
        const max_t n = cjson::__scan::index_input(padded.cbegin(), data.size(), sc.idx, {});
        mb::sink_size(usize(n));
      }));
    }
  }
  if ( only_s1 ) return 0;

  // stage-2 isolation
  const char *s2files[] = { "sample/twitter.json", "sample/5MB.json" };
  for ( const char *f : s2files ) {
    auto data = slurp(f);
    if ( data.size() == 0 ) continue;
    const usize n = data.size();
    micron::vector<u8> pristine = data.clone();
    for ( usize i = 0; i < cjson::padding; i++ ) pristine.push_back(i == 0 ? 0 : 0x20);
    micron::vector<u8> pool = pristine.clone();
    cjson::scratch sc;
    sc.ensure(n);
    const max_t nidx = cjson::__scan::index_input(pristine.cbegin(), n, sc.idx, {});
    if ( nidx < 0 ) continue;
    cjson::__parse::arena a;
    a.ensure(usize(nidx) + 8, 0);

    mb::print_row(mb::bench_one("stage2/poolcopy", "cjson-scalar", n, n, [&] {
      micron::memcpy(pool.begin(), pristine.cbegin(), n + cjson::padding);
      mb::sink_size(usize(pool.begin()[0]));
    }));

    mb::print_row(mb::bench_one("stage2/copy+build", "cjson-scalar", n, n, [&] {
      micron::memcpy(pool.begin(), pristine.cbegin(), n + cjson::padding);
      usize consumed = 0;
      const max_t r = cjson::__parse::build(pool.begin(), n, sc.idx, nidx, {}, a, consumed);
      mb::sink_bool(r >= 0);
    }));
  }

  // pure build (no copy term)
  {
    static u8 num[1u << 20];
    usize w = 0;
    num[w++] = u8('[');
    u64 seed = 0x9e3779b97f4a7c15ull;
    while ( w + 40 < sizeof(num) - cjson::padding ) {
      seed ^= seed << 13;
      seed ^= seed >> 7;
      seed ^= seed << 17;
      u8 *e = cjson::__itoa::write_u64(num + w, seed >> (seed % 48));
      w = usize(e - num);
      if ( (seed & 3) == 0 ) {
        num[w++] = u8('.');
        e = cjson::__itoa::write_u64(num + w, (seed >> 20) % 100000);
        w = usize(e - num);
      }
      num[w++] = u8(',');
    }
    num[w - 1] = u8(']');
    num[w] = 0;
    for ( usize i = w + 1; i < w + cjson::padding; i++ ) num[i] = 0x20;

    cjson::scratch sc;
    sc.ensure(w);
    const max_t nidx = cjson::__scan::index_input(num, w, sc.idx, {});
    if ( nidx >= 0 ) {
      cjson::__parse::arena a;
      a.ensure(usize(nidx) + 8, 0);
      mb::print_row(mb::bench_one("stage2/build-numeric", "cjson-scalar", w, w, [&] {
        usize consumed = 0;
        const max_t r = cjson::__parse::build(num, w, sc.idx, nidx, {}, a, consumed);
        mb::sink_bool(r >= 0);
      }));
    }
  }

  if ( only_s2 ) return 0;
  if ( argc > 1 and argv[1][0] == 'b' ) {
    auto data = slurp("sample/random.json");
    if ( data.size() != 0 ) {
      const cjson::bytes in{ data.cbegin(), data.size() };
      cjson::scratch sc;
      mb::print_row(mb::bench_one(
          "parse/scratch", "cjson-scalar", data.size(), data.size(),
          [&] {
            auto r = cjson::parse(in, {}, sc);
            mb::sink_bool(r.is_first());
          },
          4));
      mb::print_row(mb::bench_one(
          "validate", "cjson-scalar", data.size(), data.size(), [&] { mb::sink_bool(cjson::validate(in, {}) == cjson::error::ok); }, 4));
    }
  }
  return 0;
}
