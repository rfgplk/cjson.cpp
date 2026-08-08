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
#include "error.hpp"
#include "value.hpp"

#include <micron/cmalloc.hpp>
#include <micron/string/strings.hpp>
#include <micron/sum.hpp>
#include <micron/types.hpp>

// %%%%%%%%%%%%%%%%%%%%%%%%%%%
// doc

namespace cjson
{

class val;
struct scratch;
struct member;
struct arr_iter;
struct obj_iter;
struct arr_range;
struct obj_range;

struct wbounds {
  u64 flat = 0;
  u64 pretty_c = 0;
  u64 pretty_k = 0;
};

class doc
{
  value *__vals = nullptr;
  usize __nvals = 0;
  u8 *__pool = nullptr;
  const u8 *__alias = nullptr;      // insitu mode: the caller's buffer
  usize __pcap = 0;                 // allocation size of the pool (text + padding)
  usize __consumed = 0;
  wbounds __wb{};               // zero flat => predates the accumulators; writer falls back to the walks
  bool __borrowed = false;      // pool and vals belong to a scratch; release() must not free them

  friend class val;
  friend result<doc> parse(bytes, opts, scratch &) noexcept;
  friend constexpr max_t __parse_into(doc &, bytes, opts, scratch &) noexcept;
  friend constexpr max_t __parse_insitu_into(doc &, wbytes, opts, scratch &) noexcept;
  friend constexpr max_t __parse_reuse_into(doc &, bytes, opts, scratch &) noexcept;
  friend constexpr max_t __parse_insitu_reuse_into(doc &, wbytes, opts, scratch &) noexcept;

public:
  constexpr ~doc() { release(); }

  doc() = default;
  doc(const doc &) = delete;
  doc &operator=(const doc &) = delete;

  constexpr doc(doc &&o) noexcept
      : __vals(o.__vals), __nvals(o.__nvals), __pool(o.__pool), __alias(o.__alias), __pcap(o.__pcap), __consumed(o.__consumed),
        __wb(o.__wb), __borrowed(o.__borrowed)
  {
    o.__vals = nullptr;
    o.__nvals = 0;
    o.__pool = nullptr;
    o.__alias = nullptr;
    o.__pcap = 0;
    o.__wb = wbounds{};
    o.__borrowed = false;
  }

  constexpr doc &
  operator=(doc &&o) noexcept
  {
    release();
    __vals = o.__vals;
    __nvals = o.__nvals;
    __pool = o.__pool;
    __alias = o.__alias;
    __pcap = o.__pcap;
    __consumed = o.__consumed;
    __wb = o.__wb;
    __borrowed = o.__borrowed;
    o.__vals = nullptr;
    o.__nvals = 0;
    o.__pool = nullptr;
    o.__alias = nullptr;
    o.__pcap = 0;
    o.__wb = wbounds{};
    o.__borrowed = false;
    return *this;
  }

  constexpr void
  release() noexcept
  {
    if ( !__borrowed ) {
      if consteval {
        delete[] __vals;
        delete[] __pool;
      } else {
        if ( __vals ) abc::free(__vals);
        if ( __pool ) abc::free(__pool);
      }
    }
    __vals = nullptr;
    __pool = nullptr;
    __nvals = 0;
    __pcap = 0;
    __wb = wbounds{};
    __borrowed = false;
  }

  constexpr bool
  borrowed() const noexcept
  {
    return __borrowed;
  }

  constexpr bool
  alive() const noexcept
  {
    return __vals != nullptr and __nvals != 0;
  }

  constexpr wbounds
  __wbound_parts() const noexcept
  {
    return __wb;
  }

  constexpr usize
  size() const noexcept
  {
    return __nvals;
  }

  // byte offset one past the root value in the source
  constexpr usize
  consumed() const noexcept
  {
    return __consumed;
  }

  constexpr const u8 *
  pool() const noexcept
  {
    return __pool ? __pool : __alias;
  }

  constexpr val root() const noexcept;
};

namespace __doc
{
constexpr bool
key_equals(const u8 *pool, const value &v, strv key) noexcept
{
  if ( get_len(v) != key.len ) return false;
  const u8 *a = pool + v.pay.ofs;
  usize i = 0;
  while ( i + 8 <= key.len ) {
    if ( __load64(a + i) != __load64(reinterpret_cast<const u8 *>(key.ptr) + i) ) return false;
    i += 8;
  }
  for ( ; i < key.len; ++i )
    if ( a[i] != u8(key.ptr[i]) ) return false;
  return true;
}

constexpr usize
cstr_len(const char *s) noexcept
{
  usize n = 0;
  while ( s[n] ) ++n;
  return n;
}

};      // namespace __doc

class val
{
  const doc *__d = nullptr;
  const value *__v = nullptr;

public:
  ~val() = default;

  constexpr val() = default;

  constexpr val(const doc *d, const value *v) noexcept : __d(d), __v(v) { }

  constexpr kind
  type() const noexcept
  {
    return __v ? get_kind(*__v) : kind::none;
  }

  constexpr auto
  type_name() const noexcept
  {
    switch ( type() ) {
    case cjson::kind::null:
      return "null";
    case cjson::kind::boolean:
      return "boolean";
    case cjson::kind::number:
      return "number";
    case cjson::kind::string:
      return "string";
    case cjson::kind::array:
      return "array";
    case cjson::kind::object:
      return "object";
    case cjson::kind::raw:
      return "raw";
    default:
      return "none";
    }
  }

  constexpr
  operator cjson::pun()
  {
    switch ( type() ) {
    case cjson::kind::null:
      return {};
    case cjson::kind::boolean:
      return (__v->tag & s_mask) == s_true;
    case cjson::kind::number: {
      const u64 st = __v->tag & s_mask;
      if ( st == s_sint ) return __v->pay.i >= 0 ? u64(__v->pay.i) : u64(0);
      if ( st == s_uint ) return i64(__v->pay.u);
      if ( st == s_real ) return __v->pay.f;
      if ( st == s_sint ) return f64(__v->pay.i);
      return u64(0);
    }
    case cjson::kind::string: {
      strv t{ reinterpret_cast<const char *>(__d->pool() + __v->pay.ofs), usize(get_len(*__v)) };
      micron::string s{};
      s.append(t.ptr, t.len);
      return s;
    }
    case cjson::kind::array:
    case cjson::kind::object:
    case cjson::kind::raw:
      return __v;
    default:
      return {};
    }
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

  constexpr u64
  __subtype() const noexcept
  {
    return __v ? (__v->tag & s_mask) : 0;
  }

  constexpr const value *
  __raw() const noexcept
  {
    return __v;
  }

  // element count (arrays), pair count (objects), byte length (strings)
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
    if ( st == s_uint ) return __v->pay.u <= 0x7fffffffffffffffull ? i64(__v->pay.u) : d;
    return d;
  }

  constexpr u64
  u64_or(u64 d = 0) const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::number ) return d;
    const u64 st = __v->tag & s_mask;
    if ( st == s_uint ) return __v->pay.u;
    if ( st == s_sint ) return __v->pay.i >= 0 ? u64(__v->pay.i) : d;
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

  constexpr strv
  str_or(strv d = {}) const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::string ) return d;
    return strv{ reinterpret_cast<const char *>(__d->pool() + __v->pay.ofs), usize(get_len(*__v)) };
  }

  constexpr micron::string
  string_or(micron::string d = {}) const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::string ) return d;
    strv v{ reinterpret_cast<const char *>(__d->pool() + __v->pay.ofs), usize(get_len(*__v)) };
    micron::string s{};
    s.append(v.ptr, v.len);
    return s;
  }

  constexpr strv
  raw_str() const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::raw ) return strv{};
    return strv{ reinterpret_cast<const char *>(__d->pool() + __v->pay.ofs), usize(get_len(*__v)) };
  }

  constexpr result<i64>
  try_i64() const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::number ) return result<i64>{ micron::tag<error>{}, error::wrong_type };
    const u64 st = __v->tag & s_mask;
    if ( st == s_sint ) return result<i64>{ micron::tag<i64>{}, __v->pay.i };
    if ( st == s_uint and __v->pay.u <= 0x7fffffffffffffffull ) return result<i64>{ micron::tag<i64>{}, i64(__v->pay.u) };
    if ( st == s_uint ) return result<i64>{ micron::tag<error>{}, error::out_of_range };
    return result<i64>{ micron::tag<error>{}, error::wrong_type };
  }

  constexpr result<u64>
  try_u64() const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::number ) return result<u64>{ micron::tag<error>{}, error::wrong_type };
    const u64 st = __v->tag & s_mask;
    if ( st == s_uint ) return result<u64>{ micron::tag<u64>{}, __v->pay.u };
    if ( st == s_sint and __v->pay.i >= 0 ) return result<u64>{ micron::tag<u64>{}, u64(__v->pay.i) };
    if ( st == s_sint ) return result<u64>{ micron::tag<error>{}, error::out_of_range };
    return result<u64>{ micron::tag<error>{}, error::wrong_type };
  }

  constexpr result<f64>
  try_f64() const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::number ) return result<f64>{ micron::tag<error>{}, error::wrong_type };
    return result<f64>{ micron::tag<f64>{}, f64_or() };
  }

  constexpr result<bool>
  try_bool() const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::boolean ) return result<bool>{ micron::tag<error>{}, error::wrong_type };
    return result<bool>{ micron::tag<bool>{}, (__v->tag & s_mask) == s_true };
  }

  constexpr result<strv>
  try_str() const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::string ) return result<strv>{ micron::tag<error>{}, error::wrong_type };
    return result<strv>{ micron::tag<strv>{}, str_or() };
  }

  // navigation

  constexpr val
  operator[](strv key) const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::object ) return val{};
    const value *k = __v + 1;
    for ( u64 i = 0, pairs = get_len(*__v); i < pairs; ++i ) {
      if ( __doc::key_equals(__d->pool(), *k, key) ) return val{ __d, k + 1 };
      k = get_next(k + 1);
    }
    return val{};
  }

  constexpr val
  operator[](const char *key) const noexcept
  {
    return (*this)[strv{ key, __doc::cstr_len(key) }];
  }

  template<micron::is_string S>
  constexpr val
  operator[](const S &key) const noexcept
  {
    return (*this)[strv{ key.c_str(), key.size() }];
  }

  constexpr val
  operator[](usize i) const noexcept
  {
    if ( !__v or get_kind(*__v) != kind::array or i >= get_len(*__v) ) return val{};
    if ( arr_is_flat(*__v) ) return val{ __d, __v + 1 + i };
    const value *e = __v + 1;
    for ( usize n = 0; n < i; ++n ) e = get_next(e);
    return val{ __d, e };
  }

  constexpr val
  at(usize i) const noexcept
  {
    return (*this)[i];
  }

  // iteration

  constexpr arr_range items() const noexcept;
  constexpr obj_range members() const noexcept;

  constexpr val
  at_pointer(const char *ptr) const noexcept
  {
    return at_pointer(strv{ ptr, __doc::cstr_len(ptr) });
  }

  template<micron::is_string S>
  constexpr val
  at_pointer(const S &ptr) const noexcept
  {
    return at_pointer(strv{ ptr.c_str(), ptr.size() });
  }

  // rfc 6901 json pointer ("/a/b/0"; ~0 -> ~, ~1 -> /)
  constexpr val
  at_pointer(strv ptr) const noexcept
  {
    val cur = *this;
    usize i = 0;
    if ( ptr.len == 0 ) return cur;
    if ( ptr.ptr[0] != '/' ) return val{};
    ++i;
    while ( cur and i <= ptr.len ) {
      // token: [i, next '/')
      char tok[256];
      usize tn = 0;
      bool numeric = true;
      while ( i < ptr.len and ptr.ptr[i] != '/' ) {
        char c = ptr.ptr[i];
        if ( c == '~' and i + 1 < ptr.len ) {
          c = ptr.ptr[i + 1] == '0' ? '~' : (ptr.ptr[i + 1] == '1' ? '/' : c);
          ++i;
        }
        if ( tn < sizeof(tok) ) tok[tn++] = c;
        numeric = numeric and c >= '0' and c <= '9';
        ++i;
      }
      if ( cur.type() == kind::object ) {
        cur = cur[strv{ tok, tn }];
      } else if ( cur.type() == kind::array and numeric and tn > 0 and tn <= 10 ) {
        usize n = 0;
        for ( usize t = 0; t < tn; ++t ) n = n * 10 + usize(tok[t] - '0');
        cur = cur[n];
      } else {
        return val{};
      }
      if ( i >= ptr.len ) return cur;
      ++i;      // skip '/'
    }
    return cur;
  }
};

constexpr val
doc::root() const noexcept
{
  return alive() ? val{ this, __vals } : val{};
}

struct member {
  strv key;
  val v;
};

struct arr_iter {
  const doc *d;
  const value *e;

  constexpr val
  operator*() const noexcept
  {
    return val{ d, e };
  }

  constexpr arr_iter &
  operator++() noexcept
  {
    e = get_next(e);
    return *this;
  }

  constexpr bool
  operator!=(const arr_iter &o) const noexcept
  {
    return e != o.e;
  }
};

struct obj_iter {
  const doc *d;
  const value *k;

  constexpr member
  operator*() const noexcept
  {
    return member{ strv{ reinterpret_cast<const char *>(d->pool() + k->pay.ofs), usize(get_len(*k)) }, val{ d, k + 1 } };
  }

  constexpr obj_iter &
  operator++() noexcept
  {
    k = get_next(k + 1);
    return *this;
  }

  constexpr bool
  operator!=(const obj_iter &o) const noexcept
  {
    return k != o.k;
  }
};

struct arr_range {
  const doc *d;
  const value *first;
  const value *last;

  constexpr arr_iter
  begin() const noexcept
  {
    return { d, first };
  }

  constexpr arr_iter
  end() const noexcept
  {
    return { d, last };
  }
};

struct obj_range {
  const doc *d;
  const value *first;
  const value *last;

  constexpr obj_iter
  begin() const noexcept
  {
    return { d, first };
  }

  constexpr obj_iter
  end() const noexcept
  {
    return { d, last };
  }
};

constexpr arr_range
val::items() const noexcept
{
  if ( !__v or get_kind(*__v) != kind::array ) return { __d, nullptr, nullptr };
  return { __d, __v + 1, get_next(__v) };
}

constexpr obj_range
val::members() const noexcept
{
  if ( !__v or get_kind(*__v) != kind::object ) return { __d, nullptr, nullptr };
  return { __d, __v + 1, get_next(__v) };
}

};      // namespace cjson
