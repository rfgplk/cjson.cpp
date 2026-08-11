//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include "../src/cjson/cjson.hpp"

#include "tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/types.hpp>
#include <micron/vector.hpp>

// mutation semantics: scalars rewrite one slot whatever their kind, strings may grow into
// the pool, structural edits splice the tape and repatch only the ancestor chain, and the
// cached writer bound stays exact through all of it — a stale-low bound is a heap overflow
// in write_str, not a wrong answer, so the bound oracle here is the load-bearing test

namespace
{

bool
same(cjson::strv got, const char *want)
{
  usize n = 0;
  while ( want[n] ) ++n;
  if ( got.len != n ) return false;
  for ( usize i = 0; i < n; ++i )
    if ( got.ptr[i] != want[i] ) return false;
  return true;
}

bool
str_is(const micron::string &got, const char *want)
{
  return same(cjson::strv{ got.c_str(), got.size() }, want);
}

cjson::result<cjson::doc>
P(const char *s)
{
  usize n = 0;
  while ( s[n] ) ++n;
  return cjson::parse(s, n);
}

cjson::result<cjson::doc>
PR(cjson::scratch &sc, const char *s)
{
  usize n = 0;
  while ( s[n] ) ++n;
  return cjson::parse_reuse(s, n, cjson::opts{}, sc);
}

// the bound oracle: write_bound must never under-report, at every style, and the cached
// bound must still cover what a freshly invalidated doc would compute by walking
bool
bound_holds(const cjson::doc &d)
{
  for ( u8 ind : { u8(0), u8(2), u8(4) } ) {
    const cjson::style st{ .indent = ind };
    const usize b = cjson::write_bound(d, st);
    const micron::string s = cjson::write_str(d, st);
    if ( s.size() > b ) return false;
  }
  return true;
}

// descend a few random levels and hand back whatever handle we land on.
//
// note this returns by value rather than reassigning a local: mut deliberately deletes
// copy-assignment, so that `a = b` cannot silently re-seat a handle when the author meant
// to write a value through it. copy-INITIALISATION (and so parameter passing) is fine
cjson::mut
descend(cjson::mut v, tutil::rng &rg, int hops)
{
  if ( hops == 0 or !v or !cjson::is_ctn(*v.__raw()) ) return v;
  const usize n = v.size();
  return descend(v[usize(rg.below(u32(n ? n : 1)))], rg, hops - 1);
}

// mutate, then prove the emitted text is exactly `want` and reparses
bool
emits(const cjson::doc &d, const char *want)
{
  if ( !bound_holds(d) ) return false;
  const micron::string s = cjson::write_str(d);
  auto r = cjson::parse(s.c_str(), s.size());
  if ( r.is_second() ) return false;
  return str_is(s, want);
}

// comptime twins. these drive __parse_into directly: micron::option (and so result<doc>)
// is runtime-only by design, but every mutator must survive constant evaluation
template<usize N>
consteval i64
ct_set(const char (&s)[N], const char *key, i64 v)
{
  u8 *tmp = new u8[N - 1];
  for ( usize i = 0; i + 1 < N; ++i ) tmp[i] = u8(s[i]);
  i64 out = -777;
  {
    cjson::scratch sc{};
    cjson::doc d{};
    if ( cjson::__parse_into(d, cjson::bytes{ tmp, N - 1 }, {}, sc) > 0 ) {
      usize kl = 0;
      while ( key[kl] ) ++kl;
      d[cjson::strv{ key, kl }] = v;
      out = d.root()[cjson::strv{ key, kl }].i64_or(-888);
    }
  }
  delete[] tmp;
  return out;
}

// a GROWING string set at comptime: exercises the transient new[]/delete[] pool path
template<usize N>
consteval usize
ct_grow(const char (&s)[N], const char *key, const char *nv)
{
  u8 *tmp = new u8[N - 1];
  for ( usize i = 0; i + 1 < N; ++i ) tmp[i] = u8(s[i]);
  usize out = 0;
  {
    cjson::scratch sc{};
    cjson::doc d{};
    if ( cjson::__parse_into(d, cjson::bytes{ tmp, N - 1 }, {}, sc) > 0 ) {
      usize kl = 0;
      while ( key[kl] ) ++kl;
      d[cjson::strv{ key, kl }] = nv;
      out = d.root()[cjson::strv{ key, kl }].size();
    }
  }
  delete[] tmp;
  return out;
}

// a structural edit at comptime, plus the compile-time proof that the incremental bound
// still covers the emitted bytes
template<usize N>
consteval bool
ct_structural(const char (&s)[N])
{
  u8 *tmp = new u8[N - 1];
  for ( usize i = 0; i + 1 < N; ++i ) tmp[i] = u8(s[i]);
  bool ok = false;
  {
    cjson::scratch sc{};
    cjson::doc d{};
    if ( cjson::__parse_into(d, cjson::bytes{ tmp, N - 1 }, {}, sc) > 0 ) {
      d.edit().insert("zz") = i64(5);
      d["arr"].push_back() = i64(9);
      ok = d.root().size() == 3 and d["arr"].size() == 3 and d["zz"].i64_or(0) == 5 and d["arr"][usize(2)].i64_or(0) == 9;
      const usize b = cjson::write_bound(d);
      u8 *ob = new u8[b];
      const max_t w = cjson::write_into(d, cjson::wbytes{ ob, b });
      ok = ok and w > 0 and usize(w) <= b;
      delete[] ob;
    }
  }
  delete[] tmp;
  return ok;
}

static_assert(ct_set(R"({"port":8080})", "port", 42) == 42);
static_assert(ct_set(R"({"port":8080,"x":1})", "port", -9) == -9);
static_assert(ct_grow(R"({"k":"ab"})", "k", "much longer than before") == 23);
static_assert(ct_structural(R"({"a":1,"arr":[1,2]})"));

};      // namespace

int
main()
{
  {
    sb::test_case("a scalar becomes any other scalar in place, whatever its kind");
    auto r = P(R"({"n":1,"s":"ab","b":true,"z":null,"f":1.5})");
    sb::require_true(r.is_first());
    cjson::doc &d = r.cast<cjson::doc>();

    d["n"] = 7;
    sb::require(d["n"].i64_or(0), i64(7));
    d["n"] = -7;      // promotes s_uint -> s_sint
    sb::require(d["n"].i64_or(0), i64(-7));
    d["n"] = 2.5;      // -> s_real
    sb::require_true(d["n"].f64_or(0) == 2.5);
    d["n"] = "now a string";
    sb::require_true(same(d["n"].str_or(), "now a string"));
    d["n"] = true;
    sb::require_true(d["n"].bool_or(false));
    d["n"] = nullptr;
    sb::require_true(d["n"].is_null());
    d["n"] = u64(18446744073709551615ull);
    sb::require(d["n"].u64_or(0), u64(18446744073709551615ull));

    sb::require_true(emits(d, R"({"n":18446744073709551615,"s":"ab","b":true,"z":null,"f":1.5})"));
    sb::require(int(d.mut_error()), int(cjson::error::ok));
    sb::end_test_case();
  }
  {
    sb::test_case("a string may shrink in place or grow into the pool, and stays exact either way");
    auto r = P(R"({"k":"the original value","n":1})");
    sb::require_true(r.is_first());
    cjson::doc &d = r.cast<cjson::doc>();

    d["k"] = "tiny";      // shrink: the tag length must shrink with it
    sb::require_true(same(d["k"].str_or(), "tiny"));
    sb::require_true(emits(d, R"({"k":"tiny","n":1})"));

    d["k"] = "far longer than the slot ever held, forcing a pool append";
    sb::require_true(same(d["k"].str_or(), "far longer than the slot ever held, forcing a pool append"));
    sb::require_true(emits(d, R"({"k":"far longer than the slot ever held, forcing a pool append","n":1})"));
    sb::require_true(d["k"].__subtype() == cjson::s_noesc);
    sb::end_test_case();
  }
  {
    sb::test_case("setting text with a quote, a backslash or a control byte reflips the escape subtype");
    auto r = P(R"({"k":"plain"})");
    cjson::doc &d = r.cast<cjson::doc>();

    d["k"] = "a\"b";
    sb::require_true(d["k"].__subtype() == cjson::s_plain);
    sb::require_true(emits(d, R"({"k":"a\"b"})"));
    d["k"] = "c\\d";
    sb::require_true(emits(d, R"({"k":"c\\d"})"));
    d["k"] = "e\nf";
    sb::require_true(emits(d, R"({"k":"e\nf"})"));
    d["k"] = "back to clean";      // and back again
    sb::require_true(d["k"].__subtype() == cjson::s_noesc);
    sb::require_true(emits(d, R"({"k":"back to clean"})"));
    sb::end_test_case();
  }
  {
    sb::test_case("an object key can be renamed in place, keeping its value and its position");
    auto r = P(R"({"old":1,"other":2})");
    cjson::doc &d = r.cast<cjson::doc>();

    sb::require(int(d.edit().rename("old", "renamed")), int(cjson::error::ok));
    sb::require(d["renamed"].i64_or(0), i64(1));
    sb::require_true(!d["old"]);
    // renaming is a string set on the key slot, so the member keeps its ORIGINAL position
    sb::require_true(emits(d, R"({"renamed":1,"other":2})"));

    // a longer name grows into the pool like any other string
    sb::require(int(d.edit().rename("renamed", "a_considerably_longer_key")), int(cjson::error::ok));
    sb::require(d["a_considerably_longer_key"].i64_or(0), i64(1));
    sb::require_true(emits(d, R"({"a_considerably_longer_key":1,"other":2})"));

    sb::require(int(d.edit().rename("nope", "x")), int(cjson::error::no_such_field));
    d.clear_mut_error();
    sb::end_test_case();
  }
  {
    sb::test_case("members and elements can be added and removed, and the tape stays consistent");
    auto r = P(R"({"a":1,"b":{"c":2},"arr":[1,2,3]})");
    cjson::doc &d = r.cast<cjson::doc>();

    d.edit().insert("z") = 9;
    sb::require_true(emits(d, R"({"a":1,"b":{"c":2},"arr":[1,2,3],"z":9})"));
    d["b"].insert("dd") = "x";
    sb::require_true(emits(d, R"({"a":1,"b":{"c":2,"dd":"x"},"arr":[1,2,3],"z":9})"));
    d["arr"].push_back() = 4;
    sb::require_true(emits(d, R"({"a":1,"b":{"c":2,"dd":"x"},"arr":[1,2,3,4],"z":9})"));
    d.edit().erase("a");
    sb::require_true(emits(d, R"({"b":{"c":2,"dd":"x"},"arr":[1,2,3,4],"z":9})"));
    d["arr"].erase(usize(0));
    sb::require_true(emits(d, R"({"b":{"c":2,"dd":"x"},"arr":[2,3,4],"z":9})"));
    d.edit().erase("b");      // erasing a whole subtree
    sb::require_true(emits(d, R"({"arr":[2,3,4],"z":9})"));
    d["arr"].clear();
    sb::require_true(emits(d, R"({"arr":[],"z":9})"));
    d.edit().insert("z") = 42;      // upsert, not a second member
    sb::require(d.root().size(), usize(2));
    sb::require_true(emits(d, R"({"arr":[],"z":42})"));
    sb::end_test_case();
  }
  {
    sb::test_case("fresh containers can be grown from nothing");
    auto r = P(R"({})");
    cjson::doc &d = r.cast<cjson::doc>();
    auto o = d.edit().insert_object("cfg");
    o.insert("deep") = true;
    auto a = d.edit().insert_array("list");
    a.push_back() = 1;
    a.push_object().insert("q") = 2;
    a.push_array().push_back() = 3;
    sb::require_true(emits(d, R"({"cfg":{"deep":true},"list":[1,{"q":2},[3]]})"));
    sb::require(d["list"].size(), usize(3));
    sb::end_test_case();
  }
  {
    sb::test_case("an edit deep in the tree repatches every ancestor and nothing else");
    auto r = P(R"({"l1":{"l2":{"l3":{"l4":[0]}}},"tail":1})");
    cjson::doc &d = r.cast<cjson::doc>();
    d["l1"]["l2"]["l3"]["l4"].push_back() = 7;
    sb::require_true(emits(d, R"({"l1":{"l2":{"l3":{"l4":[0,7]}}},"tail":1})"));
    d["l1"]["l2"]["l3"].insert("x") = 5;
    sb::require_true(emits(d, R"({"l1":{"l2":{"l3":{"l4":[0,7],"x":5}}},"tail":1})"));
    d["l1"]["l2"].erase("l3");
    sb::require_true(emits(d, R"({"l1":{"l2":{}},"tail":1})"));
    sb::require(d["tail"].i64_or(0), i64(1));      // the sibling after the splice survived
    sb::end_test_case();
  }
  {
    sb::test_case("a growing set promotes an insitu doc off the caller's buffer");
    char buf[128] = {};
    const char src[] = R"({"k":"ab","n":1})";
    for ( usize i = 0; i + 1 < sizeof(src); ++i ) buf[i] = src[i];
    auto r = cjson::parse_insitu(cjson::wbytes{ reinterpret_cast<u8 *>(buf), sizeof(src) - 1 });
    sb::require_true(r.is_first());
    cjson::doc &d = r.cast<cjson::doc>();
    // parse_insitu rewrites the input BY CONTRACT, so the property is that MUTATION does
    // not touch it afterwards
    char snap[128] = {};
    for ( usize i = 0; i < sizeof(buf); ++i ) snap[i] = buf[i];
    d["k"] = "a replacement far longer than the original";
    bool untouched = true;
    for ( usize i = 0; i < sizeof(buf); ++i )
      if ( buf[i] != snap[i] ) untouched = false;
    sb::require_true(untouched);
    sb::require_true(emits(d, R"({"k":"a replacement far longer than the original","n":1})"));
    sb::end_test_case();
  }
  {
    sb::test_case("a growing set detaches a borrowed doc so a later parse cannot disturb it");
    cjson::scratch sc{};
    auto r = cjson::parse_reuse(R"({"k":"ab","n":1})", 16, cjson::opts{}, sc);
    sb::require_true(r.is_first());
    cjson::doc &d = r.cast<cjson::doc>();
    sb::require_true(d.borrowed());
    d["n"] = 2;      // tier 1 allocates nothing, so it does not detach
    sb::require_true(d.borrowed());
    d["k"] = "a replacement far longer than the original";
    sb::require_false(d.borrowed());

    auto r2 = cjson::parse_reuse(R"({"totally":"different and quite a lot longer"})", 46, cjson::opts{}, sc);
    sb::require_true(r2.is_first());
    // the detached doc still reads correctly after the scratch was reused under it
    sb::require_true(emits(d, R"({"k":"a replacement far longer than the original","n":2})"));
    d.edit().insert("z") = 1;
    sb::require_true(emits(d, R"({"k":"a replacement far longer than the original","n":2,"z":1})"));
    sb::end_test_case();
  }
  {
    sb::test_case("a structural edit on a borrowed doc detaches it too");
    cjson::scratch sc{};
    auto r = cjson::parse_reuse(R"({"n":1})", 7, cjson::opts{}, sc);
    cjson::doc &d = r.cast<cjson::doc>();
    sb::require_true(d.borrowed());
    d.edit().insert("q") = 3;
    sb::require_false(d.borrowed());
    sb::require_true(emits(d, R"({"n":1,"q":3})"));
    sb::end_test_case();
  }
  {
    sb::test_case("every structural edit detaches a borrowed doc, not just the ones that append bytes");
    for ( int which = 0; which < 4; ++which ) {
      cjson::scratch sc{};
      auto r = PR(sc, R"({"a":[1,2,3],"n":1})");
      sb::require_true(r.is_first());
      cjson::doc &d = r.cast<cjson::doc>();
      sb::require_true(d.borrowed());
      switch ( which ) {
      case 0:
        d["a"].push_back() = i64(4);
        break;
      case 1:
        sb::require(int(d.edit().erase("n")), int(cjson::error::ok));
        break;
      case 2:
        sb::require(int(d["a"].erase(usize(0))), int(cjson::error::ok));
        break;
      default:
        sb::require(int(d["a"].clear()), int(cjson::error::ok));
        break;
      }
      sb::require_false(d.borrowed());
      auto r2 = PR(sc, R"({"totally":"different and quite a lot longer"})");
      sb::require_true(r2.is_first());
      sb::require_true(bound_holds(d));
      const micron::string out = cjson::write_str(d);
      sb::require_true(cjson::parse(out.c_str(), out.size()).is_first());
    }
    sb::end_test_case();
  }
  {
    sb::test_case("a failed erase leaves a borrowed doc borrowed");
    cjson::scratch sc{};
    auto r = PR(sc, R"({"n":1,"a":[7]})");
    cjson::doc &d = r.cast<cjson::doc>();
    sb::require_true(d.borrowed());
    sb::require(int(d.edit().erase("nope")), int(cjson::error::no_such_field));
    d.clear_mut_error();
    sb::require(int(d["a"].erase(usize(9))), int(cjson::error::no_such_field));
    d.clear_mut_error();
    sb::require_true(d.borrowed());
    sb::require_true(emits(d, R"({"n":1,"a":[7]})"));
    sb::end_test_case();
  }
  {
    sb::test_case("a set whose source aliases the pool survives the pool moving under it");
    const char *lit = "0123456789012345678901234567890123456789012345678901234567890123456789";
    auto r = P(R"({"a":"0123456789012345678901234567890123456789012345678901234567890123456789","b":1})");
    cjson::doc &d = r.cast<cjson::doc>();
    d["b"] = d["a"].str_or();
    sb::require_true(same(d["b"].str_or(), lit));
    sb::require_true(same(d["a"].str_or(), lit));
    // and the same aliasing through an inserted key, which puts s in the pool too
    d.edit().insert(d["a"].str_or()) = 5;
    sb::require_true(same(d["a"].str_or(), lit));
    sb::require(d[lit].i64_or(0), i64(5));
    sb::require_true(bound_holds(d));
    const micron::string out = cjson::write_str(d);
    sb::require_true(cjson::parse(out.c_str(), out.size()).is_first());
    sb::end_test_case();
  }
  {
    sb::test_case("set_number rejects an empty buffer instead of reading past it");
    auto r = P(R"({"n":0})");
    cjson::doc &d = r.cast<cjson::doc>();
    sb::require(int(d["n"].set_number(cjson::bytes{ nullptr, 0 })), int(cjson::error::bad_number));
    d.clear_mut_error();
    sb::require(d["n"].i64_or(-1), i64(0));      // and the slot is untouched
    sb::end_test_case();
  }
  {
    sb::test_case("a failed set is a no-op and latches a sticky code");
    auto r = P(R"({"o":{"a":1},"n":1})");
    cjson::doc &d = r.cast<cjson::doc>();

    sb::require(int(d["o"].set(i64(1))), int(cjson::error::wrong_type));
    sb::require(int(d.mut_error()), int(cjson::error::wrong_type));
    d.clear_mut_error();
    sb::require(int(d["nope"].set(i64(1))), int(cjson::error::no_such_field));
    d.clear_mut_error();
    sb::require(int(d.edit().erase("nope")), int(cjson::error::no_such_field));
    d.clear_mut_error();
    sb::require(int(d["n"].erase("x")), int(cjson::error::wrong_type));      // not a container
    d.clear_mut_error();
    sb::require(int(d["o"].erase(usize(0))), int(cjson::error::wrong_type));      // object, not array
    d.clear_mut_error();
    // every rejection left the document exactly as it was
    sb::require_true(emits(d, R"({"o":{"a":1},"n":1})"));
    sb::end_test_case();
  }
  {
    sb::test_case("set_number writes a literal through the parser's own number kernel");
    auto r = P(R"({"n":0})");
    cjson::doc &d = r.cast<cjson::doc>();
    const char lit[] = "123456789012345678";
    sb::require(int(d["n"].set_number(cjson::bytes{ reinterpret_cast<const u8 *>(lit), sizeof(lit) - 1 })), int(cjson::error::ok));
    sb::require(d["n"].u64_or(0), u64(123456789012345678ull));
    sb::require_true(emits(d, R"({"n":123456789012345678})"));
    const char bad[] = "not-a-number";
    sb::require(int(d["n"].set_number(cjson::bytes{ reinterpret_cast<const u8 *>(bad), sizeof(bad) - 1 })), int(cjson::error::bad_number));
    sb::end_test_case();
  }
  {
    sb::test_case("the cached writer bound survives hundreds of random mutations over a corpus doc");
    micron::vector<u8> raw = tutil::slurp("sample/64kb.json");
    sb::require_true(raw.size() != 0);
    auto r = cjson::parse(tutil::view(raw), cjson::opts{ .with_write_bound = true });
    sb::require_true(r.is_first());
    cjson::doc &d = r.cast<cjson::doc>();

    tutil::rng rg{};
    bool ok = true;
    for ( int i = 0; i < 400 and ok; ++i ) {
      // walk to some scalar and rewrite it with a value of a different rendered length
      const cjson::mut v = descend(d.edit(), rg, 3);
      if ( !v or cjson::is_ctn(*v.__raw()) ) continue;
      switch ( rg.below(4) ) {
      case 0:
        v = i64(rg.next());
        break;
      case 1:
        v = "a string of some length that differs from whatever was there";
        break;
      case 2:
        v = nullptr;
        break;
      default:
        v = f64(rg.next() % 100000) / 7.0;
        break;
      }
      ok = bound_holds(d);
    }
    sb::require_true(ok);
    // and the result is still valid json
    const micron::string out = cjson::write_str(d);
    sb::require_true(cjson::parse(out.c_str(), out.size()).is_first());
    sb::end_test_case();
  }
  {
    sb::test_case("runtime mutation agrees with the comptime twins");
    // the static_asserts above already ran these inside constant evaluation
    auto r = P(R"({"port":8080})");
    cjson::doc &d = r.cast<cjson::doc>();
    d["port"] = 42;
    sb::require(d["port"].i64_or(0), i64(ct_set(R"({"port":8080})", "port", 42)));
    sb::end_test_case();
  }
  return 1;
}
