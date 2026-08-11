//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#pragma once

#include "builder.hpp"
#include "config.hpp"
#include "doc.hpp"
#include "error.hpp"
#include "ondemand.hpp"
#include "pun.hpp"
#include "write.hpp"

#include <micron/concepts.hpp>
#include <micron/maps/heap_swiss.hpp>
#include <micron/vector.hpp>

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// cjson porcelain: json <-> micron containers
//
// keys are always owned copies; a vref is valid exactly as long as its doc is (and until the next structural edit); a jraw is valid as long
// as input text is; runtime only

namespace cjson
{

struct key_view {
  using value_type = char;
  using pointer = const char *;
  using const_pointer = const char *;
  using reference = const char &;
  using const_reference = const char &;
  using iterator = const char *;
  using const_iterator = const char *;
  using size_type = usize;

  const char *__p = nullptr;
  usize __n = 0;

  constexpr key_view() = default;

  constexpr key_view(strv s) noexcept : __p(s.ptr), __n(s.len) { }

  constexpr pointer
  data() const noexcept
  {
    return __p;
  }

  constexpr iterator
  begin() const noexcept
  {
    return __p;
  }

  constexpr iterator
  end() const noexcept
  {
    return __p + __n;
  }

  constexpr const_iterator
  cbegin() const noexcept
  {
    return __p;
  }

  constexpr const_iterator
  cend() const noexcept
  {
    return __p + __n;
  }

  constexpr size_type
  size() const noexcept
  {
    return __n;
  }

  constexpr strv
  view() const noexcept
  {
    return strv{ __p, __n };
  }

  constexpr bool
  operator==(const key_view &o) const noexcept
  {
    if ( __n != o.__n ) return false;
    for ( usize i = 0; i < __n; ++i )
      if ( __p[i] != o.__p[i] ) return false;
    return true;
  }
};

// constraints
template<typename K>
concept pun_key
    = (micron::is_string<K> || micron::is_string_on_stack<K> || micron::same_as<K, key_view>) && micron::is_move_constructible_v<K>;

template<typename M>
concept kv_insertable = requires(M &m, const typename micron::remove_cvref_t<M>::key_type &ck,
                                 typename micron::remove_cvref_t<M>::key_type &&k, typename micron::remove_cvref_t<M>::mapped_type &&v) {
  { m.find(ck) } -> micron::same_as<typename micron::remove_cvref_t<M>::mapped_type *>;
  { m.insert(micron::move(k), micron::move(v)) };
  { m.size() } -> micron::convertible_to<usize>;
};

template<typename M>
concept pun_map = micron::is_map_class<micron::remove_cvref_t<M>> && micron::is_mutable_map<micron::remove_cvref_t<M>> && kv_insertable<M>
                  && pun_key<typename micron::remove_cvref_t<M>::key_type>
                  && micron::is_constructible_v<typename micron::remove_cvref_t<M>::mapped_type, pun &&>;

template<typename V>
concept pun_seq = micron::is_iterable_container<micron::remove_cvref_t<V>>
                  && micron::is_constructible_v<typename micron::remove_cvref_t<V>::value_type, pun &&> && requires(V &v, pun &&p) {
                       v.push_back(micron::move(p));
                       v.reserve(usize{});
                     };

using object_map = micron::hswiss<micron::string, pun>;
// using safe vec, not unsafe
using array_vec = micron::vector<pun>;

constexpr usize
map_slots(val v) noexcept
{
  const usize n = v.type() == kind::object ? v.size() : 0;
  const usize s = n * 8 / 7 + 1;
  return s < 16 ? 16 : s;
}

namespace __porc
{

template<pun_key K>
inline K
make_key(strv s)
{
  if constexpr ( micron::same_as<K, key_view> ) {
    return key_view{ s };
  } else {
    K k{};
    k.append(s.ptr, s.len);
    return k;
  }
}

inline micron::string
subtree(vref r)
{
  return write_str(as_val(r));
}

};      // namespace __porc

// %%%%%%%%%%%%%%%%%%%%%%%%%
// json -> micron

template<pun_map M>
inline error
to_map_into(val v, M &out)
{
  if ( !v ) return error::no_such_field;
  if ( v.type() != kind::object ) return error::wrong_type;
  using K = typename micron::remove_cvref_t<M>::key_type;
  for ( auto m : v.members() ) {
    K k = __porc::make_key<K>(m.key);
    if ( out.find(k) != nullptr ) continue;      // first wins
    out.insert(micron::move(k), pun(m.v));
  }
  return error::ok;
}

template<pun_map M>
inline error
to_map_into(cur c, M &out)
{
  if ( !c ) return error::no_such_field;
  if ( c.type() != kind::object ) return error::wrong_type;
  using K = typename micron::remove_cvref_t<M>::key_type;
  for ( auto m : c.members() ) {
    K k = __porc::make_key<K>(m.key);
    if ( out.find(k) != nullptr ) continue;
    out.insert(micron::move(k), pun(m.v));
  }
  return error::ok;
}

template<pun_seq V>
inline error
to_vector_into(val v, V &out)
{
  if ( !v ) return error::no_such_field;
  if ( v.type() != kind::array ) return error::wrong_type;
  out.reserve(v.size());
  for ( auto e : v.items() ) out.push_back(pun(e));
  return error::ok;
}

template<pun_seq V>
inline error
to_vector_into(cur c, V &out)
{
  if ( !c ) return error::no_such_field;
  if ( c.type() != kind::array ) return error::wrong_type;
  for ( auto e : c.items() ) out.push_back(pun(e));
  return error::ok;
}

inline result<object_map>
to_map(val v)
{
  object_map m{};
  if ( v.type() == kind::object ) m.reserve(v.size() + 1);
  const error e = to_map_into(v, m);
  if ( e != error::ok ) return result<object_map>{ micron::tag<error>{}, e };
  return result<object_map>{ micron::move(m) };
}

inline result<object_map>
to_map(cur c)
{
  object_map m{};
  const error e = to_map_into(c, m);
  if ( e != error::ok ) return result<object_map>{ micron::tag<error>{}, e };
  return result<object_map>{ micron::move(m) };
}

inline result<array_vec>
to_vector(val v)
{
  array_vec a{};
  const error e = to_vector_into(v, a);
  if ( e != error::ok ) return result<array_vec>{ micron::tag<error>{}, e };
  return result<array_vec>{ micron::move(a) };
}

inline result<array_vec>
to_vector(cur c)
{
  array_vec a{};
  const error e = to_vector_into(c, a);
  if ( e != error::ok ) return result<array_vec>{ micron::tag<error>{}, e };
  return result<array_vec>{ micron::move(a) };
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// micron -> json

template<pun_seq V> inline error write_seq(builder &b, const V &v);

template<pun_map M> inline error write_map(builder &b, const M &m);

inline void
write_pun(builder &b, const pun &p)
{
  if ( !p.has_value() ) {
    b.null();      // a valueless any is a dead value
    return;
  }
  p.visit([&b](const auto &x) {
    using T = micron::remove_cvref_t<decltype(x)>;
    if constexpr ( micron::is_same_v<T, jnull> )
      b.null();
    else if constexpr ( micron::is_same_v<T, jraw> )
      b.raw(x.text);
    else if constexpr ( micron::is_same_v<T, vref> ) {
      const micron::string sub = __porc::subtree(x);
      b.raw(strv{ sub.c_str(), sub.size() });
    } else
      b.value(x);
  });
}

template<pun_map M>
inline error
write_map(builder &b, const M &m)
{
  b.obj();
  m.for_each([&b](const auto &k, const auto &v) {
    if constexpr ( micron::same_as<micron::remove_cvref_t<decltype(k)>, key_view> )
      b.key(k.view());
    else
      b.key(strv{ k.c_str(), k.size() });
    write_pun(b, v);
  });
  b.end();
  return b.err();
}

template<pun_seq V>
inline error
write_seq(builder &b, const V &v)
{
  b.arr();
  for ( const auto &e : v ) write_pun(b, e);
  b.end();
  return b.err();
}

template<pun_map M>
inline result<micron::string>
to_json(const M &m)
{
  builder b;
  if ( const error e = write_map(b, m); e != error::ok ) return result<micron::string>{ micron::tag<error>{}, e };
  return result<micron::string>{ b.take() };
}

template<pun_seq V>
inline result<micron::string>
to_json_seq(const V &v)
{
  builder b;
  if ( const error e = write_seq(b, v); e != error::ok ) return result<micron::string>{ micron::tag<error>{}, e };
  return result<micron::string>{ b.take() };
}

};      // namespace cjson
