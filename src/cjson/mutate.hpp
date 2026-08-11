//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#pragma once

#include "config.hpp"
#include "doc.hpp"
#include "error.hpp"
#include "number.hpp"
#include "value.hpp"

#include <micron/type_traits.hpp>

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// in place mutations

namespace cjson
{

namespace __mut
{

constexpr u64
wb_contrib(const value &v) noexcept
{
  switch ( get_kind(v) ) {
  case kind::string:
    return ((v.tag & s_mask) == s_noesc ? u64(get_len(v)) : u64(get_len(v)) * 6) + 4;
  case kind::raw:
    return u64(get_len(v)) + 12;
  case kind::number:
    return 42;
  default:
    return 12;      // literals and containers
  }
}

constexpr bool
needs_escape(const char *s, usize n) noexcept
{
  for ( usize i = 0; i < n; ++i ) {
    const u8 c = u8(s[i]);
    if ( c == u8('"') or c == u8('\\') or c < 0x20 ) return true;
  }
  return false;
}

constexpr void
move_slots(value *dst, const value *src, usize n) noexcept
{
  if ( n == 0 or dst == src ) return;
  if consteval {
    if ( dst < src ) {
      for ( usize i = 0; i < n; ++i ) dst[i] = src[i];
    } else {
      for ( usize i = n; i-- > 0; ) dst[i] = src[i];
    }
    return;
  }
  micron::memmove(dst, src, n);
}

};      // namespace __mut

class mut: public val
{
  friend class doc;

  constexpr mut(doc *d, value *v) noexcept : val(d, v) { }

  constexpr doc *
  __md() const noexcept
  {
    return const_cast<doc *>(__docp());
  }

  constexpr value *
  __mv() const noexcept
  {
    return const_cast<value *>(__raw());
  }

  static constexpr mut
  __rebind(val v) noexcept
  {
    return mut{ const_cast<doc *>(v.__d), const_cast<value *>(v.__v) };
  }

  // prologue
  constexpr bool
  __begin_set(u64 &before) const noexcept
  {
    doc *d = __md();
    if ( !d ) return false;
    if ( !*this ) {
      d->__merr = error::no_such_field;
      return false;
    }
    if ( is_ctn(*__raw()) ) {
      d->__merr = error::wrong_type;
      return false;
    }
    before = __mut::wb_contrib(*__raw());
    return true;
  }

  constexpr error
  __finish(u64 before) const noexcept
  {
    __md()->__wb_swap(before, __mut::wb_contrib(*__raw()));
    return error::ok;
  }

public:
  constexpr mut() = default;

  mut &operator=(const mut &) = delete;
  mut &operator=(const val &) = delete;

  constexpr mut
  operator[](strv key) const noexcept
  {
    return __rebind(val::operator[](key));
  }

  constexpr mut
  operator[](const char *key) const noexcept
  {
    return __rebind(val::operator[](key));
  }

  template<micron::is_string S>
  constexpr mut
  operator[](const S &key) const noexcept
  {
    return __rebind(val::operator[](key));
  }

  constexpr mut
  operator[](usize i) const noexcept
  {
    return __rebind(val::operator[](i));
  }

  constexpr mut
  at(usize i) const noexcept
  {
    return __rebind(val::at(i));
  }

  constexpr mut
  at_pointer(strv p) const noexcept
  {
    return __rebind(val::at_pointer(p));
  }

  constexpr mut
  at_pointer(const char *p) const noexcept
  {
    return __rebind(val::at_pointer(p));
  }

  template<micron::is_string S>
  constexpr mut
  at_pointer(const S &p) const noexcept
  {
    return __rebind(val::at_pointer(p));
  }

  constexpr error
  set(i64 x) const noexcept
  {
    u64 before = 0;
    if ( !__begin_set(before) ) return __md() ? __md()->mut_error() : error::no_such_field;
    value *v = __mv();
    if ( x >= 0 ) {
      v->tag = make_tag(kind::number, s_uint, 0);
      v->pay.u = u64(x);
    } else {
      v->tag = make_tag(kind::number, s_sint, 0);
      v->pay.i = x;
    }
    return __finish(before);
  }

  constexpr error
  set(u64 x) const noexcept
  {
    u64 before = 0;
    if ( !__begin_set(before) ) return __md() ? __md()->mut_error() : error::no_such_field;
    value *v = __mv();
    v->tag = make_tag(kind::number, s_uint, 0);
    v->pay.u = x;
    return __finish(before);
  }

  constexpr error
  set(f64 x) const noexcept
  {
    u64 before = 0;
    if ( !__begin_set(before) ) return __md() ? __md()->mut_error() : error::no_such_field;
    value *v = __mv();
    v->tag = make_tag(kind::number, s_real, 0);
    v->pay.f = x;
    return __finish(before);
  }

  constexpr error
  set(bool x) const noexcept
  {
    u64 before = 0;
    if ( !__begin_set(before) ) return __md() ? __md()->mut_error() : error::no_such_field;
    value *v = __mv();
    v->tag = make_tag(kind::boolean, x ? s_true : s_false, 0);
    v->pay.u = 0;
    return __finish(before);
  }

  constexpr error
  set_null() const noexcept
  {
    u64 before = 0;
    if ( !__begin_set(before) ) return __md() ? __md()->mut_error() : error::no_such_field;
    value *v = __mv();
    v->tag = make_tag(kind::null, 0, 0);
    v->pay.u = 0;
    return __finish(before);
  }

  constexpr error
  set(strv s) const noexcept
  {
    u64 before = 0;
    if ( !__begin_set(before) ) return __md() ? __md()->mut_error() : error::no_such_field;
    doc *d = __md();
    const usize vi = __idx();
    value *v = d->__vals + vi;
    const u64 sub = __mut::needs_escape(s.ptr, s.len) ? s_plain : s_noesc;

    const bool was_str = get_kind(*v) == kind::string or get_kind(*v) == kind::raw;
    const bool writable = d->__pool != nullptr and !d->__borrowed;
    if ( was_str and writable and usize(get_len(*v)) >= s.len ) {
      u8 *p = d->__pool_w() + v->pay.ofs;
      for ( usize i = 0; i < s.len; ++i ) p[i] = u8(s.ptr[i]);
      p[s.len] = 0;
      v->tag = make_tag(kind::string, sub, s.len);
      d->__wb_swap(before, __mut::wb_contrib(*v));
      return error::ok;
    }
    const u64 ofs = d->__pool_put(s.ptr, s.len);
    if ( ofs == doc::__pool_fail ) return d->mut_error();
    v = d->__vals + vi;
    v->tag = make_tag(kind::string, sub, s.len);
    v->pay.ofs = ofs;
    d->__wb_swap(before, __mut::wb_contrib(*v));
    return error::ok;
  }

  constexpr error
  set(const char *s) const noexcept
  {
    return set(as_strv(s));
  }

  template<micron::is_string S>
  constexpr error
  set(const S &s) const noexcept
  {
    return set(as_strv(s));
  }

  constexpr error
  set_number(bytes text) const noexcept
  {
    u64 before = 0;
    if ( !__begin_set(before) ) return __md() ? __md()->mut_error() : error::no_such_field;
    value tmp{};
    // read_number reads p[start] before it consults len
    if ( text.len == 0 or __num::read_number(text.ptr, text.len, 0, tmp) < 0 ) {
      __md()->__merr = error::bad_number;
      return error::bad_number;
    }
    *__mv() = tmp;
    return __finish(before);
  }

  constexpr error
  set_key(strv key) const noexcept
  {
    doc *d = __md();
    if ( !d ) return error::no_such_field;
    value *v = __mv();
    if ( !v or get_kind(*v) != kind::string ) {
      if ( d ) d->__merr = error::wrong_type;
      return error::wrong_type;
    }
    return set(key);
  }

  // %%%%%%%%%%%%%%%%%%%%%%%%
  // structural edits
  //
  // WARNING: every fn here invalidates every outstanding val/mut in the doc; renavigate from doc::edit()

private:
  static constexpr void
  __patch_spans(doc *d, usize parent_idx, i64 dslots) noexcept
  {
    const i64 db = dslots * i64(sizeof(value));
    value *cur = d->__vals;
    usize ci = 0;
    while ( ci != parent_idx ) {
      const usize span = usize(cur->pay.ofs / sizeof(value));
      cur->pay.ofs = u64(i64(cur->pay.ofs) + db);
      value *child = cur + 1;
      const value *end = cur + span;
      bool descended = false;
      while ( child < end ) {
        const usize chi = usize(child - d->__vals);
        const usize cspan = is_ctn(*child) ? usize(child->pay.ofs / sizeof(value)) : 1;
        if ( parent_idx >= chi and parent_idx < chi + cspan ) {
          cur = child;
          ci = chi;
          descended = true;
          break;
        }
        child = const_cast<value *>(get_next(child));
      }
      if ( !descended ) return;
    }
    cur->pay.ofs = u64(i64(cur->pay.ofs) + db);
  }

  static constexpr void
  __ctn_bump_len(value *c, i64 delta) noexcept
  {
    const i64 n = i64(get_len(*c)) + delta;
    c->tag = make_tag(get_kind(*c), 0, u64(n < 0 ? 0 : n));
  }

  // the slot index of this handle's value
  constexpr usize
  __idx() const noexcept
  {
    return usize(__raw() - __md()->__vals);
  }

  constexpr usize
  __span() const noexcept
  {
    const value *v = __raw();
    return is_ctn(*v) ? usize(v->pay.ofs / sizeof(value)) : 1;
  }

  constexpr bool
  __begin_ctn(kind want) const noexcept
  {
    doc *d = __md();
    if ( !d ) return false;
    if ( !*this ) {
      d->__merr = error::no_such_field;
      return false;
    }
    if ( type() != want ) {
      d->__merr = error::wrong_type;
      return false;
    }
    return true;
  }

  constexpr usize
  __append_slots(usize n) const noexcept
  {
    doc *d = __md();
    const usize pi = __idx();
    if ( !d->__own_vals(n) ) return usize(-1);
    value *p = d->__vals + pi;
    const usize at = pi + usize(p->pay.ofs / sizeof(value));
    __patch_spans(d, pi, i64(n));
    __mut::move_slots(d->__vals + at + n, d->__vals + at, d->__nvals - at);
    d->__nvals += n;
    d->__wb_invalidate();
    return at;
  }

  static constexpr void
  __drop_slots(doc *d, usize pi, usize at, usize n) noexcept
  {
    __patch_spans(d, pi, -i64(n));
    __mut::move_slots(d->__vals + at, d->__vals + at + n, d->__nvals - at - n);
    d->__nvals -= n;
    d->__wb_invalidate();
  }

  constexpr usize
  __find_key(strv key) const noexcept
  {
    doc *d = __md();
    const value *v = __raw();
    const value *k = v + 1;
    const value *end = get_next(v);
    const u8 *pool = d->pool();
    while ( k < end ) {
      if ( __doc::key_equals(pool, *k, key) ) return usize(k - d->__vals);
      k = get_next(k + 1);
    }
    return usize(-1);
  }

  static constexpr void
  __put_null(value *v) noexcept
  {
    v->tag = make_tag(kind::null, 0, 0);
    v->pay.u = 0;
  }

  static constexpr void
  __put_empty_ctn(value *v, kind kd) noexcept
  {
    v->tag = make_tag(kd, 0, 0);
    v->pay.ofs = sizeof(value);
  }

  constexpr mut
  __member(strv key, kind kd, bool container) const noexcept
  {
    if ( !__begin_ctn(kind::object) ) return mut{};
    doc *d = __md();
    if ( const usize ki = __find_key(key); ki != usize(-1) ) return mut{ d, d->__vals + ki + 1 };
    const usize pi = __idx();
    const u64 ofs = d->__pool_put(key.ptr, key.len);
    if ( ofs == doc::__pool_fail ) return mut{};
    const mut self{ d, d->__vals + pi };
    const usize at = self.__append_slots(2);
    if ( at == usize(-1) ) return mut{};
    value *k = d->__vals + at;
    k->tag = make_tag(kind::string, __mut::needs_escape(key.ptr, key.len) ? s_plain : s_noesc, key.len);
    k->pay.ofs = ofs;
    if ( container )
      __put_empty_ctn(k + 1, kd);
    else
      __put_null(k + 1);
    __ctn_bump_len(d->__vals + pi, 1);
    return mut{ d, d->__vals + at + 1 };
  }

public:
  constexpr mut
  insert(strv key) const noexcept
  {
    return __member(key, kind::null, false);
  }

  constexpr mut
  insert(const char *key) const noexcept
  {
    return __member(as_strv(key), kind::null, false);
  }

  template<micron::is_string S>
  constexpr mut
  insert(const S &key) const noexcept
  {
    return __member(as_strv(key), kind::null, false);
  }

  constexpr mut
  insert_object(strv key) const noexcept
  {
    return __member(key, kind::object, true);
  }

  constexpr mut
  insert_object(const char *key) const noexcept
  {
    return __member(as_strv(key), kind::object, true);
  }

  template<micron::is_string S>
  constexpr mut
  insert_object(const S &key) const noexcept
  {
    return __member(as_strv(key), kind::object, true);
  }

  constexpr mut
  insert_array(strv key) const noexcept
  {
    return __member(key, kind::array, true);
  }

  constexpr mut
  insert_array(const char *key) const noexcept
  {
    return __member(as_strv(key), kind::array, true);
  }

  template<micron::is_string S>
  constexpr mut
  insert_array(const S &key) const noexcept
  {
    return __member(as_strv(key), kind::array, true);
  }

  constexpr error
  rename(strv from, strv to) const noexcept
  {
    if ( !__begin_ctn(kind::object) ) return __md() ? __md()->mut_error() : error::no_such_field;
    doc *d = __md();
    const usize ki = __find_key(from);
    if ( ki == usize(-1) ) {
      d->__merr = error::no_such_field;
      return error::no_such_field;
    }
    return mut{ d, d->__vals + ki }.set_key(to);
  }

  constexpr error
  rename(const char *from, const char *to) const noexcept
  {
    return rename(as_strv(from), as_strv(to));
  }

  constexpr error
  erase(strv key) const noexcept
  {
    if ( !__begin_ctn(kind::object) ) return __md() ? __md()->mut_error() : error::no_such_field;
    doc *d = __md();
    const usize pi = __idx();
    const usize ki = mut{ d, d->__vals + pi }.__find_key(key);      // no handle outlives the lookup
    if ( ki == usize(-1) ) {
      d->__merr = error::no_such_field;
      return error::no_such_field;
    }
    // detach only once the erase is certain
    if ( !d->__own_vals(0) ) return d->mut_error();
    const value *k = d->__vals + ki;
    const usize n = usize(get_next(k + 1) - k);      // key slot + value subtree
    __drop_slots(d, pi, ki, n);
    __ctn_bump_len(d->__vals + pi, -1);
    return error::ok;
  }

  constexpr error
  erase(const char *key) const noexcept
  {
    return erase(as_strv(key));
  }

  template<micron::is_string S>
  constexpr error
  erase(const S &key) const noexcept
  {
    return erase(as_strv(key));
  }

  constexpr mut
  push_back() const noexcept
  {
    if ( !__begin_ctn(kind::array) ) return mut{};
    doc *d = __md();
    const usize pi = __idx();
    const usize at = __append_slots(1);
    if ( at == usize(-1) ) return mut{};
    __put_null(d->__vals + at);
    __ctn_bump_len(d->__vals + pi, 1);
    return mut{ d, d->__vals + at };
  }

  constexpr mut
  push_object() const noexcept
  {
    if ( !__begin_ctn(kind::array) ) return mut{};
    doc *d = __md();
    const usize pi = __idx();
    const usize at = __append_slots(1);
    if ( at == usize(-1) ) return mut{};
    __put_empty_ctn(d->__vals + at, kind::object);
    __ctn_bump_len(d->__vals + pi, 1);
    return mut{ d, d->__vals + at };
  }

  constexpr mut
  push_array() const noexcept
  {
    if ( !__begin_ctn(kind::array) ) return mut{};
    doc *d = __md();
    const usize pi = __idx();
    const usize at = __append_slots(1);
    if ( at == usize(-1) ) return mut{};
    __put_empty_ctn(d->__vals + at, kind::array);
    __ctn_bump_len(d->__vals + pi, 1);
    return mut{ d, d->__vals + at };
  }

  constexpr error
  erase(usize i) const noexcept
  {
    if ( !__begin_ctn(kind::array) ) return __md() ? __md()->mut_error() : error::no_such_field;
    doc *d = __md();
    const usize pi = __idx();
    const value *v = d->__vals + pi;
    if ( i >= usize(get_len(*v)) ) {
      d->__merr = error::no_such_field;
      return error::no_such_field;
    }
    const value *e = v + 1;
    for ( usize j = 0; j < i; ++j ) e = get_next(e);
    const usize at = usize(e - d->__vals);
    const usize n = usize(get_next(e) - e);
    // detach only once the erase is certain
    if ( !d->__own_vals(0) ) return d->mut_error();
    __drop_slots(d, pi, at, n);
    __ctn_bump_len(d->__vals + pi, -1);
    return error::ok;
  }

  constexpr error
  clear() const noexcept
  {
    doc *d = __md();
    if ( !d ) return error::no_such_field;
    if ( !*this ) {
      d->__merr = error::no_such_field;
      return error::no_such_field;
    }
    if ( !is_ctn(*__raw()) ) {
      d->__merr = error::wrong_type;
      return error::wrong_type;
    }
    const usize pi = __idx();
    const usize n = __span() - 1;
    if ( !d->__own_vals(0) ) return d->mut_error();
    if ( n != 0 ) __drop_slots(d, pi, pi + 1, n);
    value *p = d->__vals + pi;
    p->tag = make_tag(get_kind(*p), 0, 0);
    p->pay.ofs = sizeof(value);
    d->__wb_invalidate();
    return error::ok;
  }

  constexpr const mut &
  operator=(bool x) const noexcept
  {
    set(x);
    return *this;
  }

  // NOTE: bool is excluded so = true picks the fn above
  template<typename T>
    requires(micron::is_integral_v<T> && !micron::is_same_v<micron::remove_cv_t<T>, bool>)
  constexpr const mut &
  operator=(T x) const noexcept
  {
    if constexpr ( micron::is_signed_v<T> )
      set(i64(x));
    else
      set(u64(x));
    return *this;
  }

  template<typename T>
    requires(micron::is_floating_point_v<T>)
  constexpr const mut &
  operator=(T x) const noexcept
  {
    set(f64(x));
    return *this;
  }

  constexpr const mut &
  operator=(const char *s) const noexcept
  {
    set(as_strv(s));
    return *this;
  }

  constexpr const mut &
  operator=(strv s) const noexcept
  {
    set(s);
    return *this;
  }

  template<micron::is_string S>
  constexpr const mut &
  operator=(const S &s) const noexcept
  {
    set(as_strv(s));
    return *this;
  }

  constexpr const mut &
  operator=(decltype(nullptr)) const noexcept
  {
    set_null();
    return *this;
  }
};

// %%%%%%%%%%%%%%%%%%%%%%
// main entry points
constexpr mut
doc::edit() noexcept
{
  return alive() ? mut{ this, __vals } : mut{};
}

constexpr mut
doc::operator[](strv key) noexcept
{
  return edit()[key];
}

constexpr mut
doc::operator[](const char *key) noexcept
{
  return edit()[key];
}

constexpr mut
doc::operator[](usize i) noexcept
{
  return edit()[i];
}

};      // namespace cjson
