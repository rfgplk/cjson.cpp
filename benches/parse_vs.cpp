//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// head-to-head dom parse: cjson vs yyjson (installed) vs simdjson (amalgamated by
// scripts/vsbuild). flag-matched: every contender does a full dom parse of the same
// bytes; buffer-reusing modes are labeled. comparison tu — libc/stl allowed.
//   build: scripts/vsbuild benches/parse_vs.cpp && taskset -c 0 ./bin/parse_vs

#include "_bench_common.hpp"

#include "../src/cjson/cjson.hpp"

#include <yyjson.h>

#include <cstdio>
#include <cstdlib>

// simdjson lives behind comparison/simdjson_shim.cpp: micron reimplements the x86
// intrinsics, so micron headers and <immintrin.h>-world code cannot share a tu
extern "C" {
void *sj_dom_parser_new();
void sj_dom_parser_free(void *);
int sj_dom_parse(void *, const char *, unsigned long);
void *sj_od_parser_new();
void sj_od_parser_free(void *);
int sj_od_touch(void *, const char *, unsigned long);
};

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

  const char *files[]
      = { "sample/64kb.json", "sample/128KB.json", "sample/twitter.json", "sample/1MB.json", "sample/5MB.json", "sample/large-file.json" };

  for ( const char *f : files ) {
    usize n = 0;
    char *buf = slurp(f, n);
    if ( !buf ) continue;
    const cjson::bytes in{ reinterpret_cast<const u8 *>(buf), n };

    mb::row group[4];

    // owning doc: allocates and frees its pool + value slab per op, like yyjson's row
    {
      cjson::scratch sc;
      group[0] = mb::bench_one("parse", "cjson", n, n, [&] {
        auto r = cjson::parse(in, {}, sc);
        mb::sink_bool(r.is_first());
      });
    }

    // borrowed doc off a warm scratch
    {
      cjson::scratch sc;
      group[1] = mb::bench_one("parse", "cjson-reuse", n, n, [&] {
        auto r = cjson::parse_reuse(in, {}, sc);
        mb::sink_bool(r.is_first());
      });
    }

    group[2] = mb::bench_one("parse", "yyjson", n, n, [&] {
      yyjson_doc *d = yyjson_read(buf, n, 0);
      mb::sink_bool(d != nullptr);
      if ( d ) yyjson_doc_free(d);
    });

    {
      void *parser = sj_dom_parser_new();
      group[3] = mb::bench_one("parse", "simdjson-dom", n, n, [&] { mb::sink_bool(sj_dom_parse(parser, buf, n) == 1); });
      sj_dom_parser_free(parser);
    }

    mb::print_group(group, 4);

    // parse+touch-root does fundamentally less work (root-only touch, no full
    // materialize) than the "parse" group above — not comparable, stays its own group.
    {
      void *parser = sj_od_parser_new();
      mb::row solo = mb::bench_one("parse+touch-root", "simdjson-ondemand", n, n, [&] { mb::sink_bool(sj_od_touch(parser, buf, n) == 1); });
      sj_od_parser_free(parser);
      mb::print_group(&solo, 1);
    }

    std::free(buf);
  }
  return 0;
}
