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
#include "number.hpp"
#include "scratch.hpp"
#include "stage1.hpp"
#include "string.hpp"
#include "tables.hpp"
#include "value.hpp"

#include <micron/sum.hpp>
#include <micron/types.hpp>

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// ondemand hot read path

namespace cjson
{

class view;
struct cur_arr_iter;
struct cur_obj_iter;
struct cur_arr_range;
struct cur_obj_range;

class cur
{
  const view *__v = nullptr;
  max_t __k = 0;      // position in the index array

  friend class view;
  friend struct cur_arr_iter;
  friend struct cur_obj_iter;

  constexpr cur(const view *v, max_t k) noexcept : __v(v), __k(k) { }

  constexpr const u8 *base() const noexcept;
  constexpr usize input_len() const noexcept;
  constexpr const u32 *idx() const noexcept;
  constexpr max_t nidx() const noexcept;

  constexpr u8
  tok() const noexcept
  {
    return (__v != nullptr and __k < nidx()) ? base()[idx()[__k]] : 0;
  }

  constexpr bool
  skip(max_t &k) const noexcept
  {
    if ( k >= nidx() ) return false;
    const u8 c = base()[idx()[k]];
    if ( c == u8('"') ) {
      ++k;
      while ( k < nidx() and base()[idx()[k]] != u8('"') ) ++k;
      if ( k >= nidx() ) return false;
      ++k;
      return true;
    }
    if ( c == u8('{') or c == u8('[') ) {
      u32 d = 1;
      ++k;
      while ( k < nidx() and d != 0 ) {
        const u8 t = base()[idx()[k]];
        if ( t == u8('"') ) {
          max_t s = k;
          if ( !skip(s) ) return false;
          k = s;
          continue;
        }
        if ( t == u8('{') or t == u8('[') )
          ++d;
        else if ( t == u8('}') or t == u8(']') )
          --d;
        ++k;
      }
      return d == 0;
    }
    ++k;      // scalar, ',', ':' — single index
    return true;
  }

public:
  ~cur() = default;

  constexpr cur() = default;

  // type() is peek_kind() under the name val uses
  constexpr kind
  type() const noexcept
  {
    return peek_kind();
  }

  constexpr kind
  peek_kind() const noexcept
  {
    switch ( tok() ) {
    case u8('{'):
      return kind::object;
    case u8('['):
      return kind::array;
    case u8('"'):
      return kind::string;
    case u8('t'):
    case u8('f'):
      return kind::boolean;
    case u8('n'):
      return kind::null;
    case u8('-'):
    case u8('0'):
    case u8('1'):
    case u8('2'):
    case u8('3'):
    case u8('4'):
    case u8('5'):
    case u8('6'):
    case u8('7'):
    case u8('8'):
    case u8('9'):
      return kind::number;
    default:
      return kind::none;
    }
  }

  constexpr explicit
  operator bool() const noexcept
  {
    return __v != nullptr and peek_kind() != kind::none;
  }

  constexpr cur
  operator[](strv key) const noexcept
  {
    if ( !__v or tok() != u8('{') ) return cur{};
    max_t k = __k + 1;
    while ( k < nidx() and base()[idx()[k]] == u8('"') ) {
      const usize open = idx()[k];
      // find the close quote of this key
      max_t c = k + 1;
      while ( c < nidx() and base()[idx()[c]] != u8('"') ) ++c;
      if ( c >= nidx() ) return cur{};
      const usize close = idx()[c];
      const bool match = (close - open - 1) == key.len and[&]
      {
        for ( usize i = 0; i < key.len; ++i )
          if ( base()[open + 1 + i] != u8(key.ptr[i]) ) return false;
        return true;
      }
      ();
      max_t vk = c + 1;
      if ( vk < nidx() and base()[idx()[vk]] == u8(':') ) ++vk;
      if ( match ) return cur{ __v, vk };
      if ( !skip(vk) ) return cur{};
      if ( vk < nidx() and base()[idx()[vk]] == u8(',') ) ++vk;
      k = vk;
    }
    return cur{};
  }

  constexpr cur
  operator[](const char *key) const noexcept
  {
    usize n = 0;
    while ( key[n] ) ++n;
    return (*this)[strv{ key, n }];
  }

  template<micron::is_string S>
  constexpr cur
  operator[](const S &key) const noexcept
  {
    return (*this)[strv{ key.c_str(), key.size() }];
  }

  // array element access (forward walk)
  constexpr cur
  at(usize i) const noexcept
  {
    if ( !__v or tok() != u8('[') ) return cur{};
    max_t k = __k + 1;
    for ( usize e = 0;; ++e ) {
      if ( k >= nidx() or base()[idx()[k]] == u8(']') ) return cur{};
      if ( e == i ) return cur{ __v, k };
      if ( !skip(k) ) return cur{};
      if ( k < nidx() and base()[idx()[k]] == u8(',') ) ++k;
    }
  }

  // scalars
  constexpr bool
  is_null() const noexcept
  {
    return peek_kind() == kind::null;
  }

  constexpr i64
  i64_or(i64 d = 0) const noexcept
  {
    if ( peek_kind() != kind::number ) return d;
    value v{};
    if ( __num::read_number(base(), input_len(), idx()[__k], v) < 0 ) return d;
    const u64 st = v.tag & s_mask;
    if ( st == s_sint ) return v.pay.i;
    if ( st == s_uint and v.pay.u <= 0x7fffffffffffffffull ) return i64(v.pay.u);
    return d;
  }

  constexpr u64
  u64_or(u64 d = 0) const noexcept
  {
    if ( peek_kind() != kind::number ) return d;
    value v{};
    if ( __num::read_number(base(), input_len(), idx()[__k], v) < 0 ) return d;
    const u64 st = v.tag & s_mask;
    if ( st == s_uint ) return v.pay.u;
    if ( st == s_sint and v.pay.i >= 0 ) return u64(v.pay.i);
    return d;
  }

  constexpr f64
  f64_or(f64 d = 0) const noexcept
  {
    if ( peek_kind() != kind::number ) return d;
    value v{};
    if ( __num::read_number(base(), input_len(), idx()[__k], v) < 0 ) return d;
    const u64 st = v.tag & s_mask;
    if ( st == s_real ) return v.pay.f;
    if ( st == s_sint ) return f64(v.pay.i);
    return f64(v.pay.u);
  }

  constexpr
  operator cjson::pun()
  {
    value v{};
    switch ( peek_kind() ) {
    case cjson::kind::null:
      return {};
    case cjson::kind::boolean:
      return bool_or();
    case cjson::kind::number: {
      if ( __num::read_number(base(), input_len(), idx()[__k], v) < 0 ) return u64(0);
      const u64 st = v.tag & s_mask;
      if ( st == s_sint ) return v.pay.i >= 0 ? u64(v.pay.i) : u64(0);
      if ( st == s_uint ) return i64(v.pay.u);
      if ( st == s_real ) return v.pay.f;
      if ( st == s_sint ) return f64(v.pay.i);
      return u64(0);
    }
    case cjson::kind::string: {
      auto t = str_raw();
      micron::string s{};
      s.append(t.ptr, t.len);
      return s;
    }
      // TODO: add these
    case cjson::kind::array:
    case cjson::kind::object:
    case cjson::kind::raw:
    default:
      return {};
    }
  }

  constexpr bool
  bool_or(bool d = false) const noexcept
  {
    const u8 c = tok();
    if ( c == u8('t') ) return true;
    if ( c == u8('f') ) return false;
    return d;
  }

  // escapes __NOT__ decoded
  constexpr strv
  str_raw() const noexcept
  {
    if ( tok() != u8('"') ) return strv{};
    max_t c = __k + 1;
    while ( c < nidx() and base()[idx()[c]] != u8('"') ) ++c;
    if ( c >= nidx() ) return strv{};
    const usize open = idx()[__k], close = idx()[c];
    return strv{ reinterpret_cast<const char *>(base()) + open + 1, close - open - 1 };
  }

  constexpr max_t
  str(wbytes out) const noexcept
  {
    if ( tok() != u8('"') ) return fail(error::wrong_type);
    if ( __k + 1 < nidx() and base()[idx()[__k + 1]] == u8('"') ) {
      const strv s = str_raw();
      if ( s.len > out.len ) return fail(error::short_output);
      for ( usize i = 0; i < s.len; ++i ) out.ptr[i] = u8(s.ptr[i]);
      return max_t(s.len);
    }
    const u8 *p = base();
    usize w = 0;
    usize src = idx()[__k] + 1;
    max_t j = __k + 1;
    for ( ;; ) {
      if ( j >= nidx() ) return fail(error::bad_string);
      const usize stop = idx()[j];
      if ( stop > src ) {
        if ( w + (stop - src) > out.len ) return fail(error::short_output);
        for ( usize i = 0; i < stop - src; ++i ) out.ptr[w + i] = p[src + i];
        w += stop - src;
        src = stop;
      }
      if ( p[stop] == u8('"') ) return max_t(w);
      if ( src + 1 >= input_len() ) return fail(error::bad_string);
      const u8 e = p[src + 1];
      const u8 simple = escape_map[e];
      if ( simple != 0 ) {
        if ( w + 1 > out.len ) return fail(error::short_output);
        out.ptr[w++] = simple;
        src += 2;
        ++j;
        continue;
      }
      if ( e != u8('u') or src + 6 > input_len() ) return fail(error::bad_escape);
      u32 cp = hex4_to_u32(p + src + 2);
      if ( cp > 0xffffu ) return fail(error::bad_escape);
      if ( cp >= 0xd800u and cp < 0xdc00u ) {
        if ( j + 1 >= nidx() or idx()[j + 1] != src + 6 or src + 12 > input_len() or p[src + 7] != u8('u') )
          return fail(error::bad_surrogate);
        const u32 lo = hex4_to_u32(p + src + 8);
        if ( lo < 0xdc00u or lo >= 0xe000u ) return fail(error::bad_surrogate);
        cp = (((cp - 0xd800u) << 10) | (lo - 0xdc00u)) + 0x10000u;
        if ( w + 4 > out.len ) return fail(error::short_output);
        w += __str::encode_utf8(cp, out.ptr + w);
        src += 12;
        j += 2;
        continue;
      }
      if ( cp >= 0xdc00u and cp < 0xe000u ) return fail(error::bad_surrogate);
      if ( w + 3 > out.len ) return fail(error::short_output);
      w += __str::encode_utf8(cp, out.ptr + w);
      src += 6;
      ++j;
    }
  }

  // forward iteration
  constexpr cur_arr_range items() const noexcept;
  constexpr cur_obj_range members() const noexcept;

  // rfc 6901 json pointer ("/a/b/0"; ~0 -> ~, ~1 -> /)
  constexpr cur
  at_pointer(strv ptr) const noexcept
  {
    if ( ptr.len == 0 ) return *this;
    if ( ptr.ptr[0] != '/' ) return cur{};
    cur c = *this;
    usize i = 1;
    while ( static_cast<bool>(c) ) {
      char tok[256];
      usize tn = 0;
      bool numeric = true;
      while ( i < ptr.len and ptr.ptr[i] != '/' ) {
        char ch = ptr.ptr[i];
        if ( ch == '~' and i + 1 < ptr.len ) {
          ch = ptr.ptr[i + 1] == '0' ? '~' : (ptr.ptr[i + 1] == '1' ? '/' : ch);
          ++i;
        }
        if ( tn < sizeof(tok) ) tok[tn++] = ch;
        numeric = numeric and ch >= '0' and ch <= '9';
        ++i;
      }
      if ( c.type() == kind::object ) {
        c = c[strv{ tok, tn }];
      } else if ( c.type() == kind::array and numeric and tn > 0 and tn <= 10 ) {
        usize n = 0;
        for ( usize t = 0; t < tn; ++t ) n = n * 10 + usize(tok[t] - '0');
        c = c.at(n);
      } else {
        return cur{};
      }
      if ( i >= ptr.len ) return c;
      ++i;      // past the '/'
    }
    return c;
  }

  constexpr cur
  at_pointer(const char *ptr) const noexcept
  {
    usize n = 0;
    while ( ptr[n] ) ++n;
    return at_pointer(strv{ ptr, n });
  }

  template<micron::is_string S>
  constexpr cur
  at_pointer(const S &ptr) const noexcept
  {
    return at_pointer(strv{ ptr.c_str(), ptr.size() });
  }

  // element count by walking (arrays/objects)
  constexpr usize
  count() const noexcept
  {
    const u8 c = tok();
    if ( c != u8('[') and c != u8('{') ) return 0;
    const bool obj = c == u8('{');
    usize n = 0;
    max_t k = __k + 1;
    for ( ;; ) {
      if ( k >= nidx() ) return n;
      const u8 t = base()[idx()[k]];
      if ( t == (obj ? u8('}') : u8(']')) ) return n;
      if ( obj ) {
        if ( !skip(k) ) return n;      // key
        if ( k < nidx() and base()[idx()[k]] == u8(':') ) ++k;
      }
      if ( !skip(k) ) return n;
      ++n;
      if ( k < nidx() and base()[idx()[k]] == u8(',') ) ++k;
    }
  }
};

class view
{
  const u8 *__p = nullptr;
  usize __len = 0;
  const u32 *__idx = nullptr;
  max_t __n = 0;

  friend class cur;
  friend inline result<view> iterate(bytes, opts, scratch &) noexcept;

public:
  ~view() = default;

  view() = default;

  constexpr cur
  root() const noexcept
  {
    return cur{ this, 0 };
  }

  constexpr bool
  alive() const noexcept
  {
    return __p != nullptr and __n > 0;
  }
};

constexpr const u8 *
cur::base() const noexcept
{
  return __v->__p;
}

constexpr usize
cur::input_len() const noexcept
{
  return __v->__len;
}

constexpr const u32 *
cur::idx() const noexcept
{
  return __v->__idx;
}

constexpr max_t
cur::nidx() const noexcept
{
  return __v->__n;
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// on-demand forward ranges

struct cur_member {
  strv key;      // raw key bytes
  cur v;
};

struct cur_arr_iter {
  const view *__v = nullptr;
  max_t __k = 0;
  bool __done = true;

  constexpr cur
  operator*() const noexcept
  {
    return cur{ __v, __k };
  }

  constexpr cur_arr_iter &
  operator++() noexcept
  {
    if ( __done ) return *this;
    const cur c{ __v, __k };
    max_t k = __k;
    if ( !c.skip(k) ) {
      __done = true;
      return *this;
    }
    if ( k < c.nidx() and c.base()[c.idx()[k]] == u8(',') ) ++k;
    __k = k;
    __done = (k >= c.nidx() or c.base()[c.idx()[k]] == u8(']'));
    return *this;
  }

  constexpr bool
  operator!=(const cur_arr_iter &o) const noexcept
  {
    return __done != o.__done or (!__done and __k != o.__k);
  }
};

struct cur_obj_iter {
  const view *__v = nullptr;
  max_t __k = 0;      // index of the key's opening quote
  bool __done = true;

  constexpr cur_member
  operator*() const noexcept
  {
    const cur key{ __v, __k };
    max_t vk = __k;
    if ( !key.skip(vk) ) return cur_member{};
    if ( vk < key.nidx() and key.base()[key.idx()[vk]] == u8(':') ) ++vk;
    return cur_member{ key.str_raw(), cur{ __v, vk } };
  }

  constexpr cur_obj_iter &
  operator++() noexcept
  {
    if ( __done ) return *this;
    const cur c{ __v, __k };
    max_t k = __k;
    if ( !c.skip(k) ) {      // past the key
      __done = true;
      return *this;
    }
    if ( k < c.nidx() and c.base()[c.idx()[k]] == u8(':') ) ++k;
    if ( !c.skip(k) ) {      // past the value
      __done = true;
      return *this;
    }
    if ( k < c.nidx() and c.base()[c.idx()[k]] == u8(',') ) ++k;
    __k = k;
    __done = (k >= c.nidx() or c.base()[c.idx()[k]] != u8('"'));
    return *this;
  }

  constexpr bool
  operator!=(const cur_obj_iter &o) const noexcept
  {
    return __done != o.__done or (!__done and __k != o.__k);
  }
};

struct cur_arr_range {
  cur_arr_iter __first{};

  constexpr cur_arr_iter
  begin() const noexcept
  {
    return __first;
  }

  constexpr cur_arr_iter
  end() const noexcept
  {
    return cur_arr_iter{};
  }
};

struct cur_obj_range {
  cur_obj_iter __first{};

  constexpr cur_obj_iter
  begin() const noexcept
  {
    return __first;
  }

  constexpr cur_obj_iter
  end() const noexcept
  {
    return cur_obj_iter{};
  }
};

constexpr cur_arr_range
cur::items() const noexcept
{
  if ( !__v or tok() != u8('[') ) return cur_arr_range{};
  const max_t k = __k + 1;
  const bool empty = (k >= nidx() or base()[idx()[k]] == u8(']'));
  return cur_arr_range{ cur_arr_iter{ __v, k, empty } };
}

constexpr cur_obj_range
cur::members() const noexcept
{
  if ( !__v or tok() != u8('{') ) return cur_obj_range{};
  const max_t k = __k + 1;
  const bool empty = (k >= nidx() or base()[idx()[k]] != u8('"'));
  return cur_obj_range{ cur_obj_iter{ __v, k, empty } };
}

inline result<view>
iterate(bytes in, opts o, scratch &sc) noexcept
{
  if ( in.len == 0 ) return result<view>{ micron::tag<error>{}, error::empty_input };
  if ( !sc.ensure(in.len) ) return result<view>{ micron::tag<error>{}, error::oom };
  const max_t n = __scan::index_input(in.ptr, in.len, sc.idx, o);
  if ( n < 0 ) return result<view>{ micron::tag<error>{}, as_error(n) };
  if ( n == 0 ) return result<view>{ micron::tag<error>{}, error::empty_input };
  view v{};
  v.__p = in.ptr;
  v.__len = in.len;
  v.__idx = sc.idx;
  v.__n = n;
  return result<view>{ micron::tag<view>{}, micron::move(v) };
}

inline result<view>
iterate(bytes in, scratch &sc) noexcept
{
  return iterate(in, opts{}, sc);
}

inline result<view>
iterate(const char *p, usize n, opts o, scratch &sc) noexcept
{
  return iterate(bytes{ reinterpret_cast<const u8 *>(p), n }, o, sc);
}

inline result<view>
iterate(const char *p, usize n, scratch &sc) noexcept
{
  return iterate(bytes{ reinterpret_cast<const u8 *>(p), n }, opts{}, sc);
}

inline result<view>
iterate(const u8 *p, usize n, opts o, scratch &sc) noexcept
{
  return iterate(bytes{ p, n }, o, sc);
}

inline result<view>
iterate(const u8 *p, usize n, scratch &sc) noexcept
{
  return iterate(bytes{ p, n }, opts{}, sc);
}

inline result<view>
iterate(strv in, opts o, scratch &sc) noexcept
{
  return iterate(bytes{ reinterpret_cast<const u8 *>(in.ptr), in.len }, o, sc);
}

inline result<view>
iterate(strv in, scratch &sc) noexcept
{
  return iterate(bytes{ reinterpret_cast<const u8 *>(in.ptr), in.len }, opts{}, sc);
}

template<text_source C>
inline result<view>
iterate(const C &in, opts o, scratch &sc) noexcept
{
  return iterate(as_bytes(in), o, sc);
}

template<text_source C>
inline result<view>
iterate(const C &in, scratch &sc) noexcept
{
  return iterate(as_bytes(in), opts{}, sc);
}

};      // namespace cjson
