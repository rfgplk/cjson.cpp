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

};      // extern "C"
