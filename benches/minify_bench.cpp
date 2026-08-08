//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// text->text minify rows per corpus file, plus a synthesized pretty-printed twitter
// (long whitespace runs — the compaction stress the raw corpus lacks). into-reuse
// isolates the sweep from allocation; owning is the whole public op.
// build via benches.duck; run pinned (the harness pins cpu 0)

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

// template so the requires-guarded branch is discarded without type-checking at HEAD
// (if constexpr in a non-template still checks both arms)
template<typename P = const u8 *>
void
minify_rows(const char *tag, P p, usize n)
{
  {
    micron::vector<u8> out;
    out.reserve(n + 8);
    mb::print_row(mb::bench_one("minify/into-reuse", tag, n, n, [&] {
      const max_t r = cjson::minify_into(p, n, out.begin(), n, {});
      mb::sink_size(usize(r));
    }));
    // scratch-hot row exists only where the overload does (post-rework trees); the
    // requires-guard keeps this TU buildable at HEAD for the a/b runner
    if constexpr ( requires(cjson::scratch &sc, P q) { cjson::minify_into(q, n, static_cast<u8 *>(nullptr), n, cjson::opts{}, sc); } ) {
      cjson::scratch sc;
      mb::print_row(mb::bench_one("minify/scratch", tag, n, n, [&] {
        const max_t r = cjson::minify_into(p, n, out.begin(), n, {}, sc);
        mb::sink_size(usize(r));
      }));
    }
  }
  mb::print_row(mb::bench_one("minify/owning", tag, n, n, [&] {
    auto r = cjson::minify(cjson::bytes{ p, n });
    mb::sink_bool(r.is_first());
  }));
}

};      // namespace

int
main()
{
  mb::pin_cpu0();
  mb::print_header();

  const char *files[] = { "sample/64kb.json", "sample/twitter.json", "sample/5MB.json", "sample/large-file.json" };
  for ( const char *f : files ) {
    auto data = slurp(f);
    if ( data.size() == 0 ) continue;
    minify_rows("cjson", data.cbegin(), data.size());
  }

  // pretty-printed twitter: parse -> write(indent 4); ~40% whitespace in long runs
  {
    auto data = slurp("sample/twitter.json");
    if ( data.size() != 0 ) {
      auto r = cjson::parse(cjson::bytes{ data.cbegin(), data.size() });
      if ( r.is_first() ) {
        const cjson::doc &d = r.cast<cjson::doc>();
        cjson::fjson pretty = cjson::write(d, cjson::style{ .indent = 4 });
        minify_rows("cjson-pretty4", pretty.begin(), pretty.size());
      }
    }
  }

  return 0;
}
