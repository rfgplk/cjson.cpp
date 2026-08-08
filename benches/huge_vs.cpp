//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include "_bench_common.hpp"

#include "../src/cjson/cjson.hpp"

#include <yyjson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {

void *sj_dom_parser_new();
void sj_dom_parser_free(void *);
int sj_dom_parse(void *, const char *, unsigned long);
void *sj_od_parser_new();
void sj_od_parser_free(void *);
long long sj_od_extract(void *, const char *, unsigned long, const char *);
// nlohmann
void *nl_new();
void nl_free(void *);
int nl_parse(void *, const char *, unsigned long);
int nl_load(void *, const char *, unsigned long);
long long nl_serialize(void *);
long long nl_extract(void *, const char *, unsigned long, const char *);
// rapidjson
void *rj_new();
void rj_free(void *);
int rj_parse(void *, const char *, unsigned long);
int rj_load(void *, const char *, unsigned long);
long long rj_serialize(void *);
long long rj_extract(void *, const char *, unsigned long, const char *);
// boost.json
void *bj_new();
void bj_free(void *);
int bj_parse(void *, const char *, unsigned long);
int bj_load(void *, const char *, unsigned long);
long long bj_serialize(void *);
long long bj_extract(void *, const char *, unsigned long, const char *);
// glaze
void *gl_new();
void gl_free(void *);
int gl_parse(void *, const char *, unsigned long);
int gl_load(void *, const char *, unsigned long);
long long gl_serialize(void *);
long long gl_extract(void *, const char *, unsigned long, const char *);
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
  if ( sz <= 0 ) {
    std::fclose(f);
    return nullptr;
  }
  char *buf = static_cast<char *>(std::malloc(usize(sz) + 64));
  if ( !buf ) return nullptr;
  n = std::fread(buf, 1, usize(sz), f);
  for ( usize i = 0; i < 64; i++ ) buf[n + i] = 0;
  std::fclose(f);
  return buf;
}

struct corpus {
  const char *path;
  const char *label;
  const char *ptr;
};

constexpr corpus k_corpora[] = {
  { "sample/web/1GB.json", "1GB", "/0/meta/rev" },
};

constexpr u32 k_mult_cjson = 700;
constexpr u32 k_mult_yyjson = 200;
constexpr u32 k_mult_simdjson = 200;
constexpr u32 k_mult_rapidjson = 600;
constexpr u32 k_mult_glaze = 500;
constexpr u32 k_mult_boost = 800;
constexpr u32 k_mult_nlohmann = 1500;

usize
mem_available_bytes()
{
  FILE *f = std::fopen("/proc/meminfo", "rb");
  if ( !f ) return 0;
  char line[256];
  unsigned long long kb = 0;
  while ( std::fgets(line, sizeof line, f) ) {
    if ( std::strncmp(line, "MemAvailable:", 13) == 0 ) {
      kb = std::strtoull(line + 13, nullptr, 10);
      break;
    }
  }
  std::fclose(f);
  return usize(kb) * 1024u;
}

usize
projected_bytes(usize n, u32 mult_pct)
{

  return usize((u64(n) * u64(mult_pct + 100)) / 100u);
}

bool
fits(usize n, u32 mult_pct, usize budget)
{
  return projected_bytes(n, mult_pct) <= budget;
}

void
report_excluded(const char *who, usize n, u32 mult_pct, usize budget)
{
  micron::io::println("    -- ", who, " excluded: projected ", projected_bytes(n, mult_pct) >> 20, " MB resident (", mult_pct / 100,
                      "x) exceeds the ", budget >> 20, " MB budget");
}

long long
cj_checksum(cjson::cur c)
{
  switch ( c.type() ) {
  case cjson::kind::number:
    return static_cast<long long>(c.f64_or(0));
  case cjson::kind::string:
    return static_cast<long long>(c.str_raw().len);
  case cjson::kind::boolean:
    return c.bool_or(false) ? 1 : 2;
  case cjson::kind::array:
  case cjson::kind::object:
    return static_cast<long long>(c.count());
  default:
    return 0;
  }
}

long long
cj_extract(cjson::bytes in, const char *ptr, cjson::scratch &sc)
{
  auto rv = cjson::iterate(in, sc);
  if ( rv.is_second() ) return 0;
  return cj_checksum(rv.cast<cjson::view>().root().at_pointer(ptr));
}

long long
cj_extract_dom(cjson::bytes in, const char *ptr, cjson::scratch &sc)
{
  auto r = cjson::parse_reuse(in, {}, sc);
  if ( r.is_second() ) return 0;
  const cjson::val v = r.cast<cjson::doc>().root().at_pointer(ptr);
  switch ( v.type() ) {
  case cjson::kind::number:
    return static_cast<long long>(v.f64_or(0));
  case cjson::kind::string:
    return static_cast<long long>(v.str_or().len);
  case cjson::kind::boolean:
    return v.bool_or(false) ? 1 : 2;
  case cjson::kind::array:
  case cjson::kind::object:
    return static_cast<long long>(v.size());
  default:
    return 0;
  }
}

u64
reps_cap(usize n)
{
  if ( n > (512u << 20) ) return 1;
  if ( n > (16u << 20) ) return 4;
  return 16;
}

bool
want(int argc, char **argv, const char *op)
{
  bool any_op = false;
  for ( int i = 1; i < argc; i++ ) {
    if ( std::strncmp(argv[i], "only=", 5) == 0 ) continue;
    if ( std::strncmp(argv[i], "budget=", 7) == 0 ) continue;
    any_op = true;
    if ( std::strcmp(argv[i], op) == 0 ) return true;
  }
  return !any_op;
}

bool
want_corpus(int argc, char **argv, const char *label)
{
  const char *list = nullptr;
  for ( int i = 1; i < argc; i++ )
    if ( std::strncmp(argv[i], "only=", 5) == 0 ) list = argv[i] + 5;
  if ( !list ) return true;

  const usize ln = std::strlen(label);
  for ( const char *p = list; *p; ) {
    const char *e = p;
    while ( *e and *e != ',' ) ++e;
    if ( usize(e - p) == ln and std::strncmp(p, label, ln) == 0 ) return true;
    p = (*e == ',') ? e + 1 : e;
  }
  return false;
}

usize
resolve_budget(int argc, char **argv)
{
  for ( int i = 1; i < argc; i++ )
    if ( std::strncmp(argv[i], "budget=", 7) == 0 ) return usize(std::strtoull(argv[i] + 7, nullptr, 10)) << 20;
  const usize avail = mem_available_bytes();
  if ( avail == 0 ) return usize(4) << 30;
  return (avail / 5u) * 4u;
}

};      // namespace

int
main(int argc, char **argv)
{
  mb::pin_cpu0();
  mb::print_header();

  const bool do_parse = want(argc, argv, "parse");
  const bool do_extract = want(argc, argv, "extract");
  const bool do_serialize = want(argc, argv, "serialize");
  const usize budget = resolve_budget(argc, argv);

  void *sj_dom = sj_dom_parser_new();
  void *sj_od = sj_od_parser_new();
  void *nl = nl_new();
  void *rj = rj_new();
  void *bj = bj_new();
  void *gl = gl_new();

  for ( const corpus &c : k_corpora ) {
    if ( !want_corpus(argc, argv, c.label) ) continue;
    usize n = 0;
    char *buf = slurp(c.path, n);
    if ( !buf ) continue;
    const cjson::bytes in{ reinterpret_cast<const u8 *>(buf), n };
    const u64 cap = reps_cap(n);

    if ( cjson::validate(in) != cjson::error::ok ) {
      micron::io::println("  !! ", c.label, " does not validate — skipped");
      std::free(buf);
      continue;
    }

    micron::io::println("### ", c.label, "  (", n, " B)");
    micron::io::println("    budget ", budget >> 20, " MB  (80% MemAvailable unless budget= given)");

    const bool ok_cjson = fits(n, k_mult_cjson, budget);
    const bool ok_yy = fits(n, k_mult_yyjson, budget);
    const bool ok_sj = fits(n, k_mult_simdjson, budget);
    const bool ok_rj = fits(n, k_mult_rapidjson, budget);
    const bool ok_gl = fits(n, k_mult_glaze, budget);
    const bool ok_bj = fits(n, k_mult_boost, budget);
    const bool ok_nl = fits(n, k_mult_nlohmann, budget);

    if ( !ok_cjson ) report_excluded("cjson", n, k_mult_cjson, budget);
    if ( !ok_yy ) report_excluded("yyjson", n, k_mult_yyjson, budget);
    if ( !ok_sj ) report_excluded("simdjson", n, k_mult_simdjson, budget);
    if ( !ok_rj ) report_excluded("rapidjson", n, k_mult_rapidjson, budget);
    if ( !ok_gl ) report_excluded("glaze", n, k_mult_glaze, budget);
    if ( !ok_bj ) report_excluded("boost.json", n, k_mult_boost, budget);
    if ( !ok_nl ) report_excluded("nlohmann", n, k_mult_nlohmann, budget);

    if ( do_parse ) {
      mb::row g[8];
      u32 k = 0;
      if ( ok_cjson ) {
        cjson::scratch sc;
        g[k++] = mb::bench_one("parse", "cjson", n, n, [&] { mb::sink_bool(cjson::parse(in, {}, sc).is_first()); }, cap);
      }
      if ( ok_cjson ) {
        static cjson::scratch warm;
        g[k++] = mb::bench_one("parse", "cjson-reuse", n, n, [&] { mb::sink_bool(cjson::parse_reuse(in, {}, warm).is_first()); }, cap);
      }
      if ( ok_yy )
        g[k++] = mb::bench_one(
            "parse", "yyjson", n, n,
            [&] {
              yyjson_doc *d = yyjson_read(buf, n, 0);
              mb::sink_bool(d != nullptr);
              if ( d ) yyjson_doc_free(d);
            },
            cap);
      if ( ok_sj ) g[k++] = mb::bench_one("parse", "simdjson-dom", n, n, [&] { mb::sink_bool(sj_dom_parse(sj_dom, buf, n) == 1); }, cap);
      if ( ok_rj ) g[k++] = mb::bench_one("parse", "rapidjson", n, n, [&] { mb::sink_bool(rj_parse(rj, buf, n) == 1); }, cap);
      if ( ok_gl ) g[k++] = mb::bench_one("parse", "glaze", n, n, [&] { mb::sink_bool(gl_parse(gl, buf, n) == 1); }, cap);
      if ( ok_bj ) g[k++] = mb::bench_one("parse", "boost.json", n, n, [&] { mb::sink_bool(bj_parse(bj, buf, n) == 1); }, cap);
      if ( ok_nl ) g[k++] = mb::bench_one("parse", "nlohmann", n, n, [&] { mb::sink_bool(nl_parse(nl, buf, n) == 1); }, cap);
      mb::print_group(g, k);
    }

    if ( do_extract && c.ptr[0] != '\0' && ok_cjson ) {
      static cjson::scratch od_sc;
      static cjson::scratch dom_sc;

      const long long want_v = cj_extract(in, c.ptr, od_sc);
      const long long got_dom = cj_extract_dom(in, c.ptr, dom_sc);
      const long long got_sj = ok_sj ? sj_od_extract(sj_od, buf, n, c.ptr) : want_v;
      const long long got_gl = ok_gl ? gl_extract(gl, buf, n, c.ptr) : want_v;
      const long long got_rj = ok_rj ? rj_extract(rj, buf, n, c.ptr) : want_v;
      const long long got_bj = ok_bj ? bj_extract(bj, buf, n, c.ptr) : want_v;
      const long long got_nl = ok_nl ? nl_extract(nl, buf, n, c.ptr) : want_v;

      auto agrees = [&](const char *who, long long v) {
        if ( v == want_v ) return true;
        micron::io::println("  !! ", who, " disagrees on ", c.label, c.ptr, ": got ", v, " want ", want_v, " — row dropped");
        return false;
      };
      const bool a_dom = agrees("cjson-dom", got_dom);
      const bool a_sj = ok_sj && agrees("simdjson-ondemand", got_sj);
      const bool a_gl = ok_gl && agrees("glaze-lazy", got_gl);
      const bool a_rj = ok_rj && agrees("rapidjson", got_rj);
      const bool a_bj = ok_bj && agrees("boost.json", got_bj);
      const bool a_nl = ok_nl && agrees("nlohmann", got_nl);

      {
        mb::row g[3];
        u32 k = 0;
        g[k++] = mb::bench_one("extract-lazy", "cjson-ondemand", n, n, [&] { mb::sink_size(usize(cj_extract(in, c.ptr, od_sc))); }, cap);
        if ( a_sj )
          g[k++] = mb::bench_one(
              "extract-lazy", "simdjson-ondemand", n, n, [&] { mb::sink_size(usize(sj_od_extract(sj_od, buf, n, c.ptr))); }, cap);
        if ( a_gl )
          g[k++] = mb::bench_one("extract-lazy", "glaze-lazy", n, n, [&] { mb::sink_size(usize(gl_extract(gl, buf, n, c.ptr))); }, cap);
        mb::print_group(g, k);
      }
      {
        mb::row g[5];
        u32 k = 0;
        if ( a_dom )
          g[k++] = mb::bench_one("extract-dom", "cjson-dom", n, n, [&] { mb::sink_size(usize(cj_extract_dom(in, c.ptr, dom_sc))); }, cap);
        if ( ok_yy )
          g[k++] = mb::bench_one(
              "extract-dom", "yyjson", n, n,
              [&] {
                yyjson_doc *d = yyjson_read(buf, n, 0);
                if ( d ) {
                  yyjson_val *v = yyjson_doc_ptr_get(d, c.ptr);
                  mb::sink_size(usize(yyjson_get_len(v)));
                  yyjson_doc_free(d);
                }
              },
              cap);
        if ( a_rj )
          g[k++] = mb::bench_one("extract-dom", "rapidjson", n, n, [&] { mb::sink_size(usize(rj_extract(rj, buf, n, c.ptr))); }, cap);
        if ( a_bj )
          g[k++] = mb::bench_one("extract-dom", "boost.json", n, n, [&] { mb::sink_size(usize(bj_extract(bj, buf, n, c.ptr))); }, cap);
        if ( a_nl )
          g[k++] = mb::bench_one("extract-dom", "nlohmann", n, n, [&] { mb::sink_size(usize(nl_extract(nl, buf, n, c.ptr))); }, cap);
        mb::print_group(g, k);
      }
    }

    if ( do_serialize && ok_cjson ) {
      auto rc = cjson::parse(in, cjson::opts{ .with_write_bound = true });
      yyjson_doc *yd = ok_yy ? yyjson_read(buf, n, 0) : nullptr;
      const bool nl_ok = ok_nl && nl_load(nl, buf, n) == 1;
      const bool rj_ok = ok_rj && rj_load(rj, buf, n) == 1;
      const bool bj_ok = ok_bj && bj_load(bj, buf, n) == 1;
      const bool gl_ok = ok_gl && gl_load(gl, buf, n) == 1;

      if ( rc.is_first() ) {
        const cjson::doc &cd = rc.cast<cjson::doc>();
        mb::row g[6];
        u32 k = 0;
        g[k++] = mb::bench_one(
            "serialize", "cjson", n, n,
            [&] {
              cjson::fjson o = cjson::write(cd);
              mb::sink_size(o.size());
            },
            cap);
        if ( yd )
          g[k++] = mb::bench_one(
              "serialize", "yyjson", n, n,
              [&] {
                usize len = 0;
                char *s = yyjson_write(yd, 0, &len);
                mb::sink_size(len);
                std::free(s);
              },
              cap);
        if ( rj_ok ) g[k++] = mb::bench_one("serialize", "rapidjson", n, n, [&] { mb::sink_size(usize(rj_serialize(rj))); }, cap);
        if ( gl_ok ) g[k++] = mb::bench_one("serialize", "glaze", n, n, [&] { mb::sink_size(usize(gl_serialize(gl))); }, cap);
        if ( bj_ok ) g[k++] = mb::bench_one("serialize", "boost.json", n, n, [&] { mb::sink_size(usize(bj_serialize(bj))); }, cap);
        if ( nl_ok ) g[k++] = mb::bench_one("serialize", "nlohmann", n, n, [&] { mb::sink_size(usize(nl_serialize(nl))); }, cap);
        mb::print_group(g, k);
      }
      if ( yd ) yyjson_doc_free(yd);
    }

    std::free(buf);
  }

  gl_free(gl);
  bj_free(bj);
  rj_free(rj);
  nl_free(nl);
  sj_od_parser_free(sj_od);
  sj_dom_parser_free(sj_dom);
  return 0;
}
