// Copyright (c) 2025 David Lucius Severus
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#pragma once

#include "config.hpp"
#include "doc.hpp"
#include "error.hpp"
#include "minify.hpp"
#include "parse.hpp"
#include "value.hpp"
#include "write.hpp"

#include <micron/types.hpp>

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// consteval layer
//
// parse, validate, minify and write JSON at compile time

namespace cjson::ct
{

template<usize N> struct bytes {
  u8 data[N ? N : 1]{};
  usize len = 0;

  constexpr usize
  size() const noexcept
  {
    return len;
  }

  constexpr const u8 *
  begin() const noexcept
  {
    return data;
  }

  constexpr u8
  operator[](usize i) const noexcept
  {
    return data[i];
  }
};

template<usize N> struct str {
  u8 data[N]{};
  usize len = N - 1;

  consteval str() : len(0) { }

  consteval str(const char (&s)[N])
  {
    for ( usize i = 0; i + 1 < N; ++i ) data[i] = u8(s[i]);
  }
};

namespace __ct
{

[[noreturn]] void __ct_fail(const char *what) noexcept;

consteval void
require(bool ok, const char *what)
{
  if ( !ok ) __ct_fail(what);
}

};      // namespace __ct

template<str S, opts O = opts{}> consteval bool validate() noexcept { return cjson::validate(S.data, S.len, O) == error::ok; }

template<str S, opts O = opts{}> consteval usize minify_size()
{
  u8 *tmp = new u8[S.len ? S.len : 1];
  const max_t w = minify_into(S.data, S.len, tmp, S.len, O);
  delete[] tmp;
  __ct::require(w >= 0, "cjson::ct::minify: invalid json");
  return usize(w);
}

template<str S, opts O = opts{}> consteval auto minify()
{
  constexpr usize m = minify_size<S, O>();
  bytes<m> out{};
  const max_t w = minify_into(S.data, S.len, out.data, m ? m : 1, O);
  __ct::require(w == max_t(m), "cjson::ct::minify: probe/value pass size mismatch");
  out.len = usize(w);
  return out;
}

struct counts {
  usize nv = 0;
  usize ns = 0;
};

template<str S, opts O = opts{}> consteval counts parse_counts()
{
  scratch sc{};
  doc d{};
  const max_t r = __parse_into(d, cjson::bytes{ S.data, S.len }, O, sc);
  __ct::require(r > 0, "cjson::ct::parse: invalid json");
  return counts{ usize(r), S.len };
}

template<usize NV, usize NS> struct tree;

class tval
{
  const value *__vals = nullptr;
  const u8 *__pool = nullptr;
  const value *__v = nullptr;

public:
  ~tval() = default;

  constexpr tval() = default;

  constexpr tval(const value *vals, const u8 *pool, const value *v) noexcept : __vals(vals), __pool(pool), __v(v) { }

  constexpr kind
  type() const noexcept
  {
    return __v ? get_kind(*__v) : kind::none;
  }

  constexpr explicit
  operator bool() const noexcept
  {
    return __v != nullptr and get_kind(*__v) != kind::none;
  }

  constexpr bool
  is_null() const noexcept
  {
    return type() == kind::null;
  }

  constexpr usize
  size() const noexcept
  {
    return __v ? usize(get_len(*__v)) : 0;
  }

  constexpr i64
  i64_or(i64 d = 0) const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::number ) return d;
    const u64 st = __v->tag & s_mask;
    if ( st == s_sint ) return __v->pay.i;
    if ( st == s_uint and __v->pay.u <= 0x7fffffffffffffffull ) return i64(__v->pay.u);
    return d;
  }

  constexpr u64
  u64_or(u64 d = 0) const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::number ) return d;
    const u64 st = __v->tag & s_mask;
    if ( st == s_uint ) return __v->pay.u;
    if ( st == s_sint and __v->pay.i >= 0 ) return u64(__v->pay.i);
    return d;
  }

  constexpr f64
  f64_or(f64 d = 0) const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::number ) return d;
    const u64 st = __v->tag & s_mask;
    if ( st == s_real ) return __v->pay.f;
    if ( st == s_sint ) return f64(__v->pay.i);
    return f64(__v->pay.u);
  }

  constexpr bool
  bool_or(bool d = false) const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::boolean ) return d;
    return (__v->tag & s_mask) == s_true;
  }

  strv
  str_or(strv d = {}) const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::string ) return d;
    return strv{ reinterpret_cast<const char *>(__pool + __v->pay.ofs), usize(get_len(*__v)) };
  }

  constexpr usize
  str_len() const noexcept
  {
    return (__v and get_kind(*__v) == kind::string) ? usize(get_len(*__v)) : 0;
  }

  constexpr u8
  str_at(usize i) const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::string or i >= get_len(*__v) ) return 0;
    return __pool[__v->pay.ofs + i];
  }

  constexpr bool
  str_is(const char *s) const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::string ) return false;
    const usize n = __doc::cstr_len(s);
    if ( n != get_len(*__v) ) return false;
    for ( usize i = 0; i < n; ++i )
      if ( __pool[__v->pay.ofs + i] != u8(s[i]) ) return false;
    return true;
  }

  constexpr tval
  operator[](strv key) const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::object ) return tval{};
    const value *k = __v + 1;
    for ( u64 i = 0, pairs = get_len(*__v); i < pairs; ++i ) {
      if ( __doc::key_equals(__pool, *k, key) ) return tval{ __vals, __pool, k + 1 };
      k = get_next(k + 1);
    }
    return tval{};
  }

  constexpr tval
  operator[](const char *key) const noexcept
  {
    return (*this)[strv{ key, __doc::cstr_len(key) }];
  }

  template<micron::is_string S>
  constexpr tval
  operator[](const S &key) const noexcept
  {
    return (*this)[strv{ key.c_str(), key.size() }];
  }

  constexpr tval
  operator[](usize i) const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::array or i >= get_len(*__v) ) return tval{};
    if ( arr_is_flat(*__v) ) return tval{ __vals, __pool, __v + 1 + i };
    const value *e = __v + 1;
    for ( usize n = 0; n < i; ++n ) e = get_next(e);
    return tval{ __vals, __pool, e };
  }
};

template<usize NV, usize NS> struct tree {
  value vals[NV ? NV : 1]{};
  u8 pool[NS ? NS : 1]{};
  usize n_vals = 0;
  usize n_pool = 0;

  constexpr tval
  root() const noexcept
  {
    return n_vals ? tval{ vals, pool, vals } : tval{};
  }
};

template<str S, opts O = opts{}> consteval auto parse()
{
  constexpr counts c = parse_counts<S, O>();
  tree<c.nv, c.ns> out{};
  {
    scratch sc{};
    doc d{};
    const max_t r = __parse_into(d, cjson::bytes{ S.data, S.len }, O, sc);
    __ct::require(r == max_t(c.nv), "cjson::ct::parse: probe/value pass mismatch");
    const value *dv = d.root().__raw();
    for ( usize i = 0; i < c.nv; ++i ) out.vals[i] = dv[i];
    const u8 *dp = d.pool();
    for ( usize i = 0; i < c.ns; ++i ) out.pool[i] = dp[i];
    out.n_vals = c.nv;
    out.n_pool = c.ns;
  }
  return out;
}

template<auto &Tree, style St = style{}> consteval usize write_size()
{
  const usize cap = __write::bound_slots(Tree.vals, Tree.n_vals) + __write::pretty_extra(Tree.vals, St.indent);
  u8 *tmp = new u8[cap ? cap : 1];
  u8 *end = __write::emit(tmp, Tree.vals, Tree.pool, St);
  const usize w = usize(end - tmp);
  delete[] tmp;
  return w;
}

template<auto &Tree, style St = style{}> consteval auto write()
{
  constexpr usize m = write_size<Tree, St>();
  // NOTE: the walk transiently writes a closer + comma before erasing
  u8 *tmp = new u8[m + 8];
  u8 *end = __write::emit(tmp, Tree.vals, Tree.pool, St);
  __ct::require(usize(end - tmp) == m, "cjson::ct::write: probe/value pass size mismatch");
  bytes<m> out{};
  for ( usize i = 0; i < m; ++i ) out.data[i] = tmp[i];
  delete[] tmp;
  out.len = m;
  return out;
}

};      // namespace cjson::ct
