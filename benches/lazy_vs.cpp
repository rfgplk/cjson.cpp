//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// lazy extraction head-to-head, swept by FIELDS PER DOCUMENT: cjson on-demand vs
// glz::lazy_json vs glz::get_view_json vs simdjson on-demand vs yyjson.
// comparison tu — libc allowed.
//   build: scripts/vsbuild benches/lazy_vs.cpp && taskset -c 0 ./bin/lazy_vs
//
// WHY THIS BENCH EXISTS. corpus_vs's extract-lazy group resolves ONE pointer per
// document, and on that workload a lazy walk beats an index by two orders of magnitude
// for a reason that has nothing to do with either implementation's quality: cjson and
// simdjson index the whole buffer before walking it, glaze's walk stops at the value the
// pointer names. ARCHITECTURE.md says so and says where the real question lives —
//
//   "cjson's index is reusable across many reads on the same buffer and glaze's walk is
//    not, so the crossover moves with the number of fields extracted."
//
// — and then nothing measures it. This does. Each x position is N fields resolved from
// ONE document handle, so the index is paid once and amortized while the walks are paid
// N times. The deliverable is the crossover N, not any single row.
//
// TWO AXES, because the answer is not one number:
//   head    the N fields come from ceil(N/4) adjacent records at the front — short
//           walks, four reads per held cursor, glaze's best case, and the shape of
//           "pull a record's fields out of a payload"
//   spread  one field from each of N records strided across all 100 statuses — the
//           walks get long, nothing is reusable between them, the index does not care,
//           and this is the shape of a real scan
//
// glz::lazy_json is NOT what corpus_vs's `glaze-lazy` row measures (that is
// get_view_json, stateless, restarts at byte 0 every call). lazy_json hands back a
// document whose views carry a parse_pos_ cursor, so it is the one glaze api that can
// amortize; comparison/glaze_shim.cpp holds the shared path across pointers so the row
// is glaze's best form and not a strawman. `glaze-getview` is kept beside it as the
// stateless control — the margin between those two rows IS the value of the cursor.
//
// EVERY CONTENDER IS CROSS-CHECKED before it is timed. A pointer walk that quietly
// misses posts a spectacular number for doing nothing, which is how a benchmark becomes
// a rumour; a row whose checksum disagrees with cjson's is named and dropped.

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
void *sj_od_parser_new();
void sj_od_parser_free(void *);
long long sj_od_nfields(void *, const char *, unsigned long, const char *const *, int);
// glaze
void *gl_new();
void gl_free(void *);
long long gl_lazy_nfields(void *, const char *, unsigned long, const char *const *, int);
long long gl_lazy_nfields_reach(void *, const char *, unsigned long, const char *const *, int);
long long gl_getview_nfields(void *, const char *, unsigned long, const char *const *, int);
long long gl_getview_nfields_reach(void *, const char *, unsigned long, const char *const *, int);
};

namespace
{

constexpr const char *k_path = "sample/twitter.json";
constexpr const char *k_label = "twitter";
constexpr u32 k_statuses = 100;      // sample/twitter.json's /statuses length

// The four fields read from each status, IN DOCUMENT ORDER — id sits at key 3, user at
// 13, then retweet_count and favorite_count. Order matters: a progressive cursor that is
// asked to go backwards pays a wrap-around pass, and the point here is to measure each
// library's intended use, not to trip it.
//
// ALL FOUR ARE NUMBERS, deliberately. cjson's checksum takes a string's RAW span while
// glaze's takes its DECODED length, so a single escaped string would make the contenders
// disagree for a reason that has nothing to do with speed and silently drop the row.
// Numbers remove that whole class of failure.
constexpr const char *k_field[4] = { "id", "user/followers_count", "retweet_count", "favorite_count" };

constexpr u32 k_ns[] = { 1, 2, 4, 8, 16, 32, 64 };
constexpr u32 k_max_n = 64;
constexpr u32 k_ptr_max = 64;      // "/statuses/99/user/followers_count" + slack

// one wanted field, resolved structurally rather than by pointer text
struct sel {
  u32 k;
  u32 f;
};

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
  // 64 bytes of zero past the end: simdjson's padded_string_view requires the slack, and
  // glaze's default opts are null_terminated, so its scan loops want the sentinel too
  for ( usize i = 0; i < 64; i++ ) buf[n + i] = 0;
  std::fclose(f);
  return buf;
}

// Build the N pointers for one (count, layout) cell.
//
//   head    all four fields of status 0, then all four of status 1, … — N fields drawn
//           from ceil(N/4) adjacent records at the front. Short walks, and a held cursor
//           can serve four reads before it has to move.
//   spread  one field per status, strided so the N records cover the array — the walks
//           get long and nothing can be reused between them.
//
// The contrast is the point: head is where an amortizing cursor should win and spread is
// where an index should. Both orders are ascending in status and, within a status,
// follow document key order, so a forward-only reader is never asked to go backwards.
// At N=1 the two coincide by construction, a free identity check on the generator.
void
build(u32 count, bool spread, sel *sels, char (*bufs)[k_ptr_max], const char **ptrs)
{
  const u32 span = count < k_statuses ? count : k_statuses;
  const u32 stride = spread ? (k_statuses / span) : 1;
  for ( u32 i = 0; i < count; i++ ) {
    const u32 f = i % 4;
    const u32 k = spread ? i * stride : i / 4;
    sels[i] = sel{ k, f };
    std::snprintf(bufs[i], k_ptr_max, "/statuses/%u/%s", k, k_field[f]);
    ptrs[i] = bufs[i];
  }
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// cjson

// numbers by value, matching every shim's convention. these four fields are all numbers,
// so this is the whole checksum the bench needs.
long long
cj_num(cjson::cur c)
{
  return static_cast<long long>(c.f64_or(0));
}

// N pointers from the root, the way anyone reaching for at_pointer writes it: stage 1
// once, then a fresh walk from the root per pointer.
long long
cj_nfields(cjson::bytes in, const char *const *ptrs, u32 count, cjson::scratch &sc)
{
  auto rv = cjson::iterate(in, sc);
  if ( rv.is_second() ) return 0;
  const cjson::view v = rv.cast<cjson::view>();
  const cjson::cur root = v.root();
  long long sum = 0;
  for ( u32 i = 0; i < count; i++ ) sum += cj_num(root.at_pointer(ptrs[i]));
  return sum;
}

long long
cj_field(cjson::cur st, u32 f)
{
  switch ( f ) {
  case 0:
    return cj_num(st["id"]);
  case 1:
    return cj_num(st["user"]["followers_count"]);
  case 2:
    return cj_num(st["retweet_count"]);
  default:
    return cj_num(st["favorite_count"]);
  }
}

// The same N fields, read the way the cursor api wants to be used: ONE forward pass over
// statuses with the element cursor held, instead of re-walking from the root per
// pointer. This row exists for symmetry of effort — glaze is given its held-view best
// form in the shim, so cjson has to be given its best form too or the bench is rigged by
// omission. (It is also the fix for the at(i) trap: at(i) restarts from the '[' every
// call, so indexing a loop by it is quadratic.)
long long
cj_nfields_cursor(cjson::bytes in, const sel *sels, u32 count, cjson::scratch &sc)
{
  auto rv = cjson::iterate(in, sc);
  if ( rv.is_second() ) return 0;
  const cjson::view v = rv.cast<cjson::view>();
  long long sum = 0;
  u32 i = 0;
  u32 k = 0;
  for ( auto st : v.root()["statuses"].items() ) {
    if ( i >= count ) break;
    while ( i < count and sels[i].k == k ) sum += cj_field(st, sels[i++].f);
    ++k;
  }
  return sum;
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// yyjson — full dom parse, then N pointer gets. included directly; it is plain c and
// does not collide with micron's intrinsics, so it needs no shim.

long long
yy_nfields(const char *buf, usize n, const char *const *ptrs, u32 count)
{
  yyjson_doc *d = yyjson_read(buf, n, 0);
  if ( !d ) return 0;
  long long sum = 0;
  for ( u32 i = 0; i < count; i++ ) sum += static_cast<long long>(yyjson_get_num(yyjson_doc_ptr_get(d, ptrs[i])));
  yyjson_doc_free(d);
  return sum;
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%

u64
reps_cap(usize n)
{
  if ( n > (512u << 20) ) return 1;
  if ( n > (16u << 20) ) return 4;
  if ( n > (4u << 20) ) return 16;
  return mb::MAX_REPS;
}

bool
want_n(int argc, char **argv, u32 n)
{
  bool any = false;
  for ( int i = 1; i < argc; i++ ) {
    if ( std::strncmp(argv[i], "n=", 2) != 0 ) continue;
    any = true;
    const char *list = argv[i] + 2;
    for ( const char *p = list; *p; ) {
      u32 v = 0;
      while ( *p >= '0' and *p <= '9' ) v = v * 10 + u32(*p++ - '0');
      if ( v == n ) return true;
      while ( *p and *p != ',' ) ++p;
      if ( *p == ',' ) ++p;
    }
  }
  return !any;
}

};      // namespace

int
main(int argc, char **argv)
{
  mb::pin_cpu0();
  mb::print_header();

  usize n = 0;
  char *buf = slurp(k_path, n);
  if ( !buf ) {
    micron::io::println("  !! ", k_path, " not found — nothing to do");
    return 0;
  }
  const cjson::bytes in{ reinterpret_cast<const u8 *>(buf), n };
  const u64 cap = reps_cap(n);

  if ( cjson::validate(in) != cjson::error::ok ) {
    micron::io::println("  !! ", k_label, " does not validate — skipped");
    std::free(buf);
    return 0;
  }

  micron::io::println("### ", k_label, "  (", n, " B)");
  micron::io::println("    N fields resolved from ONE document handle.");
  micron::io::println("    head   = all 4 fields of each of ceil(N/4) adjacent records at the front");
  micron::io::println("    spread = 1 field from each of N records strided across all ", k_statuses, " statuses");
  micron::io::println("    cyc/op is the ranking column — GB/s here is a per-contender SCAN RATE, since");
  micron::io::println("    the lazy walks and the indexers do not read the same bytes.");

  void *sj = sj_od_parser_new();
  void *gl = gl_new();

  static cjson::scratch od_sc;
  static cjson::scratch cur_sc;

  sel sels[k_max_n];
  char bufs[k_max_n][k_ptr_max];
  const char *ptrs[k_max_n];

  for ( u32 layout = 0; layout < 2; layout++ ) {
    const bool spread = layout == 1;
    for ( const u32 count : k_ns ) {
      if ( !want_n(argc, argv, count) ) continue;
      build(count, spread, sels, bufs, ptrs);

      char op[24];
      std::snprintf(op, sizeof(op), "n%u-%s", count, spread ? "spread" : "head");

      // %%%%%%%%% cross-check, untimed: every contender must agree with cjson
      const long long want = cj_nfields(in, ptrs, count, od_sc);
      auto agrees = [&](const char *who, long long v) {
        if ( v == want ) return true;
        micron::io::println("  !! ", who, " disagrees on ", op, ": got ", v, " want ", want, " — row dropped");
        return false;
      };
      const bool ok_cur = agrees("cjson-cursor", cj_nfields_cursor(in, sels, count, cur_sc));
      const bool ok_gl = agrees("glaze-lazyjson", gl_lazy_nfields(gl, buf, n, ptrs, int(count)));
      const bool ok_gv = agrees("glaze-getview", gl_getview_nfields(gl, buf, n, ptrs, int(count)));
      const bool ok_sj = agrees("simdjson-ondemand", sj_od_nfields(sj, buf, n, ptrs, int(count)));
      const bool ok_yy = agrees("yyjson", yy_nfields(buf, n, ptrs, count));

      // GB/s denominated PER CONTENDER in the bytes that contender actually reads, and
      // reps_cap taken on that SAME number rather than on n. Both are settled bugs:
      // charging a lazy walk the whole document once printed a 636 cyc/op row over
      // 8.6 MB as "3841.68 GB/s", and capping reps by n pinned a 644 cyc/op row to 16
      // reps, few enough that the perf ioctls bracketing the loop outweighed the loop.
      // cjson and simdjson index the whole buffer, so n is what they read; yyjson parses
      // it. The glaze rows read only as far as their furthest walk.
      const u64 gl_bytes = ok_gl ? u64(gl_lazy_nfields_reach(gl, buf, n, ptrs, int(count))) : 0;
      const u64 gv_bytes = ok_gv ? u64(gl_getview_nfields_reach(gl, buf, n, ptrs, int(count))) : 0;

      mb::row g[8];
      u32 k = 0;
      g[k++] = mb::bench_one(op, "cjson-ondemand", n, n, [&] { mb::sink_size(usize(cj_nfields(in, ptrs, count, od_sc))); }, cap);
      if ( ok_cur )
        g[k++] = mb::bench_one(op, "cjson-cursor", n, n, [&] { mb::sink_size(usize(cj_nfields_cursor(in, sels, count, cur_sc))); }, cap);
      if ( ok_gl )
        g[k++] = mb::bench_one(
            op, "glaze-lazyjson", n, gl_bytes ? gl_bytes : n, [&] { mb::sink_size(usize(gl_lazy_nfields(gl, buf, n, ptrs, int(count)))); },
            gl_bytes ? reps_cap(gl_bytes) : cap);
      if ( ok_gv )
        g[k++] = mb::bench_one(
            op, "glaze-getview", n, gv_bytes ? gv_bytes : n,
            [&] { mb::sink_size(usize(gl_getview_nfields(gl, buf, n, ptrs, int(count)))); }, gv_bytes ? reps_cap(gv_bytes) : cap);
      if ( ok_sj )
        g[k++]
            = mb::bench_one(op, "simdjson-ondemand", n, n, [&] { mb::sink_size(usize(sj_od_nfields(sj, buf, n, ptrs, int(count)))); }, cap);
      if ( ok_yy ) g[k++] = mb::bench_one(op, "yyjson", n, n, [&] { mb::sink_size(usize(yy_nfields(buf, n, ptrs, count))); }, cap);
      mb::print_group(g, k);
    }
  }

  gl_free(gl);
  sj_od_parser_free(sj);
  std::free(buf);
  return 0;
}
