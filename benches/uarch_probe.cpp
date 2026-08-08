//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// Per-region microarchitectural probe. Each hot region is isolated into a row whose
// setup happens ONCE outside the measured lambda, and a single region+corpus can be
// selected from argv — so wrapping one invocation in `perf stat` attributes nearly
// every counted cycle to that region rather than to the whole binary. That is the
// difference from parse_bench: this TU exists to be perf-stat'd, not to be read.
//
//   bin/uarch_probe [region] [corpus]
//     region  s1 poolcopy build validate wmin wpretty msweep   (default: all)
//     corpus  64kb 128KB twitter 1MB 5MB large                 (default: twitter)
//
// The in-process bbench counters (cycles/instructions/branches/branch-misses, 4
// events, unmultiplexed on this 5-usable-PMC box) stay the decision metric; the
// perf-stat passes in scripts/uarch add the frontend/backend/dispatch detail that
// bbench's fixed group cannot carry without re-introducing multiplex scaling.
//
// build via benches.duck (append --perf --fp --no-ssp --no-lto when profiling)

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

bool
streq(const char *a, const char *b) noexcept
{
  if ( a == nullptr or b == nullptr ) return false;
  while ( *a and *a == *b ) {
    ++a;
    ++b;
  }
  return *a == *b;
}

const char *
corpus_path(const char *name) noexcept
{
  if ( streq(name, "64kb") ) return "sample/64kb.json";
  if ( streq(name, "128KB") ) return "sample/128KB.json";
  if ( streq(name, "1MB") ) return "sample/1MB.json";
  if ( streq(name, "5MB") ) return "sample/5MB.json";
  if ( streq(name, "large") ) return "sample/large-file.json";
  return "sample/twitter.json";
}

};      // namespace

int
main(int argc, char **argv)
{
  mb::pin_cpu0();

  const char *region = argc > 1 ? argv[1] : nullptr;
  const char *corpus = argc > 2 ? argv[2] : "twitter";
  const bool all = region == nullptr or streq(region, "all");
  auto want = [&](const char *r) noexcept { return all or streq(region, r); };

  auto data = slurp(corpus_path(corpus));
  if ( data.size() == 0 ) return 0;
  const usize n = data.size();

  mb::print_header();

  // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
  // shared setup: a padded pristine image and a warm index array. Everything below
  // reads these; nothing measured allocates them.

  micron::vector<u8> pristine = data.clone();
  for ( usize i = 0; i < cjson::padding; i++ ) pristine.push_back(i == 0 ? 0 : 0x20);

  cjson::scratch sc;
  if ( !sc.ensure(n) ) return 0;
  const max_t nidx = cjson::__scan::index_input(pristine.cbegin(), n, sc.idx, {});
  if ( nidx < 0 ) return 0;

  // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
  // stage 1: the classify + serial spine + flatten sweep, over an already-padded
  // buffer. No copy term, no allocation.

  if ( want("s1") ) {
    mb::print_row(mb::bench_one("uarch/s1", "cjson", n, n, [&] {
      const max_t r = cjson::__scan::index_input(pristine.cbegin(), n, sc.idx, {});
      mb::sink_size(usize(r));
    }));
  }

  // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
  // stage 2. build MUTATES the pool (nul terminators from read_string, in-place
  // unescape), so it needs a pristine pool every rep — hence copy+build, with the
  // copy row alongside for the subtraction. That copy term is exactly what S3.1
  // proposes to fuse away, so it is a measured row here, not an accounting nuisance.

  if ( want("poolcopy") or want("build") ) {
    micron::vector<u8> pool = pristine.clone();
    cjson::__parse::arena a;
    if ( !a.ensure(usize(nidx) + 8, 0) ) return 0;

    if ( want("poolcopy") ) {
      mb::print_row(mb::bench_one("uarch/poolcopy", "cjson", n, n, [&] {
        micron::memcpy(pool.begin(), pristine.cbegin(), n + cjson::padding);
        mb::sink_size(usize(pool.begin()[0]));
      }));
    }

    if ( want("build") ) {
      mb::print_row(mb::bench_one("uarch/copy+build", "cjson", n, n, [&] {
        micron::memcpy(pool.begin(), pristine.cbegin(), n + cjson::padding);
        usize consumed = 0;
        const max_t r = cjson::__parse::build(pool.begin(), n, sc.idx, nidx, {}, a, consumed);
        mb::sink_bool(r >= 0);
      }));
    }
  }

  // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
  // the zero-materialization FSM. Non-mutating, so this one is clean — no copy term.

  if ( want("validate") ) {
    mb::print_row(mb::bench_one("uarch/validate-fsm", "cjson", n, n, [&] {
      usize consumed = 0;
      const max_t r = cjson::__parse::validate_indexes(pristine.cbegin(), n, sc.idx, nidx, {}, consumed);
      mb::sink_bool(r >= 0);
    }));
  }

  // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
  // writer: emit into a caller buffer sized once, so the row is the emitter alone
  // (no alloc, no bound pass — with_write_bound makes write_bound O(1) anyway).

  if ( want("wmin") or want("wpretty") ) {
    auto r = cjson::parse(cjson::bytes{ data.cbegin(), n }, cjson::opts{ .with_write_bound = true });
    if ( r.is_first() ) {
      const cjson::doc &d = r.cast<cjson::doc>();
      const usize cap = cjson::write_bound(d);
      micron::vector<u8> out;
      out.reserve(cap + 8);

      if ( want("wmin") ) {
        mb::print_row(mb::bench_one("uarch/emit-minify", "cjson", n, n, [&] {
          const max_t w = cjson::write_into(d, cjson::wbytes{ out.begin(), cap });
          mb::sink_size(usize(w));
        }));
      }
      if ( want("wpretty") ) {
        mb::print_row(mb::bench_one("uarch/emit-pretty4", "cjson", n, n, [&] {
          const max_t w = cjson::write_into(d, cjson::wbytes{ out.begin(), cap }, cjson::style{ .indent = 4 });
          mb::sink_size(usize(w));
        }));
      }
    }
  }

  // %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
  // minify: the compact sweep against a warm scratch, into a caller buffer.

  if ( want("msweep") ) {
    micron::vector<u8> out;
    out.reserve(n + 8);
    cjson::scratch msc;
    mb::print_row(mb::bench_one("uarch/minify-sweep", "cjson", n, n, [&] {
      const max_t r = cjson::minify_into(data.cbegin(), n, out.begin(), n, {}, msc);
      mb::sink_size(usize(r));
    }));
  }

  return 0;
}
