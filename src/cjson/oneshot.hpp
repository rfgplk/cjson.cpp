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
#include "ondemand.hpp"
#include "parse.hpp"
#include "scratch.hpp"
#include "write.hpp"

#include <micron/string/strings.hpp>
#include <micron/sum.hpp>
#include <micron/type_traits.hpp>
#include <micron/types.hpp>

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// cjson oneshot
//
//   cjson::get<i64>(text, "/a/b")      one shot; owns everything, frees on return
//   cjson::get<i64>(text, "/a/b", sc)  same, on a caller-owned warm scratch
//   cjson::get(text, "/a/b", sc)       marshalls via any
//   cjson::iterate / cjson::parse      layered API, when you want the cursor
//
// NOTE: everything except pretty/compact/reformat runs on the on-demand path

namespace cjson
{

struct jtext {
  bytes b{};

  constexpr jtext(bytes v) noexcept : b(v) { }

  constexpr jtext(wbytes v) noexcept : b{ v.ptr, v.len } { }

  constexpr jtext(strv v) noexcept : b{ reinterpret_cast<const u8 *>(v.ptr), v.len } { }

  // NUL-terminated: the literal case
  constexpr jtext(const char *p) noexcept : b{ reinterpret_cast<const u8 *>(p), as_strv(p).len } { }

  template<text_source C> jtext(const C &c) noexcept : b(as_bytes(c)) { }
};

// an rfc 6901 pointer
struct jptr {
  strv s{};

  constexpr jptr() = default;

  constexpr jptr(strv v) noexcept : s(v) { }

  constexpr jptr(const char *p) noexcept : s(as_strv(p)) { }

  template<micron::is_string S> constexpr jptr(const S &v) noexcept : s{ v.c_str(), v.size() } { }
};

namespace __oneshot
{

// every oneshot goes through here
template<typename Fn>
inline auto
at(jtext in, jptr ptr, opts o, scratch &sc, Fn fn) -> decltype(fn(cur{}))
{
  auto rv = iterate(in.b, o, sc);
  if ( rv.is_second() ) return fn(cur{});
  const view v = rv.cast<view>();
  return fn(v.root().at_pointer(ptr.s));
}

template<typename T> struct getter;

template<> struct getter<i64> {
  static constexpr result<i64>
  from(cur c) noexcept
  {
    if ( !c ) return result<i64>{ micron::tag<error>{}, error::no_such_field };
    if ( c.type() != kind::number ) return result<i64>{ micron::tag<error>{}, error::wrong_type };
    return result<i64>{ micron::tag<i64>{}, c.i64_or(0) };
  }
};

template<> struct getter<u64> {
  static constexpr result<u64>
  from(cur c) noexcept
  {
    if ( !c ) return result<u64>{ micron::tag<error>{}, error::no_such_field };
    if ( c.type() != kind::number ) return result<u64>{ micron::tag<error>{}, error::wrong_type };
    return result<u64>{ micron::tag<u64>{}, c.u64_or(0) };
  }
};

template<> struct getter<f64> {
  static constexpr result<f64>
  from(cur c) noexcept
  {
    if ( !c ) return result<f64>{ micron::tag<error>{}, error::no_such_field };
    if ( c.type() != kind::number ) return result<f64>{ micron::tag<error>{}, error::wrong_type };
    return result<f64>{ micron::tag<f64>{}, c.f64_or(0) };
  }
};

template<> struct getter<bool> {
  static constexpr result<bool>
  from(cur c) noexcept
  {
    if ( !c ) return result<bool>{ micron::tag<error>{}, error::no_such_field };
    if ( c.type() != kind::boolean ) return result<bool>{ micron::tag<error>{}, error::wrong_type };
    return result<bool>{ micron::tag<bool>{}, c.bool_or(false) };
  }
};

// owning + escape-decoded: the scratch does not survive the call
template<> struct getter<micron::string> {
  static inline result<micron::string>
  from(cur c)
  {
    if ( !c ) return result<micron::string>{ micron::tag<error>{}, error::no_such_field };
    if ( c.type() != kind::string ) return result<micron::string>{ micron::tag<error>{}, error::wrong_type };
    const strv raw = c.str_raw();
    micron::string s{};
    s.reserve(raw.len + 1);
    const max_t w = c.str(wbytes{ reinterpret_cast<u8 *>(s.data()), raw.len });
    if ( w < 0 ) return result<micron::string>{ micron::tag<error>{}, as_error(w) };
    s.set_size(usize(w));
    return result<micron::string>{ micron::tag<micron::string>{}, micron::move(s) };
  }
};

};      // namespace __oneshot

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// get / get_or

template<typename T>
inline result<T>
get(jtext in, jptr ptr, opts o, scratch &sc)
{
  return __oneshot::at(in, ptr, o, sc, [](cur c) { return __oneshot::getter<T>::from(c); });
}

template<typename T>
inline result<T>
get(jtext in, jptr ptr, scratch &sc)
{
  return get<T>(in, ptr, opts{}, sc);
}

template<typename T>
inline result<T>
get(jtext in, jptr ptr, opts o = {})
{
  scratch sc{};
  return get<T>(in, ptr, o, sc);
}

inline result<pun>
get(jtext in, jptr ptr, opts o, scratch &sc)
{
  return __oneshot::at(in, ptr, o, sc, [](cur c) { return pun(c); });
}

inline result<pun>
get(jtext in, jptr ptr, scratch &sc)
{
  return get(in, ptr, opts{}, sc);
}

inline result<pun>
get(jtext in, jptr ptr, opts o = {})
{
  scratch sc{};
  return get(in, ptr, o, sc);
}

template<typename T>
inline T
get_or(jtext in, jptr ptr, T dflt, opts o = {})
{
  auto r = get<T>(in, ptr, o);
  return r.is_first() ? r.template cast<T>() : dflt;
}

template<typename T>
inline T
get_or(jtext in, jptr ptr, T dflt, opts o, scratch &sc)
{
  auto r = get<T>(in, ptr, o, sc);
  return r.is_first() ? r.template cast<T>() : dflt;
}

// borrowed, undecoded string
inline result<strv>
get_str_raw(jtext in, jptr ptr, opts o, scratch &sc)
{
  return __oneshot::at(in, ptr, o, sc, [](cur c) {
    if ( !c ) return result<strv>{ micron::tag<error>{}, error::no_such_field };
    if ( c.type() != kind::string ) return result<strv>{ micron::tag<error>{}, error::wrong_type };
    return result<strv>{ micron::tag<strv>{}, c.str_raw() };
  });
}

inline result<strv>
get_str_raw(jtext in, jptr ptr, scratch &sc)
{
  return get_str_raw(in, ptr, opts{}, sc);
}

// predicates
inline bool
exists(jtext in, jptr ptr, opts o, scratch &sc)
{
  return __oneshot::at(in, ptr, o, sc, [](cur c) { return static_cast<bool>(c); });
}

inline bool
exists(jtext in, jptr ptr, opts o = {})
{
  scratch sc{};
  return exists(in, ptr, o, sc);
}

// kind::none for a miss OR a malformed document
inline kind
kind_at(jtext in, jptr ptr, opts o, scratch &sc)
{
  return __oneshot::at(in, ptr, o, sc, [](cur c) { return c.type(); });
}

inline kind
kind_at(jtext in, jptr ptr, opts o = {})
{
  scratch sc{};
  return kind_at(in, ptr, o, sc);
}

// array elements / object pairs at ptr
inline result<usize>
count_at(jtext in, jptr ptr, opts o, scratch &sc)
{
  return __oneshot::at(in, ptr, o, sc, [](cur c) {
    if ( !c ) return result<usize>{ micron::tag<error>{}, error::no_such_field };
    const kind k = c.type();
    if ( k != kind::array and k != kind::object ) return result<usize>{ micron::tag<error>{}, error::wrong_type };
    return result<usize>{ micron::tag<usize>{}, c.count() };
  });
}

inline result<usize>
count_at(jtext in, jptr ptr, opts o = {})
{
  scratch sc{};
  return count_at(in, ptr, o, sc);
}

template<typename Fn>
inline error
each(jtext in, jptr ptr, opts o, scratch &sc, Fn fn)
{
  return __oneshot::at(in, ptr, o, sc, [&](cur c) {
    if ( !c ) return error::no_such_field;
    if ( c.type() == kind::array ) {
      if constexpr ( micron::is_invocable_v<Fn, cur> ) {
        for ( auto e : c.items() ) fn(e);
        return error::ok;
      } else {
        return error::wrong_type;      // fn only accepts members; an array has none
      }
    }
    if ( c.type() == kind::object ) {
      if constexpr ( micron::is_invocable_v<Fn, cur_member> ) {
        for ( auto m : c.members() ) fn(m);
        return error::ok;
      } else if constexpr ( micron::is_invocable_v<Fn, cur> ) {
        for ( auto m : c.members() ) fn(m.v);
        return error::ok;
      } else {
        return error::wrong_type;
      }
    }
    return error::wrong_type;
  });
}

template<typename Fn>
inline error
each(jtext in, jptr ptr, Fn fn)
{
  scratch sc{};
  return each(in, ptr, opts{}, sc, fn);
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// whole-document reshaping

inline bool
valid(jtext in, opts o = {}) noexcept
{
  return validate(in.b, o) == error::ok;
}

inline result<micron::string>
compact(jtext in, opts o = {})
{
  return minify_str(in.b, o);
}

inline result<micron::string>
reformat(jtext in, style st, opts o = {})
{
  auto r = parse(in.b, o);
  if ( r.is_second() ) return result<micron::string>{ micron::tag<error>{}, r.cast<error>() };
  return result<micron::string>{ micron::tag<micron::string>{}, write_str(r.cast<doc>(), st) };
}

inline result<micron::string>
pretty(jtext in, u8 indent = 2, opts o = {})
{
  return reformat(in, style{ .indent = indent }, o);
}
};      // namespace cjson
