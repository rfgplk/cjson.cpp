//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// glaze isolation shim (vendored from ~/Git/glaze by scripts/vsbuild into
// comparison/.glaze-vendor; the reference checkout stays untouched).
//
// REQUIRED, not optional: glaze uses <immintrin.h> in glaze/simd/simd.hpp and
// glaze/util/zmij.hpp, the same collision with micron's reimplemented intrinsics that
// forces the simdjson shim. Built at -std=c++23 (the plain path — glaze's optional
// P2996 reflection backend needs -freflection and buys nothing for an untyped dom).
//
// The generic value type is glz::generic; glz::json_t is a deprecated alias
// (glaze/json/generic_fwd.hpp:565) and duck's -w turns warnings up rather than down.
//
// NOTE on the extract row: glaze is the only contender besides cjson and simdjson with
// a genuinely lazy path — glz::get_view_json walks the raw buffer to a json pointer
// without building a dom. That is what its extract row uses, because that is what
// anyone reaching for glaze on a hot path would use. Its parse and serialize rows are
// full-dom, like everyone else's.
//
// Which is why gl_extract_reach exists: the walk stops at the value the pointer names
// and never sees the rest of the buffer, so the extract-lazy row has to be denominated
// in the bytes it actually read, not in the document it was handed.
//
// NOTE on the gl_lazy_* rows: those are glz::lazy_json, which is NOT what gl_extract
// uses. get_view_json is stateless — every call restarts at byte 0 and keeps nothing.
// glz::lazy_json hands back a lazy_document whose views carry a parse_pos_ cursor, so a
// second key lookup on the SAME view resumes where the first stopped instead of
// rescanning the object (glaze/json/lazy.hpp:1000-1110, with a wrap-around pass when the
// key sits behind the cursor). That makes it the first glaze api that can amortize
// across several reads of one buffer, which is the axis the cjson/glaze comparison
// actually turns on — cjson pays stage 1 once and reuses the index; a stateless walk
// cannot. benches/lazy_vs.cpp sweeps it.
//
// lazy.hpp is already reachable through <glaze/json.hpp> (it includes it at line 17), so
// none of this needs a build change.

#include <glaze/json.hpp>

#include <string>
#include <string_view>

namespace
{

struct state {
  glz::generic doc;
  std::string out;
  bool live = false;
};

long long
checksum(const glz::generic &v)
{
  if ( v.is_number() ) return static_cast<long long>(v.get_number());
  if ( v.is_string() ) return static_cast<long long>(v.get_string().size());
  if ( v.is_boolean() ) return v.get_boolean() ? 1 : 2;
  if ( v.is_array() ) return static_cast<long long>(v.get_array().size());
  if ( v.is_object() ) return static_cast<long long>(v.get_object().size());
  return 0;
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// glz::lazy_json

inline constexpr auto k_lz = glz::opts{};
using lz_view = glz::lazy_json_view<k_lz>;

constexpr unsigned k_seg_max = 64;       // longest pointer token we carry
constexpr unsigned k_depth_max = 8;      // deepest pointer we cache a path for

// one rfc 6901 reference token, unescaped in place. ~1 -> '/' and ~0 -> '~', IN THAT
// ORDER (decoding ~0 first would turn "~01" into "/"). returns the cursor just past the
// token; *p must be at the '/' that introduces it.
const char *
next_token(const char *p, char *out, unsigned &len)
{
  ++p;      // the '/'
  len = 0;
  while ( *p and *p != '/' ) {
    char c = *p++;
    if ( c == '~' and *p ) {
      if ( *p == '1' ) {
        c = '/';
        ++p;
      } else if ( *p == '0' ) {
        c = '~';
        ++p;
      }
    }
    if ( len + 1 < k_seg_max ) out[len++] = c;
  }
  out[len] = '\0';
  return p;
}

bool
token_is_index(const char *s, unsigned len, size_t &idx)
{
  if ( len == 0 or len > 19 ) return false;
  size_t v = 0;
  for ( unsigned i = 0; i < len; i++ ) {
    if ( s[i] < '0' or s[i] > '9' ) return false;
    v = v * 10 + static_cast<size_t>(s[i] - '0');
  }
  idx = v;
  return true;
}

// descend one token. arrays take the numeric form, everything else is a key lookup —
// lazy_json_view has no at_pointer, so rfc 6901 is walked by hand here.
lz_view
step(const lz_view &v, const char *tok, unsigned len)
{
  size_t idx = 0;
  if ( v.is_array() and token_is_index(tok, len, idx) ) return v[idx];
  return v[std::string_view(tok, len)];
}

lz_view
walk_pointer(const lz_view &root, const char *ptr)
{
  if ( !ptr or ptr[0] == '\0' ) return root;
  char tok[k_seg_max];
  unsigned len = 0;
  lz_view v = root;
  for ( const char *p = ptr; *p == '/'; ) {
    p = next_token(p, tok, len);
    v = step(v, tok, len);
    if ( v.has_error() ) return v;
  }
  return v;
}

// same per-kind convention as checksum() above — numbers by value, strings by DECODED
// length, bools 1/2, containers by element count, 0 on any miss. read_into is the
// single-pass form; raw_json() + read_json would scan the value twice.
long long
lazy_checksum(const lz_view &v)
{
  if ( v.has_error() ) return 0;
  if ( v.is_number() ) {
    double d = 0;
    if ( v.read_into(d) ) return 0;
    return static_cast<long long>(d);
  }
  if ( v.is_string() ) {
    std::string s;
    if ( v.read_into(s) ) return 0;
    return static_cast<long long>(s.size());
  }
  if ( v.is_boolean() ) {
    bool b = false;
    if ( v.read_into(b) ) return 0;
    return b ? 1 : 2;
  }
  if ( v.is_array() or v.is_object() ) return static_cast<long long>(v.size());
  return 0;
}

// how far into buf the walk to `v` had to look. raw_json() runs skip_value_lazy from the
// value's first byte, so its end IS the last byte the walk touched.
//
// NOT json_end(): that returns doc_->json_ + doc_->len_, the end of the WHOLE BUFFER
// (glaze/json/lazy.hpp:922-926), which would charge every lazy row the entire document
// and reproduce the "glaze-lazy 3841.68 GB/s" bug this denominator exists to fix.
long long
lazy_reach(const lz_view &v, const char *buf)
{
  if ( v.has_error() ) return 0;
  const std::string_view sv = v.raw_json();
  if ( sv.empty() ) return 0;
  return static_cast<long long>(sv.data() - buf) + static_cast<long long>(sv.size());
}

// A held path down the document, so N pointers sharing a prefix pay for it once.
//
// This is what makes the lazy_json row glaze's BEST form rather than a strawman, and it
// is not optional dressing: operator[](size_t) is not progressive — it restarts at the
// '[' and re-skips `index` elements on every call (lazy.hpp:968-999) — so resolving
// /statuses/K/a, /statuses/K/b, /statuses/K/c without holding the element view is
// quadratic in K. Holding it also lets the four field lookups inside one status share a
// single parse_pos_ cursor, which is the whole point of the api.
//
// Sized small and constructed cheap ON PURPOSE. It is per-call scratch that sits inside
// the timed region, so anything it costs is charged to glaze: an earlier 32x128 version
// value-initialized ~5.5 KB per call and put ~200 cycles of harness on the one-pointer
// row, where the whole measurement is ~1200. depth is the only member that has to start
// at a known value — it gates every read of the other three — so it is the only one
// initialized. 8 levels covers any pointer in this corpus (the deepest is 4) and
// walk_cached degrades to an uncached walk past that rather than overrunning.
struct path_cache {
  char seg[k_depth_max][k_seg_max];
  unsigned len[k_depth_max];
  lz_view v[k_depth_max];      // v[d] is the view REACHED BY seg[0..d]
  unsigned depth;

  path_cache() noexcept : depth(0) { }
};

// `cur` is a POINTER, not a copy, and that is load-bearing: parse_pos_ is mutable, so
// stepping off the CACHED view is what advances its cursor and makes the next lookup on
// that same object resume instead of rescan. Copying the slot into a local would leave
// the cache frozen at its first position and quietly give back a stateless walk.
lz_view
walk_cached(lz_view &root, const char *ptr, path_cache &pc)
{
  if ( !ptr or ptr[0] == '\0' ) return root;
  char tok[k_seg_max];
  unsigned len = 0;
  unsigned d = 0;
  lz_view *cur = &root;
  lz_view deep{};      // holds the tail of a pointer deeper than the cache

  for ( const char *p = ptr; *p == '/'; ) {
    p = next_token(p, tok, len);
    // reuse while the token still matches what we already hold at this depth
    if ( d < pc.depth and pc.len[d] == len and __builtin_memcmp(pc.seg[d], tok, len) == 0 ) {
      cur = &pc.v[d];
      ++d;
      continue;
    }
    pc.depth = d;      // anything deeper than here is now stale
    const lz_view next = step(*cur, tok, len);
    if ( next.has_error() ) return next;
    if ( d < k_depth_max ) {
      __builtin_memcpy(pc.seg[d], tok, len + 1);
      pc.len[d] = len;
      pc.v[d] = next;
      cur = &pc.v[d];
      pc.depth = ++d;
    } else {
      deep = next;
      cur = &deep;
      ++d;
    }
  }
  return *cur;
}

};      // namespace

extern "C" {

void *
gl_new()
{
  return new state();
}

void
gl_free(void *p)
{
  delete static_cast<state *>(p);
}

int
gl_parse(void *p, const char *buf, unsigned long n)
{
  auto *s = static_cast<state *>(p);
  s->doc = glz::generic{};
  const std::string_view sv(buf, n);
  s->live = !glz::read_json(s->doc, sv);
  return s->live ? 1 : 0;
}

int
gl_load(void *p, const char *buf, unsigned long n)
{
  return gl_parse(p, buf, n);
}

long long
gl_serialize(void *p)
{
  auto *s = static_cast<state *>(p);
  if ( !s->live ) return -1;
  s->out.clear();
  if ( glz::write_json(s->doc, s->out) ) return -1;
  return static_cast<long long>(s->out.size());
}

// validate-only: glaze's skip-value pass, no dom, no values materialized
long long
gl_validate(const char *buf, unsigned long n)
{
  const std::string_view sv(buf, n);
  return glz::validate_json(sv) ? 0 : 1;
}

// lazy: walk the raw buffer to the pointer, then read only that fragment
long long
gl_extract(void *, const char *buf, unsigned long n, const char *ptr)
{
  const std::string_view sv(buf, n);
  const auto view = glz::get_view_json(std::string_view(ptr), sv);
  if ( !view ) return 0;
  glz::generic v{};
  if ( glz::read_json(v, *view) ) return 0;
  return checksum(v);
}

// bytes gl_extract actually has to look at, for the caller's GB/s denominator.
// get_view_json returns a view INTO buf, so the walk stopped at
// (view.data() - buf) + view.size() and never touched the tail beyond it. Charging the
// extract-lazy row the whole document instead credits glaze bytes it never read — that
// is what printed "glaze-lazy 3841.68 GB/s" on an 8.6 MB corpus it resolved in 636
// cycles. Timing-free and called once, outside the measured loop.
long long
gl_extract_reach(void *, const char *buf, unsigned long n, const char *ptr)
{
  const std::string_view sv(buf, n);
  const auto view = glz::get_view_json(std::string_view(ptr), sv);
  if ( !view ) return 0;
  return static_cast<long long>(view->data() - buf) + static_cast<long long>(view->size());
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// glz::lazy_json
//
// The n-field entry points take a pointer ARRAY: the abi stays pod-only
// (void*/int/long long/const char*/unsigned long, plus const char *const * here), so no
// stl type crosses it, same as every other shim in this directory.

// one-shot, so it can sit beside gl_extract on the same corpus row. deliberately routed
// through raw_json() -> glz::generic -> checksum(), the identical tail gl_extract uses,
// so the two rows cannot disagree on what a value is worth and get each other dropped by
// corpus_vs's agrees() gate.
long long
gl_lazy_extract(void *, const char *buf, unsigned long n, const char *ptr)
{
  auto doc = glz::lazy_json<k_lz>(std::string_view(buf, n));
  if ( !doc ) return 0;
  const lz_view v = walk_pointer(doc->root(), ptr);
  if ( v.has_error() ) return 0;
  glz::generic g{};
  if ( glz::read_json(g, v.raw_json()) ) return 0;
  return checksum(g);
}

long long
gl_lazy_extract_reach(void *, const char *buf, unsigned long n, const char *ptr)
{
  auto doc = glz::lazy_json<k_lz>(std::string_view(buf, n));
  if ( !doc ) return 0;
  return lazy_reach(walk_pointer(doc->root(), ptr), buf);
}

// N pointers against ONE lazy_document, with the shared prefix held across them. This is
// the row the sweep is about: lazy_json is the only glaze api that can amortize a walk
// over several reads, and this is what that amortization looks like when you use it.
long long
gl_lazy_nfields(void *, const char *buf, unsigned long n, const char *const *ptrs, int count)
{
  auto doc = glz::lazy_json<k_lz>(std::string_view(buf, n));
  if ( !doc ) return 0;
  path_cache pc;
  lz_view &root = doc->root();
  long long sum = 0;
  for ( int i = 0; i < count; i++ ) sum += lazy_checksum(walk_cached(root, ptrs[i], pc));
  return sum;
}

// the furthest byte any of the N walks had to look at — the GB/s denominator for the
// row above. timing-free, called once outside the measured loop.
long long
gl_lazy_nfields_reach(void *, const char *buf, unsigned long n, const char *const *ptrs, int count)
{
  auto doc = glz::lazy_json<k_lz>(std::string_view(buf, n));
  if ( !doc ) return 0;
  path_cache pc;
  lz_view &root = doc->root();
  long long hi = 0;
  for ( int i = 0; i < count; i++ ) {
    const long long r = lazy_reach(walk_cached(root, ptrs[i], pc), buf);
    if ( r > hi ) hi = r;
  }
  return hi;
}

// the control: the same N pointers, resolved the stateless way. get_view_json keeps
// nothing between calls, so this is N independent walks from byte 0 and the row the
// lazy_json one has to beat to have earned its cursor.
long long
gl_getview_nfields(void *, const char *buf, unsigned long n, const char *const *ptrs, int count)
{
  const std::string_view sv(buf, n);
  long long sum = 0;
  for ( int i = 0; i < count; i++ ) {
    const auto view = glz::get_view_json(std::string_view(ptrs[i]), sv);
    if ( !view ) continue;
    glz::generic g{};
    if ( glz::read_json(g, *view) ) continue;
    sum += checksum(g);
  }
  return sum;
}

long long
gl_getview_nfields_reach(void *, const char *buf, unsigned long n, const char *const *ptrs, int count)
{
  const std::string_view sv(buf, n);
  long long hi = 0;
  for ( int i = 0; i < count; i++ ) {
    const auto view = glz::get_view_json(std::string_view(ptrs[i]), sv);
    if ( !view ) continue;
    const long long r = static_cast<long long>(view->data() - buf) + static_cast<long long>(view->size());
    if ( r > hi ) hi = r;
  }
  return hi;
}

};      // extern "C"
