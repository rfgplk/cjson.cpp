//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// head-to-head writer: cjson write vs yyjson_write over pre-parsed corpus docs.
//   build: scripts/vsbuild benches/write_vs.cpp && taskset -c 0 ./bin/write_vs

#include "_bench_common.hpp"

#include "../src/cjson/cjson.hpp"

#include <yyjson.h>

#include <cstdio>
#include <cstdlib>

extern "C" long long sj_minify(const char *, unsigned long, char *);

namespace
{

char *
slurp(const char *path, usize &n)
{
  FILE *f = std::fopen(path, "rb");
  if ( !f ) return nullptr;
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  char *buf = static_cast<char *>(std::malloc(usize(sz) + 64));
  n = std::fread(buf, 1, usize(sz), f);
  for ( usize i = 0; i < 64; i++ ) buf[n + i] = 0;
  std::fclose(f);
  return buf;
}

};      // namespace

int
main()
{
  mb::pin_cpu0();
  mb::print_header();

  // labelled like corpus_vs's k_corpora, and for the same reason: the "### <label>
  // (<n> B)" banner below is what lets scripts/chart_vs put a name on the x axis
  // instead of a raw byte count
  struct corpus {
    const char *path;
    const char *label;
  };

  constexpr corpus files[] = {
    { "sample/64kb.json", "64kb" },
    { "sample/twitter.json", "twitter" },
    { "sample/1MB.json", "1MB" },
    { "sample/5MB.json", "5MB" },
  };

  for ( const corpus &c : files ) {
    usize n = 0;
    char *buf = slurp(c.path, n);
    if ( !buf ) continue;

    micron::io::println("### ", c.label, "  (", n, " B)");

    auto rc = cjson::parse(buf, n, cjson::opts{ .with_write_bound = true });
    yyjson_doc *yd = yyjson_read(buf, n, 0);
    if ( rc.is_second() or !yd ) {
      std::free(buf);
      continue;
    }
    const cjson::doc &cd = rc.cast<cjson::doc>();

    {
      mb::row group[2];
      group[0] = mb::bench_one("write/minify", "cjson", n, n, [&] {
        cjson::fjson o = cjson::write(cd);
        mb::sink_size(o.size());
      });
      group[1] = mb::bench_one("write/minify", "yyjson", n, n, [&] {
        usize len = 0;
        char *s = yyjson_write(yd, 0, &len);
        mb::sink_size(len);
        std::free(s);
      });
      mb::print_group(group, 2);
    }

    {
      mb::row group[2];
      group[0] = mb::bench_one("write/pretty", "cjson", n, n, [&] {
        cjson::fjson o = cjson::write(cd, cjson::style{ .indent = 4 });
        mb::sink_size(o.size());
      });
      group[1] = mb::bench_one("write/pretty", "yyjson", n, n, [&] {
        usize len = 0;
        char *s = yyjson_write(yd, YYJSON_WRITE_PRETTY, &len);
        mb::sink_size(len);
        std::free(s);
      });
      mb::print_group(group, 2);
    }

    // caller-buffer reuse, flag-matched
    {
      mb::row group[2];
      {
        const usize wcap = cjson::write_bound(cd);
        u8 *wbuf = static_cast<u8 *>(std::malloc(wcap));
        group[0] = mb::bench_one("write/minify-into", "cjson", n, n, [&] {
          const max_t w = cjson::write_into(cd, cjson::wbytes{ wbuf, wcap });
          mb::sink_size(usize(w));
        });
        std::free(wbuf);
      }
      {
        char *pool = static_cast<char *>(std::malloc(n * 2 + 65536));
        yyjson_alc alc;
        yyjson_alc_pool_init(&alc, pool, n * 2 + 65536);
        group[1] = mb::bench_one("write/minify-into", "yyjson-poolalc", n, n, [&] {
          usize len = 0;
          char *s = yyjson_write_opts(yd, 0, &alc, &len, nullptr);
          mb::sink_size(len);
          if ( s ) alc.free(alc.ctx, s);
        });
        std::free(pool);
      }
      mb::print_group(group, 2);
    }

    // text->text minify head-to-head (reused output buffer both sides)
    {
      u8 *mout = static_cast<u8 *>(std::malloc(n + 64));
      mb::row group[2];
      group[0] = mb::bench_one("minify/text", "cjson", n, n, [&] {
        const max_t r = cjson::minify_into(reinterpret_cast<const u8 *>(buf), n, mout, n, {});
        mb::sink_size(usize(r));
      });
      group[1] = mb::bench_one("minify/text", "simdjson", n, n,
                               [&] { mb::sink_size(usize(sj_minify(buf, n, reinterpret_cast<char *>(mout)))); });
      mb::print_group(group, 2);
      std::free(mout);
    }
    // yyjson has no text->text minifier; nearest public op is read + minify-write
    {
      mb::row solo = mb::bench_one("minify/roundtrip", "yyjson", n, n, [&] {
        yyjson_doc *d = yyjson_read(buf, n, 0);
        usize len = 0;
        char *s = yyjson_write(d, 0, &len);
        mb::sink_size(len);
        std::free(s);
        yyjson_doc_free(d);
      });
      mb::print_group(&solo, 1);
    }

    yyjson_doc_free(yd);
    std::free(buf);
  }
  return 0;
}
