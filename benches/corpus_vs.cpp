//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// wide-net head-to-head: cjson vs yyjson, simdjson, rapidjson, nlohmann, boost.json and
// glaze, over a corpus chosen for SHAPE rather than size — float arrays, pure number
// arrays, escape-heavy strings, whitespace-heavy pretty text, deep nesting, geo
// polygons. The tracked sample/ corpus is all one shape, and a library tuned only
// against it is a library tuned against one workload.
//
// Three op groups per corpus:
//   parse      full dom materialization, every contender, copy mode
//   extract    one rfc 6901 pointer, however the library likes to reach it — the lazy
//              paths (cjson-ondemand, simdjson-ondemand, glaze-lazy) are doing
//              genuinely less work than the dom ones, so they print in their own group,
//              and inside that group GB/s divides by the bytes each contender actually
//              reads: glaze's walk stops at the pointer, cjson and simdjson index the
//              whole buffer. It is a scan rate, not a race — cyc/op is the race
//   serialize  dom -> minified text, off a document parsed once outside the timer
//
// Corpora absent from disk are skipped silently: run scripts/fetch_corpus to populate
// sample/web/ first. comparison tu — libc/stl allowed.
//   build: scripts/vsbuild benches/corpus_vs.cpp && taskset -c 0 ./bin/corpus_vs
//   ops:    ./bin/corpus_vs parse|extract|serialize        (default: all three)
//   corpus: ./bin/corpus_vs only=twitter,canada,numbers    (default: all present)
//   chart:  ./bin/corpus_vs > out.txt && scripts/chart_corpus out.txt

#include "_bench_common.hpp"

#include "../src/cjson/cjson.hpp"

#include <yyjson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

// every contender lives behind an extern "C" shim in comparison/: micron reimplements
// the x86 intrinsics, so micron headers and <immintrin.h>-world code cannot share a tu
extern "C" {
// simdjson
void *sj_dom_parser_new();
void sj_dom_parser_free(void *);
int sj_dom_parse(void *, const char *, unsigned long);
void *sj_od_parser_new();
void sj_od_parser_free(void *);
long long sj_od_extract(void *, const char *, unsigned long, const char *);
long long sj_minify(const char *, unsigned long, char *);
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
long long gl_validate(const char *, unsigned long);
long long gl_extract(void *, const char *, unsigned long, const char *);
long long gl_extract_reach(void *, const char *, unsigned long, const char *);
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
  // tracked corpus (always present)
  { "sample/64kb.json", "64kb", "" },
  { "sample/twitter.json", "twitter", "/statuses/0/user/screen_name" },
  { "sample/1MB.json", "1MB", "" },
  { "sample/5MB.json", "5MB", "" },
  { "sample/large-file.json", "large-file", "" },
  // fetched corpus (scripts/fetch_corpus)
  { "sample/web/github_events.json", "github_events", "/0/actor/login" },
  { "sample/web/numbers.json", "numbers", "/5000" },
  { "sample/web/instruments.json", "instruments", "/name" },
  { "sample/web/countries.geo.json", "countries.geo", "/features/0/properties/name" },
  { "sample/web/update-center.json", "update-center", "/core/version" },
  { "sample/web/twitterescaped.json", "twitterescaped", "/statuses/0/user/screen_name" },
  { "sample/web/mesh.json", "mesh", "/positions/100" },
  { "sample/web/citm_catalog.json", "citm_catalog", "/areaNames/205705994" },
  { "sample/web/mesh.pretty.json", "mesh.pretty", "/positions/100" },
  { "sample/web/canada.json", "canada", "/features/0/properties/name" },
  { "sample/web/marine_ik.json", "marine_ik", "/metadata/version" },
  { "sample/web/gsoc-2018.json", "gsoc-2018", "/0/name" },
  { "sample/web/semanticscholar-corpus.json", "semanticscholar", "/0/magId" }
  //{ "sample/web/api.github.com.json", "gh-openapi", "/info/version" }
  //{ "sample/web/citylots.json", "citylots", "/features/0/properties/BLOCK_NUM" }
  // generated by scripts/fetch_corpus --gen-1g; absent unless you ask for it
  //{ "sample/web/1GB.json", "1GB", "/0/meta/rev" }, MOVED TO BENCHES HUGE
};

constexpr usize k_dom_mem_limit = 512u << 20;

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
  if ( n > (4u << 20) ) return 16;
  return mb::MAX_REPS;
}

bool
want(int argc, char **argv, const char *op)
{
  bool any_op = false;
  for ( int i = 1; i < argc; i++ ) {
    if ( std::strncmp(argv[i], "only=", 5) == 0 ) continue;
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

};      // namespace

int
main(int argc, char **argv)
{
  mb::pin_cpu0();
  mb::print_header();

  const bool do_parse = want(argc, argv, "parse");
  const bool do_extract = want(argc, argv, "extract");
  const bool do_serialize = want(argc, argv, "serialize");

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

    const bool mem_capped = n > k_dom_mem_limit;
    if ( mem_capped && do_parse )
      micron::io::println("    (", n >> 20, " MB > ", k_dom_mem_limit >> 20,
                          " MB dom budget: nlohmann, boost.json, rapidjson and glaze excluded — "
                          "their dom would exceed this box's RAM)");

    if ( do_parse ) {
      mb::row g[8];
      u32 k = 0;
      {
        cjson::scratch sc;
        g[k++] = mb::bench_one("parse", "cjson", n, n, [&] { mb::sink_bool(cjson::parse(in, {}, sc).is_first()); }, cap);
      }
      {
        static cjson::scratch warm;
        g[k++] = mb::bench_one("parse", "cjson-reuse", n, n, [&] { mb::sink_bool(cjson::parse_reuse(in, {}, warm).is_first()); }, cap);
      }
      g[k++] = mb::bench_one(
          "parse", "yyjson", n, n,
          [&] {
            yyjson_doc *d = yyjson_read(buf, n, 0);
            mb::sink_bool(d != nullptr);
            if ( d ) yyjson_doc_free(d);
          },
          cap);
      g[k++] = mb::bench_one("parse", "simdjson-dom", n, n, [&] { mb::sink_bool(sj_dom_parse(sj_dom, buf, n) == 1); }, cap);
      if ( !mem_capped ) {
        g[k++] = mb::bench_one("parse", "rapidjson", n, n, [&] { mb::sink_bool(rj_parse(rj, buf, n) == 1); }, cap);
        g[k++] = mb::bench_one("parse", "glaze", n, n, [&] { mb::sink_bool(gl_parse(gl, buf, n) == 1); }, cap);
        g[k++] = mb::bench_one("parse", "boost.json", n, n, [&] { mb::sink_bool(bj_parse(bj, buf, n) == 1); }, cap);
        g[k++] = mb::bench_one("parse", "nlohmann", n, n, [&] { mb::sink_bool(nl_parse(nl, buf, n) == 1); }, cap);
      }
      mb::print_group(g, k);
    }

    if ( do_extract && c.ptr[0] != '\0' ) {
      static cjson::scratch od_sc;
      static cjson::scratch dom_sc;

      const long long want = cj_extract(in, c.ptr, od_sc);
      const long long got_od = cj_extract_dom(in, c.ptr, dom_sc);
      const long long got_sj = sj_od_extract(sj_od, buf, n, c.ptr);
      const long long got_gl = gl_extract(gl, buf, n, c.ptr);
      const long long got_rj = rj_extract(rj, buf, n, c.ptr);
      const long long got_bj = bj_extract(bj, buf, n, c.ptr);
      const long long got_nl = nl_extract(nl, buf, n, c.ptr);

      auto agrees = [&](const char *who, long long v) {
        if ( v == want ) return true;
        micron::io::println("  !! ", who, " disagrees on ", c.label, c.ptr, ": got ", v, " want ", want, " — row dropped");
        return false;
      };
      const bool ok_dom = agrees("cjson-dom", got_od);
      const bool ok_sj = agrees("simdjson-ondemand", got_sj);
      const bool ok_gl = agrees("glaze-lazy", got_gl);
      const bool ok_rj = agrees("rapidjson", got_rj);
      const bool ok_bj = agrees("boost.json", got_bj);
      const bool ok_nl = agrees("nlohmann", got_nl);

      {
        // GB/s denominated PER CONTENDER in the bytes that contender actually reads,
        // because on this group they genuinely do not read the same thing. cjson's
        // iterate and simdjson's stage 1 both index the whole buffer, so n is what they
        // read. glaze's walk stops at the value the pointer names — charging it n
        // credited it a document it never touched, which is how a 636 cyc/op row over
        // 8.6 MB came out as "3841.68 GB/s". The column is a scan rate; cyc/op is the
        // one that answers which library resolves the query first.
        const u64 gl_bytes = ok_gl ? u64(gl_extract_reach(gl, buf, n, c.ptr)) : 0;

        mb::row g[3];
        u32 k = 0;
        g[k++] = mb::bench_one("extract-lazy", "cjson-ondemand", n, n, [&] { mb::sink_size(usize(cj_extract(in, c.ptr, od_sc))); }, cap);
        if ( ok_sj )
          g[k++] = mb::bench_one(
              "extract-lazy", "simdjson-ondemand", n, n, [&] { mb::sink_size(usize(sj_od_extract(sj_od, buf, n, c.ptr))); }, cap);
        if ( ok_gl )
          // reps_cap on the BYTES READ, not on n: the cap exists to stop the harness
          // spending forever on an op that costs O(document), and glaze's walk does not.
          // Capping it by n pinned semanticscholar's 644 cyc/op row to 16 reps, few
          // enough that the perf ioctls around the loop outweighed the loop.
          g[k++] = mb::bench_one(
              "extract-lazy", "glaze-lazy", n, gl_bytes ? gl_bytes : n, [&] { mb::sink_size(usize(gl_extract(gl, buf, n, c.ptr))); },
              gl_bytes ? reps_cap(gl_bytes) : cap);
        mb::print_group(g, k);
      }

      {
        mb::row g[5];
        u32 k = 0;
        if ( ok_dom )
          g[k++] = mb::bench_one("extract-dom", "cjson-dom", n, n, [&] { mb::sink_size(usize(cj_extract_dom(in, c.ptr, dom_sc))); }, cap);
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
        if ( ok_rj )
          g[k++] = mb::bench_one("extract-dom", "rapidjson", n, n, [&] { mb::sink_size(usize(rj_extract(rj, buf, n, c.ptr))); }, cap);
        if ( ok_bj )
          g[k++] = mb::bench_one("extract-dom", "boost.json", n, n, [&] { mb::sink_size(usize(bj_extract(bj, buf, n, c.ptr))); }, cap);
        if ( ok_nl )
          g[k++] = mb::bench_one("extract-dom", "nlohmann", n, n, [&] { mb::sink_size(usize(nl_extract(nl, buf, n, c.ptr))); }, cap);
        mb::print_group(g, k);
      }
    }

    if ( do_serialize && !mem_capped ) {

      auto rc = cjson::parse(in, cjson::opts{ .with_write_bound = true });
      yyjson_doc *yd = yyjson_read(buf, n, 0);
      const bool nl_ok = nl_load(nl, buf, n) == 1;
      const bool rj_ok = rj_load(rj, buf, n) == 1;
      const bool bj_ok = bj_load(bj, buf, n) == 1;
      const bool gl_ok = gl_load(gl, buf, n) == 1;

      if ( rc.is_first() && yd ) {
        const cjson::doc &cd = rc.cast<cjson::doc>();
        // GB/s denominated in EMITTED bytes, uniformly for every contender: they all
        // produce the same minified text, so the ratios are untouched and the absolute
        // number finally means "serialization throughput" rather than "input bytes/s"
        const usize ob = cjson::write(cd).size();
        mb::row g[7];
        u32 k = 0;
        g[k++] = mb::bench_one(
            "serialize", "cjson", n, ob,
            [&] {
              cjson::fjson o = cjson::write(cd);
              mb::sink_size(o.size());
            },
            cap);
        // apples-to-apples with glaze/rapidjson/boost/nlohmann, which all serialize into a
        // retained buffer. the row above pays a fresh mmap + first-touch fault storm per
        // rep; this one does not, and that difference is ~2.2x of wall clock on 5MB
        {
          static cjson::wbuf warm;
          g[k++] = mb::bench_one("serialize", "cjson-reuse", n, ob, [&] { mb::sink_size(usize(cjson::write_into(cd, warm))); }, cap);
        }
        g[k++] = mb::bench_one(
            "serialize", "yyjson", n, ob,
            [&] {
              usize len = 0;
              char *s = yyjson_write(yd, 0, &len);
              mb::sink_size(len);
              std::free(s);
            },
            cap);
        if ( rj_ok ) g[k++] = mb::bench_one("serialize", "rapidjson", n, ob, [&] { mb::sink_size(usize(rj_serialize(rj))); }, cap);
        if ( gl_ok ) g[k++] = mb::bench_one("serialize", "glaze", n, ob, [&] { mb::sink_size(usize(gl_serialize(gl))); }, cap);
        if ( bj_ok ) g[k++] = mb::bench_one("serialize", "boost.json", n, ob, [&] { mb::sink_size(usize(bj_serialize(bj))); }, cap);
        if ( nl_ok ) g[k++] = mb::bench_one("serialize", "nlohmann", n, ob, [&] { mb::sink_size(usize(nl_serialize(nl))); }, cap);
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
