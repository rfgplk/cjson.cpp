//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// field extraction head-to-head: cjson on-demand vs yyjson full-parse+get vs simdjson
// on-demand. two workloads: the ~200b jwt claims shape (the ox hot metric) and a
// twitter drill-down. comparison tu — libc allowed.
//   build: scripts/vsbuild benches/ondemand_vs.cpp && taskset -c 0 ./bin/ondemand_vs

#include "_bench_common.hpp"

#include "../src/cjson/cjson.hpp"

#include <yyjson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
void *sj_od_parser_new();
void sj_od_parser_free(void *);
long long sj_od_claims(void *, const char *, unsigned long);
long long sj_od_twitter(void *, const char *, unsigned long);
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

  // %%%%%%%%% jwt claims (~200 bytes, the per-request ox shape)
  {
    const char payload[]
        = R"({"iss":"https://ox.example","sub":"user-4211","aud":"api","iat":1735686000,"exp":1735689600,"scope":"read write","admin":false,"jti":"f3b1"})";
    const usize n = sizeof(payload) - 1;
    char *buf = static_cast<char *>(std::malloc(n + 64));
    std::memcpy(buf, payload, n);
    for ( usize i = 0; i < 64; i++ ) buf[n + i] = 0;

    mb::row group[3];
    {
      cjson::scratch sc;
      group[0] = mb::bench_one("jwt-claims", "cjson-ondemand", n, n, [&] {
        auto rv = cjson::iterate(buf, n, {}, sc);
        auto root = rv.cast<cjson::view>().root();
        const i64 exp = root["exp"].i64_or(0);
        const auto sub = root["sub"].str_raw();
        const bool admin = root["admin"].bool_or(true);
        mb::sink_size(usize(exp) + sub.len + usize(admin));
      });
    }

    group[1] = mb::bench_one("jwt-claims", "yyjson", n, n, [&] {
      yyjson_doc *d = yyjson_read(buf, n, 0);
      yyjson_val *r = yyjson_doc_get_root(d);
      const i64 exp = yyjson_get_sint(yyjson_obj_get(r, "exp"));
      const usize sl = yyjson_get_len(yyjson_obj_get(r, "sub"));
      const bool admin = yyjson_get_bool(yyjson_obj_get(r, "admin"));
      mb::sink_size(usize(exp) + sl + usize(admin));
      yyjson_doc_free(d);
    });

    {
      void *parser = sj_od_parser_new();
      group[2] = mb::bench_one("jwt-claims", "simdjson-ondemand", n, n, [&] { mb::sink_size(usize(sj_od_claims(parser, buf, n))); });
      sj_od_parser_free(parser);
    }
    mb::print_group(group, 3);
    std::free(buf);
  }

  // %%%%%%%%% twitter drill-down
  {
    usize n = 0;
    char *buf = slurp("sample/twitter.json", n);
    if ( buf ) {
      mb::row group[3];
      {
        cjson::scratch sc;
        group[0] = mb::bench_one("twitter-drill", "cjson-ondemand", n, n, [&] {
          auto rv = cjson::iterate(buf, n, {}, sc);
          auto root = rv.cast<cjson::view>().root();
          auto first = root["statuses"].at(0);
          const u64 id = first["id"].u64_or(0);
          const auto name = first["user"]["screen_name"].str_raw();
          mb::sink_size(usize(id) + name.len);
        });
      }

      group[1] = mb::bench_one("twitter-drill", "yyjson", n, n, [&] {
        yyjson_doc *d = yyjson_read(buf, n, 0);
        yyjson_val *r = yyjson_doc_get_root(d);
        yyjson_val *first = yyjson_arr_get(yyjson_obj_get(r, "statuses"), 0);
        const u64 id = yyjson_get_uint(yyjson_obj_get(first, "id"));
        const usize nl = yyjson_get_len(yyjson_obj_get(yyjson_obj_get(first, "user"), "screen_name"));
        mb::sink_size(usize(id) + nl);
        yyjson_doc_free(d);
      });

      {
        void *parser = sj_od_parser_new();
        group[2] = mb::bench_one("twitter-drill", "simdjson-ondemand", n, n, [&] { mb::sink_size(usize(sj_od_twitter(parser, buf, n))); });
        sj_od_parser_free(parser);
      }
      mb::print_group(group, 3);
      std::free(buf);
    }
  }
  return 0;
}
