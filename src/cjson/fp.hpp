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
#include "ondemand.hpp"
#include "value.hpp"

#include <micron/concepts.hpp>
#include <micron/function.hpp>
#include <micron/memory/actions.hpp>
#include <micron/type_traits.hpp>
#include <micron/types.hpp>

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// cjson fp
//
// adaptors are lazy

namespace cjson
{

// micron's |> lives in namespace micron; the using-declaration puts it in cjson so ADL
// finds it for cjson-typed ranges (which are not micron types and would otherwise miss)
using micron::operator|;

template<typename V>
concept json_value = requires(const V v, strv k, usize i) {
  { v[k] } -> micron::same_as<V>;
  { v.at(i) } -> micron::same_as<V>;
  { v.type() } -> micron::same_as<kind>;
  { v.i64_or(i64{}) } -> micron::same_as<i64>;
  { v.u64_or(u64{}) } -> micron::same_as<u64>;
  { v.f64_or(f64{}) } -> micron::same_as<f64>;
  { v.bool_or(bool{}) } -> micron::same_as<bool>;
  { static_cast<bool>(v) };
};

static_assert(json_value<val>);
static_assert(json_value<cur>);

template<typename R>
concept json_range = requires(const R &r) {
  { r.begin() };
  { r.end() };
};

template<typename M>
concept json_member = requires(const M &m) {
  { m.key } -> micron::convertible_to<strv>;
  { m.v };
};

namespace __fp
{

template<typename R> using iter_t = decltype(micron::declval<const R &>().begin());

template<typename R> using elem_t = micron::remove_cvref_t<decltype(*micron::declval<const R &>().begin())>;

};      // namespace __fp

constexpr bool
streq(strv a, strv b) noexcept
{
  if ( a.len != b.len ) return false;
  for ( usize i = 0; i < a.len; ++i )
    if ( a.ptr[i] != b.ptr[i] ) return false;
  return true;
}

template<typename T> struct indexed {
  usize i;
  T v;
};

template<typename R, typename F> struct fmap_view {
  R __src;
  F __fn;

  struct iterator {
    __fp::iter_t<R> __it;
    F __fn;

    constexpr auto
    operator*() const
    {
      return __fn(*__it);
    }

    constexpr iterator &
    operator++()
    {
      ++__it;
      return *this;
    }

    constexpr bool
    operator!=(const iterator &o) const
    {
      return __it != o.__it;
    }
  };

  constexpr iterator
  begin() const
  {
    return iterator{ __src.begin(), __fn };
  }

  constexpr iterator
  end() const
  {
    return iterator{ __src.end(), __fn };
  }
};

template<typename R, typename P> struct filter_view {
  R __src;
  P __pred;

  struct iterator {
    __fp::iter_t<R> __it;
    __fp::iter_t<R> __end;
    P __pred;

    constexpr void
    __settle()
    {
      while ( __it != __end and !__pred(*__it) ) ++__it;
    }

    constexpr auto
    operator*() const
    {
      return *__it;
    }

    constexpr iterator &
    operator++()
    {
      ++__it;
      __settle();
      return *this;
    }

    constexpr bool
    operator!=(const iterator &o) const
    {
      return __it != o.__it;
    }
  };

  constexpr iterator
  begin() const
  {
    iterator it{ __src.begin(), __src.end(), __pred };
    it.__settle();
    return it;
  }

  constexpr iterator
  end() const
  {
    return iterator{ __src.end(), __src.end(), __pred };
  }
};

template<typename R> struct take_view {
  R __src;
  usize __n;

  struct iterator {
    __fp::iter_t<R> __it;
    usize __left;

    constexpr auto
    operator*() const
    {
      return *__it;
    }

    constexpr iterator &
    operator++()
    {
      ++__it;
      if ( __left ) --__left;
      return *this;
    }

    constexpr bool
    operator!=(const iterator &o) const
    {
      return __left != 0 and __it != o.__it;
    }
  };

  constexpr iterator
  begin() const
  {
    return iterator{ __src.begin(), __n };
  }

  constexpr iterator
  end() const
  {
    return iterator{ __src.end(), 0 };
  }
};

template<typename R> struct drop_view {
  R __src;
  usize __n;

  constexpr __fp::iter_t<R>
  begin() const
  {
    auto it = __src.begin();
    const auto e = __src.end();
    for ( usize i = 0; i < __n and it != e; ++i ) ++it;
    return it;
  }

  constexpr __fp::iter_t<R>
  end() const
  {
    return __src.end();
  }
};

template<typename R, typename P> struct take_while_view {
  R __src;
  P __pred;

  struct iterator {
    __fp::iter_t<R> __it;
    __fp::iter_t<R> __end;
    P __pred;
    bool __live = false;

    constexpr void
    __check()
    {
      __live = (__it != __end) and __pred(*__it);
    }

    constexpr auto
    operator*() const
    {
      return *__it;
    }

    constexpr iterator &
    operator++()
    {
      ++__it;
      __check();
      return *this;
    }

    constexpr bool
    operator!=(const iterator &o) const
    {
      return __live != o.__live or (__live and __it != o.__it);
    }
  };

  constexpr iterator
  begin() const
  {
    iterator it{ __src.begin(), __src.end(), __pred, false };
    it.__check();
    return it;
  }

  constexpr iterator
  end() const
  {
    return iterator{ __src.end(), __src.end(), __pred, false };
  }
};

template<typename R, typename P> struct drop_while_view {
  R __src;
  P __pred;

  constexpr __fp::iter_t<R>
  begin() const
  {
    auto it = __src.begin();
    const auto e = __src.end();
    while ( it != e and __pred(*it) ) ++it;
    return it;
  }

  constexpr __fp::iter_t<R>
  end() const
  {
    return __src.end();
  }
};

template<typename R> struct enumerate_view {
  R __src;

  struct iterator {
    __fp::iter_t<R> __it;
    usize __i = 0;

    constexpr indexed<__fp::elem_t<R>>
    operator*() const
    {
      return indexed<__fp::elem_t<R>>{ __i, *__it };
    }

    constexpr iterator &
    operator++()
    {
      ++__it;
      ++__i;
      return *this;
    }

    constexpr bool
    operator!=(const iterator &o) const
    {
      return __it != o.__it;
    }
  };

  constexpr iterator
  begin() const
  {
    return iterator{ __src.begin(), 0 };
  }

  constexpr iterator
  end() const
  {
    return iterator{ __src.end(), 0 };
  }
};

template<typename R, typename F> struct flat_map_view {
  R __src;
  F __fn;

  using inner_t = micron::remove_cvref_t<decltype(micron::declval<const F &>()(*micron::declval<const R &>().begin()))>;

  struct iterator {
    __fp::iter_t<R> __out;
    __fp::iter_t<R> __outend;
    F __fn;
    inner_t __inner{};
    __fp::iter_t<inner_t> __in{};
    bool __live = false;

    constexpr void
    __settle()
    {
      while ( __out != __outend ) {
        __inner = __fn(*__out);
        __in = __inner.begin();
        if ( __in != __inner.end() ) {
          __live = true;
          return;
        }
        ++__out;
      }
      __live = false;
    }

    constexpr auto
    operator*() const
    {
      return *__in;
    }

    constexpr iterator &
    operator++()
    {
      ++__in;
      if ( __in != __inner.end() ) return *this;
      ++__out;
      __settle();
      return *this;
    }

    constexpr bool
    operator!=(const iterator &o) const
    {
      return __live != o.__live or (__live and __out != o.__out);
    }
  };

  constexpr iterator
  begin() const
  {
    iterator it{ __src.begin(), __src.end(), __fn, {}, {}, false };
    it.__settle();
    return it;
  }

  constexpr iterator
  end() const
  {
    return iterator{ __src.end(), __src.end(), __fn, {}, {}, false };
  }
};

template<typename F, json_range R>
constexpr auto
fmap(F fn, R r)
{
  return fmap_view<R, F>{ micron::move(r), micron::move(fn) };
}

template<typename P, json_range R>
constexpr auto
filter(P p, R r)
{
  return filter_view<R, P>{ micron::move(r), micron::move(p) };
}

template<typename P, json_range R>
constexpr auto
reject(P p, R r)
{
  auto np = [p = micron::move(p)](const auto &x) { return !p(x); };
  return filter(micron::move(np), micron::move(r));
}

template<json_range R>
constexpr auto
take(usize n, R r)
{
  return take_view<R>{ micron::move(r), n };
}

template<json_range R>
constexpr auto
drop(usize n, R r)
{
  return drop_view<R>{ micron::move(r), n };
}

template<typename P, json_range R>
constexpr auto
take_while(P p, R r)
{
  return take_while_view<R, P>{ micron::move(r), micron::move(p) };
}

template<typename P, json_range R>
constexpr auto
drop_while(P p, R r)
{
  return drop_while_view<R, P>{ micron::move(r), micron::move(p) };
}

template<json_range R>
constexpr auto
enumerate(R r)
{
  return enumerate_view<R>{ micron::move(r) };
}

template<typename F, json_range R>
constexpr auto
flat_map(F fn, R r)
{
  return flat_map_view<R, F>{ micron::move(r), micron::move(fn) };
}

template<json_range R>
constexpr auto
keys(R r)
{
  return fmap([](const auto &m) { return m.key; }, micron::move(r));
}

template<json_range R>
constexpr auto
values(R r)
{
  return fmap([](const auto &m) { return m.v; }, micron::move(r));
}

template<json_range R>
constexpr auto
pluck(strv key, R r)
{
  return fmap([key](const auto &v) { return v[key]; }, micron::move(r));
}

template<json_range R>
constexpr auto
pluck(const char *key, R r)
{
  return pluck(as_strv(key), micron::move(r));
}

template<micron::is_string S, json_range R>
constexpr auto
pluck(const S &key, R r)
{
  return pluck(strv{ key.c_str(), key.size() }, micron::move(r));
}

template<json_range R, typename A, typename F>
constexpr A
fold(const R &r, A init, F fn)
{
  A acc = micron::move(init);
  for ( auto it = r.begin(), e = r.end(); it != e; ++it ) acc = fn(acc, *it);
  return acc;
}

template<json_range R>
constexpr usize
count(const R &r)
{
  usize n = 0;
  for ( auto it = r.begin(), e = r.end(); it != e; ++it ) ++n;
  return n;
}

template<typename P, json_range R>
constexpr usize
count_if(P p, const R &r)
{
  usize n = 0;
  for ( auto it = r.begin(), e = r.end(); it != e; ++it )
    if ( p(*it) ) ++n;
  return n;
}

template<typename P, json_range R>
constexpr bool
any_of(P p, const R &r)
{
  for ( auto it = r.begin(), e = r.end(); it != e; ++it )
    if ( p(*it) ) return true;
  return false;
}

template<typename P, json_range R>
constexpr bool
all_of(P p, const R &r)
{
  for ( auto it = r.begin(), e = r.end(); it != e; ++it )
    if ( !p(*it) ) return false;
  return true;
}

template<typename P, json_range R>
constexpr bool
none_of(P p, const R &r)
{
  return !any_of(p, r);
}

template<typename P, json_range R>
constexpr result<__fp::elem_t<R>>
find_first(P p, const R &r)
{
  using T = __fp::elem_t<R>;
  for ( auto it = r.begin(), e = r.end(); it != e; ++it )
    if ( p(*it) ) return result<T>{ micron::tag<T>{}, *it };
  return result<T>{ micron::tag<error>{}, error::no_such_field };
}

template<typename Proj, json_range R>
constexpr result<__fp::elem_t<R>>
max_by(Proj proj, const R &r)
{
  using T = __fp::elem_t<R>;
  auto it = r.begin();
  const auto e = r.end();
  if ( !(it != e) ) return result<T>{ micron::tag<error>{}, error::no_such_field };
  T best = *it;
  auto bestk = proj(best);
  for ( ++it; it != e; ++it ) {
    auto k = proj(*it);
    if ( bestk < k ) {
      bestk = k;
      best = *it;
    }
  }
  return result<T>{ micron::tag<T>{}, best };
}

template<typename Proj, json_range R>
constexpr result<__fp::elem_t<R>>
min_by(Proj proj, const R &r)
{
  using T = __fp::elem_t<R>;
  auto it = r.begin();
  const auto e = r.end();
  if ( !(it != e) ) return result<T>{ micron::tag<error>{}, error::no_such_field };
  T best = *it;
  auto bestk = proj(best);
  for ( ++it; it != e; ++it ) {
    auto k = proj(*it);
    if ( k < bestk ) {
      bestk = k;
      best = *it;
    }
  }
  return result<T>{ micron::tag<T>{}, best };
}

template<typename F, json_range R>
constexpr void
for_each(F fn, const R &r)
{
  for ( auto it = r.begin(), e = r.end(); it != e; ++it ) fn(*it);
}

template<typename C, json_range R>
inline C
collect_into(const R &r)
{
  C out{};
  for ( auto it = r.begin(), e = r.end(); it != e; ++it ) out.push_back(*it);
  return out;
}

template<typename F>
constexpr auto
fmap_c(F fn)
{
  return [fn = micron::move(fn)](auto r) { return fmap(fn, micron::move(r)); };
}

template<typename P>
constexpr auto
filter_c(P p)
{
  return [p = micron::move(p)](auto r) { return filter(p, micron::move(r)); };
}

template<typename P>
constexpr auto
reject_c(P p)
{
  return [p = micron::move(p)](auto r) { return filter([p](const auto &x) { return !p(x); }, micron::move(r)); };
}

inline constexpr auto
take_c(usize n)
{
  return [n](auto r) { return take(n, micron::move(r)); };
}

inline constexpr auto
drop_c(usize n)
{
  return [n](auto r) { return drop(n, micron::move(r)); };
}

template<typename P>
constexpr auto
take_while_c(P p)
{
  return [p = micron::move(p)](auto r) { return take_while(p, micron::move(r)); };
}

template<typename P>
constexpr auto
drop_while_c(P p)
{
  return [p = micron::move(p)](auto r) { return drop_while(p, micron::move(r)); };
}

inline constexpr auto
enumerate_c()
{
  return [](auto r) { return enumerate(micron::move(r)); };
}

template<typename F>
constexpr auto
flat_map_c(F fn)
{
  return [fn = micron::move(fn)](auto r) { return flat_map(fn, micron::move(r)); };
}

inline constexpr auto
keys_c()
{
  return [](auto r) { return keys(micron::move(r)); };
}

inline constexpr auto
values_c()
{
  return [](auto r) { return values(micron::move(r)); };
}

inline constexpr auto
pluck_c(strv key)
{
  return [key](auto r) { return pluck(key, micron::move(r)); };
}

inline constexpr auto
pluck_c(const char *key)
{
  return pluck_c(as_strv(key));
}

template<micron::is_string S>
constexpr auto
pluck_c(const S &key)
{
  return pluck_c(strv{ key.c_str(), key.size() });
}

template<typename A, typename F>
constexpr auto
fold_c(A init, F fn)
{
  return [init = micron::move(init), fn = micron::move(fn)](auto r) { return fold(r, init, fn); };
}

inline constexpr auto
count_c()
{
  return [](auto r) { return count(r); };
}

template<typename P>
constexpr auto
count_if_c(P p)
{
  return [p = micron::move(p)](auto r) { return count_if(p, r); };
}

template<typename P>
constexpr auto
any_of_c(P p)
{
  return [p = micron::move(p)](auto r) { return any_of(p, r); };
}

template<typename P>
constexpr auto
all_of_c(P p)
{
  return [p = micron::move(p)](auto r) { return all_of(p, r); };
}

template<typename P>
constexpr auto
none_of_c(P p)
{
  return [p = micron::move(p)](auto r) { return none_of(p, r); };
}

template<typename P>
constexpr auto
find_first_c(P p)
{
  return [p = micron::move(p)](auto r) { return find_first(p, r); };
}

template<typename Proj>
constexpr auto
max_by_c(Proj proj)
{
  return [proj = micron::move(proj)](auto r) { return max_by(proj, r); };
}

template<typename Proj>
constexpr auto
min_by_c(Proj proj)
{
  return [proj = micron::move(proj)](auto r) { return min_by(proj, r); };
}

template<typename F>
constexpr auto
for_each_c(F fn)
{
  return [fn = micron::move(fn)](auto r) {
    for_each(fn, r);
    return r;
  };
}

template<typename C>
constexpr auto
collect_into_c()
{
  return [](auto r) { return collect_into<C>(r); };
}

inline constexpr auto plus = [](auto a, auto b) constexpr noexcept { return a + b; };
inline constexpr auto minus = [](auto a, auto b) constexpr noexcept { return a - b; };
inline constexpr auto times = [](auto a, auto b) constexpr noexcept { return a * b; };
inline constexpr auto max_of = [](auto a, auto b) constexpr noexcept { return a < b ? b : a; };
inline constexpr auto min_of = [](auto a, auto b) constexpr noexcept { return b < a ? b : a; };

inline constexpr auto to_i64 = [](const auto &v) constexpr noexcept { return v.i64_or(i64(0)); };
inline constexpr auto to_u64 = [](const auto &v) constexpr noexcept { return v.u64_or(u64(0)); };
inline constexpr auto to_f64 = [](const auto &v) constexpr noexcept { return v.f64_or(f64(0)); };
inline constexpr auto to_bool = [](const auto &v) constexpr noexcept { return v.bool_or(false); };

inline constexpr auto to_str = [](const auto &v) constexpr noexcept -> strv {
  if constexpr ( requires { v.str_or(); } )
    return v.str_or();
  else
    return v.str_raw();
};

inline constexpr auto is_truthy = [](const auto &v) constexpr noexcept {
  return static_cast<bool>(v) and v.type() != kind::null and !(v.type() == kind::boolean and !v.bool_or(false));
};

inline constexpr auto
is_kind(kind k)
{
  return [k](const auto &v) constexpr noexcept { return v.type() == k; };
}

inline constexpr auto
has(strv key)
{
  return [key](const auto &v) constexpr noexcept { return static_cast<bool>(v[key]); };
}

inline constexpr auto
has(const char *key)
{
  return has(as_strv(key));
}

template<micron::is_string S>
constexpr auto
has(const S &key)
{
  return has(strv{ key.c_str(), key.size() });
}

inline constexpr auto
key_is(strv k)
{
  return [k](const auto &m) constexpr noexcept { return streq(m.key, k); };
}

inline constexpr auto
key_is(const char *k)
{
  return key_is(as_strv(k));
}

template<micron::is_string S>
constexpr auto
key_is(const S &k)
{
  return key_is(strv{ k.c_str(), k.size() });
}

inline constexpr auto
field(strv key)
{
  return [key](const auto &v) constexpr noexcept { return v[key]; };
}

inline constexpr auto
field(const char *key)
{
  return field(as_strv(key));
}

template<micron::is_string S>
constexpr auto
field(const S &key)
{
  return field(strv{ key.c_str(), key.size() });
}

inline constexpr auto
field_i64(strv key)
{
  return [key](const auto &v) constexpr noexcept { return v[key].i64_or(i64(0)); };
}

inline constexpr auto
field_i64(const char *key)
{
  return field_i64(as_strv(key));
}

inline constexpr auto
field_u64(strv key)
{
  return [key](const auto &v) constexpr noexcept { return v[key].u64_or(u64(0)); };
}

inline constexpr auto
field_u64(const char *key)
{
  return field_u64(as_strv(key));
}

inline constexpr auto
field_f64(strv key)
{
  return [key](const auto &v) constexpr noexcept { return v[key].f64_or(f64(0)); };
}

inline constexpr auto
field_f64(const char *key)
{
  return field_f64(as_strv(key));
}

inline constexpr auto
field_bool(strv key)
{
  return [key](const auto &v) constexpr noexcept { return v[key].bool_or(false); };
}

inline constexpr auto
field_bool(const char *key)
{
  return field_bool(as_strv(key));
}

};      // namespace cjson
