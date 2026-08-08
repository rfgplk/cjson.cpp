//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// cjson-only writer rows (the HEAD-A/B target the vsbuild benches can't be): owning
// minify+pretty (alloc + bound + emit), caller-buffer reuse (bound check + emit), and
// bound-only (the sizing pass in isolation — O(nvals) today, O(1) after the doc-carried
// bound). build via benches.duck; run pinned (the harness pins cpu 0)

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
main()
{
  mb::pin_cpu0();
  mb::print_header();

  const char *files[] = { "sample/64kb.json", "sample/twitter.json", "sample/1MB.json", "sample/5MB.json" };

  for ( const char *f : files ) {
    auto data = slurp(f);
    if ( data.size() == 0 ) continue;
    const usize n = data.size();
    // with_write_bound: writer-bound users opt in at parse time; O(1) write_bound after
    auto r = cjson::parse(cjson::bytes{ data.cbegin(), n }, cjson::opts{ .with_write_bound = true });
    if ( r.is_second() ) continue;
    const cjson::doc &d = r.cast<cjson::doc>();

    mb::print_row(mb::bench_one("write/minify", "cjson", n, n, [&] {
      cjson::fjson o = cjson::write(d);
      mb::sink_size(o.size());
    }));

    mb::print_row(mb::bench_one("write/pretty4", "cjson", n, n, [&] {
      cjson::fjson o = cjson::write(d, cjson::style{ .indent = 4 });
      mb::sink_size(o.size());
    }));

    {
      const usize cap = cjson::write_bound(d);
      micron::vector<u8> out;
      out.reserve(cap + 8);
      mb::print_row(mb::bench_one("write/into-reuse", "cjson", n, n, [&] {
        const max_t w = cjson::write_into(d, cjson::wbytes{ out.begin(), cap });
        mb::sink_size(usize(w));
      }));
    }

    mb::print_row(mb::bench_one("write/bound-only", "cjson", n, n, [&] { mb::sink_size(cjson::write_bound(d)); }));
  }

  return 0;
}
