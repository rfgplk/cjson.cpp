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
// INDEX vs TREE: the split that runs through every group here.
//
// "parse a document" is two different jobs and the numbers do not mix.
//
//   * An INDEX (tape) builder — cjson, simdjson-dom, yyjson — emits a flat array of
//     fixed-size slots over the caller's bytes. Strings are not materialized: cjson
//     stores {offset, length} and only unescapes on demand. Objects get no key index;
//     lookup is a linear scan at access time. Teardown is one free.
//   * A TREE builder — glz::generic, rapidjson, boost.json, nlohmann — returns an owning,
//     mutable node graph. Every string value and every key is its own std::string; every
//     array is a vector that reallocs as it grows; every object is a map that builds a
//     lookup index. Teardown is a recursive destructor walk.
//
// The tree band genuinely retires several times the instructions of the index band, and
// on a 26 MB document that is ~6x. That difference is the representation the caller asked
// for, not the speed of the parser that built it. Printing all eight in one ranked list
// implied an ordering the data does not support — the two bands are separated by a gap
// with nothing in it — so every group below is split and print_group is never handed rows
// from both. Within a band the comparison is like-for-like and the ranking is real.
//
// glz::generic is named in full for the same reason: it is glaze's schema-less fallback
// tree, not its reflected fast path, and the row should say which one it is. glaze's
// lazy apis are measured separately in the extract group and in benches/lazy_vs.cpp.
//
// Op groups per corpus:
//   parse/index      tape materialization  — cjson, cjson-reuse, yyjson, simdjson-dom
//   parse/tree       owning tree           — rapidjson, glz::generic, boost.json, nlohmann
//   extract-lazy     one rfc 6901 pointer, walked without building anything (cjson-ondemand,
//                    simdjson-ondemand, glaze-lazy, glaze-lazyjson). GB/s divides by the
//                    bytes each contender actually reads: glaze's walk stops at the
//                    pointer, cjson and simdjson index the whole buffer. It is a scan
//                    rate, not a race — cyc/op is the race
//   extract-dom/*    same pointer, but reached by building first — split index vs tree
//   serialize/*      dom -> minified text off a document parsed once outside the timer,
//                    split index vs tree
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
long long gl_lazy_extract(void *, const char *, unsigned long, const char *);
long long gl_lazy_extract_reach(void *, const char *, unsigned long, const char *);
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
                          " MB dom budget: the whole parse/tree band is excluded — an owning "
                          "node tree over this document would exceed this box's RAM)");

    // TWO groups, never one — see the banner comment at the top of this file.
    if ( do_parse ) {
      {
        mb::row g[4];
        u32 k = 0;
        {
          cjson::scratch sc;
          g[k++] = mb::bench_one("parse/index", "cjson", n, n, [&] { mb::sink_bool(cjson::parse(in, {}, sc).is_first()); }, cap);
        }
        {
          static cjson::scratch warm;
          g[k++]
              = mb::bench_one("parse/index", "cjson-reuse", n, n, [&] { mb::sink_bool(cjson::parse_reuse(in, {}, warm).is_first()); }, cap);
        }
        g[k++] = mb::bench_one(
            "parse/index", "yyjson", n, n,
            [&] {
              yyjson_doc *d = yyjson_read(buf, n, 0);
              mb::sink_bool(d != nullptr);
              if ( d ) yyjson_doc_free(d);
            },
            cap);
        g[k++] = mb::bench_one("parse/index", "simdjson-dom(warm)", n, n, [&] { mb::sink_bool(sj_dom_parse(sj_dom, buf, n) == 1); }, cap);
        mb::print_group(g, k);
      }
      // no cjson row here on purpose: cjson does not build an owning tree, so there is no
      // like-for-like cjson number to put beside these and print_group prints no ratio
      if ( !mem_capped ) {
        mb::row g[4];
        u32 k = 0;
        g[k++] = mb::bench_one("parse/tree", "rapidjson", n, n, [&] { mb::sink_bool(rj_parse(rj, buf, n) == 1); }, cap);
        g[k++] = mb::bench_one("parse/tree", "glz::generic", n, n, [&] { mb::sink_bool(gl_parse(gl, buf, n) == 1); }, cap);
        g[k++] = mb::bench_one("parse/tree", "boost.json", n, n, [&] { mb::sink_bool(bj_parse(bj, buf, n) == 1); }, cap);
        g[k++] = mb::bench_one("parse/tree", "nlohmann", n, n, [&] { mb::sink_bool(nl_parse(nl, buf, n) == 1); }, cap);
        mb::print_group(g, k);
      }
    }

    if ( do_extract && c.ptr[0] != '\0' ) {
      static cjson::scratch od_sc;
      static cjson::scratch dom_sc;

      const long long want = cj_extract(in, c.ptr, od_sc);
      const long long got_od = cj_extract_dom(in, c.ptr, dom_sc);
      const long long got_sj = sj_od_extract(sj_od, buf, n, c.ptr);
      const long long got_gl = gl_extract(gl, buf, n, c.ptr);
      const long long got_gl_lz = gl_lazy_extract(gl, buf, n, c.ptr);
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
      const bool ok_gl_lz = agrees("glaze-lazyjson", got_gl_lz);
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
        const u64 gl_lz_bytes = ok_gl_lz ? u64(gl_lazy_extract_reach(gl, buf, n, c.ptr)) : 0;

        mb::row g[4];
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
        if ( ok_gl_lz )
          // glz::lazy_json, which is NOT what the row above measures: get_view_json is
          // stateless, lazy_json hands back a document whose views carry a cursor. On
          // ONE pointer the two do the same walk and this row exists to show that they
          // cost the same; the cursor only pays for itself across several reads of one
          // buffer, which is what benches/lazy_vs.cpp sweeps.
          g[k++] = mb::bench_one(
              "extract-lazy", "glaze-lazyjson", n, gl_lz_bytes ? gl_lz_bytes : n,
              [&] { mb::sink_size(usize(gl_lazy_extract(gl, buf, n, c.ptr))); }, gl_lz_bytes ? reps_cap(gl_lz_bytes) : cap);
        mb::print_group(g, k);
      }

      // build-then-reach, split the same way as parse: the build dominates this row, so
      // the index/tree gap dominates it too
      {
        mb::row g[2];
        u32 k = 0;
        if ( ok_dom )
          g[k++] = mb::bench_one(
              "extract-dom/index", "cjson-dom", n, n, [&] { mb::sink_size(usize(cj_extract_dom(in, c.ptr, dom_sc))); }, cap);
        g[k++] = mb::bench_one(
            "extract-dom/index", "yyjson", n, n,
            [&] {
              yyjson_doc *d = yyjson_read(buf, n, 0);
              if ( d ) {
                yyjson_val *v = yyjson_doc_ptr_get(d, c.ptr);
                mb::sink_size(usize(yyjson_get_len(v)));
                yyjson_doc_free(d);
              }
            },
            cap);
        mb::print_group(g, k);
      }
      {
        mb::row g[3];
        u32 k = 0;
        if ( ok_rj )
          g[k++] = mb::bench_one("extract-dom/tree", "rapidjson", n, n, [&] { mb::sink_size(usize(rj_extract(rj, buf, n, c.ptr))); }, cap);
        if ( ok_bj )
          g[k++] = mb::bench_one("extract-dom/tree", "boost.json", n, n, [&] { mb::sink_size(usize(bj_extract(bj, buf, n, c.ptr))); }, cap);
        if ( ok_nl )
          g[k++] = mb::bench_one("extract-dom/tree", "nlohmann", n, n, [&] { mb::sink_size(usize(nl_extract(nl, buf, n, c.ptr))); }, cap);
        if ( k ) mb::print_group(g, k);
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
        // emitting from a tape and emitting from a node tree are the same split as parse:
        // one is a straight-line walk of contiguous slots, the other chases owned nodes
        {
          mb::row g[3];
          u32 k = 0;
          g[k++] = mb::bench_one(
              "serialize/index", "cjson", n, ob,
              [&] {
                cjson::fjson o = cjson::write(cd);
                mb::sink_size(o.size());
              },
              cap);
          // the row above pays a fresh mmap + first-touch fault storm per rep; this one
          // does not, and that difference is ~2.2x of wall clock on 5MB
          {
            static cjson::wbuf warm;
            g[k++]
                = mb::bench_one("serialize/index", "cjson-reuse", n, ob, [&] { mb::sink_size(usize(cjson::write_into(cd, warm))); }, cap);
          }
          g[k++] = mb::bench_one(
              "serialize/index", "yyjson", n, ob,
              [&] {
                usize len = 0;
                char *s = yyjson_write(yd, 0, &len);
                mb::sink_size(len);
                std::free(s);
              },
              cap);
          mb::print_group(g, k);
        }
        // buffer conventions inside this band are NOT uniform and the labels say so.
        // rapidjson (sb.Clear()), glz::generic (out.clear()) and boost.json (serializer
        // into a retained string) all reuse their output buffer. nlohmann's dump() returns
        // a fresh std::string and it has no public api that writes into a caller's buffer,
        // so its row allocates once per rep — that is nlohmann's fastest public spelling,
        // not a handicap imposed here.
        {
          mb::row g[4];
          u32 k = 0;
          if ( rj_ok ) g[k++] = mb::bench_one("serialize/tree", "rapidjson", n, ob, [&] { mb::sink_size(usize(rj_serialize(rj))); }, cap);
          if ( gl_ok )
            g[k++] = mb::bench_one("serialize/tree", "glz::generic", n, ob, [&] { mb::sink_size(usize(gl_serialize(gl))); }, cap);
          if ( bj_ok ) g[k++] = mb::bench_one("serialize/tree", "boost.json", n, ob, [&] { mb::sink_size(usize(bj_serialize(bj))); }, cap);
          if ( nl_ok )
            g[k++] = mb::bench_one("serialize/tree", "nlohmann(alloc)", n, ob, [&] { mb::sink_size(usize(nl_serialize(nl))); }, cap);
          if ( k ) mb::print_group(g, k);
        }
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
