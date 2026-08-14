//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// the dom surface, exhaustively: every getter and try_*, navigation by name / index /
// json-pointer, iteration order, duplicate-name semantics, sibling-offset integrity, and
// the relocatability that falls out of payloads being offsets rather than pointers.
//
// The integrity walk is the load-bearing part. A value arena whose sibling offsets or
// parent back-offsets are wrong still answers simple queries correctly -- it fails only
// on the traversal that crosses the damage. So the walk here visits every node by BOTH
// routes (index and iterator) and requires them to agree, at every depth.

#include "../../src/cjson/cjson.hpp"

#include "../tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/string/strings.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

// lengths are DEDUCED from the literal, never hand-counted. A count that is too small
// does not fail loudly -- it silently parses a shorter, still-valid document, and the
// test then asserts things about a document nobody wrote. This bit twice while these
// files were being written.
template<usize N>
inline cjson::result<cjson::doc>
PJ(const char (&s)[N])
{
  return cjson::parse(reinterpret_cast<const u8 *>(s), N - 1);
}

cjson::result<cjson::doc>
P(const char *s)
{
  usize n = 0;
  while ( s[n] ) ++n;
  return cjson::parse(reinterpret_cast<const u8 *>(s), n);
}

// visit every node, reaching containers by index AND by iterator, and require the two
// routes to produce identical values
usize
walk(cjson::val v, u32 depth = 0)
{
  if ( depth > 200 ) return 0;
  usize seen = 1;
  switch ( v.type() ) {
  case cjson::kind::array: {
    const usize n = v.size();
    usize by_iter = 0;
    for ( auto e : v.items() ) {
      const cjson::val by_index = v.at(by_iter);
      sb::require_true(e.type() == by_index.type());
      if ( e.type() == cjson::kind::number ) {
        const f64 a = e.f64_or(0.0), b = by_index.f64_or(1.0);
        sb::require_true(__builtin_bit_cast(u64, a) == __builtin_bit_cast(u64, b));
      }
      seen += walk(e, depth + 1);
      ++by_iter;
    }
    sb::require(by_iter, n);
    // one past the end is a null val, never a crash and never a wrap
    sb::require_true(v.at(n).type() == cjson::kind::none);
    sb::require_true(v.at(n + 1000).type() == cjson::kind::none);
    break;
  }
  case cjson::kind::object: {
    const usize n = v.size();
    usize by_iter = 0;
    for ( auto m : v.members() ) {
      // every name the iterator yields must be findable by name
      const cjson::val by_name = v[m.key];
      sb::require_true(by_name.type() != cjson::kind::none);
      seen += walk(m.v, depth + 1);
      ++by_iter;
    }
    sb::require(by_iter, n);
    sb::require_true(v[cjson::as_strv("\x01no-such-key\x01")].type() == cjson::kind::none);
    break;
  }
  default:
    break;
  }
  return seen;
}

};      // namespace

int
main()
{
  {
    sb::test_case("kind is reported for every value shape");
    auto r = P(R"({"nul":null,"t":true,"f":false,"i":1,"d":1.5,"s":"x","a":[],"o":{}})");
    sb::require_true(r.is_first());
    auto root = r.cast<cjson::doc>().root();
    sb::require_true(root.type() == cjson::kind::object);
    sb::require_true(root["nul"].type() == cjson::kind::null);
    sb::require_true(root["t"].type() == cjson::kind::boolean);
    sb::require_true(root["f"].type() == cjson::kind::boolean);
    sb::require_true(root["i"].type() == cjson::kind::number);
    sb::require_true(root["d"].type() == cjson::kind::number);
    sb::require_true(root["s"].type() == cjson::kind::string);
    sb::require_true(root["a"].type() == cjson::kind::array);
    sb::require_true(root["o"].type() == cjson::kind::object);
    sb::require_true(root["missing"].type() == cjson::kind::none);

    sb::require_true(root["nul"].is_null());
    sb::require_false(root["t"].is_null());
    // a missing value is falsey; a present one is not
    sb::require_true(static_cast<bool>(root["t"]));
    sb::require_false(static_cast<bool>(root["missing"]));
    sb::end_test_case();
  }
  {
    sb::test_case("size reports elements, PAIRS and bytes by kind");
    auto r = P(R"({"a":[1,2,3],"o":{"x":1,"y":2},"s":"abcde","e":[],"eo":{}})");
    sb::require_true(r.is_first());
    auto root = r.cast<cjson::doc>().root();
    sb::require(root.size(), static_cast<usize>(5));      // 5 members, not 10
    sb::require(root["a"].size(), static_cast<usize>(3));
    sb::require(root["o"].size(), static_cast<usize>(2));      // pairs
    sb::require(root["s"].size(), static_cast<usize>(5));      // bytes
    sb::require(root["e"].size(), static_cast<usize>(0));
    sb::require(root["eo"].size(), static_cast<usize>(0));
    sb::end_test_case();
  }
  {
    sb::test_case("the lossy getters fall back rather than fail");
    auto r = P(R"({"i":-7,"u":42,"d":1.5,"b":true,"s":"hi","n":null})");
    sb::require_true(r.is_first());
    auto root = r.cast<cjson::doc>().root();
    sb::require(root["i"].i64_or(0), static_cast<i64>(-7));
    sb::require(root["u"].u64_or(0), static_cast<u64>(42));
    sb::require_true(root["d"].f64_or(0.0) == 1.5);
    sb::require_true(root["b"].bool_or(false));
    sb::require(root["s"].str_or().len, static_cast<usize>(2));

    // wrong kind or missing -> the supplied default, every time
    sb::require(root["s"].i64_or(99), static_cast<i64>(99));
    sb::require(root["missing"].i64_or(99), static_cast<i64>(99));
    sb::require(root["n"].i64_or(99), static_cast<i64>(99));
    sb::require_true(root["i"].bool_or(true));
    sb::require(root["b"].str_or(cjson::strv{ "d", 1 }).len, static_cast<usize>(1));
    sb::end_test_case();
  }
  {
    sb::test_case("the try_ getters report wrong_type and out_of_range");
    auto r = P(R"({"i":-7,"u":18446744073709551615,"d":1.5,"b":true,"s":"hi"})");
    sb::require_true(r.is_first());
    auto root = r.cast<cjson::doc>().root();

    auto ti = root["i"].try_i64();
    sb::require_true(ti.is_first());
    sb::require(ti.cast<i64>(), static_cast<i64>(-7));

    // u64 max cannot be an i64
    auto tu = root["u"].try_i64();
    sb::require_true(tu.is_second());
    sb::require_true(tu.cast<cjson::error>() == cjson::error::out_of_range);

    // a negative cannot be a u64
    auto tn = root["i"].try_u64();
    sb::require_true(tn.is_second());

    // a string is not a number
    auto ts = root["s"].try_i64();
    sb::require_true(ts.is_second());
    sb::require_true(ts.cast<cjson::error>() == cjson::error::wrong_type);

    auto tb = root["b"].try_bool();
    sb::require_true(tb.is_first() and tb.cast<bool>());

    auto tstr = root["s"].try_str();
    sb::require_true(tstr.is_first());
    sb::require(tstr.cast<cjson::strv>().len, static_cast<usize>(2));

    // a miss is no_such_field
    auto tm = root["nope"].try_i64();
    sb::require_true(tm.is_second());
    sb::end_test_case();
  }
  {
    sb::test_case("navigation reaches every corner and misses cleanly");
    auto r = P(R"({"a":{"b":{"c":[0,[1,2,{"d":true}]]}},"e":[[],{}],"f":"tail"})");
    sb::require_true(r.is_first());
    auto root = r.cast<cjson::doc>().root();
    sb::require_true(root["a"]["b"]["c"].at(1).at(2)["d"].bool_or(false));
    sb::require(root["a"]["b"]["c"].at(0).i64_or(-1), static_cast<i64>(0));
    sb::require(root["e"].size(), static_cast<usize>(2));

    // a miss anywhere in a chain propagates as a null val rather than a crash
    sb::require_true(root["zz"]["yy"]["xx"].at(9).type() == cjson::kind::none);
    sb::require(root["zz"]["yy"].i64_or(5), static_cast<i64>(5));
    // indexing an object, or naming an array, is a miss and not a reinterpretation
    sb::require_true(root["a"].at(0).type() == cjson::kind::none);
    sb::require_true(root["e"]["nope"].type() == cjson::kind::none);
    sb::end_test_case();
  }
  {
    // rfc 6901. "" names the whole document; ~1 is / and ~0 is ~, in that order.
    sb::test_case("at_pointer implements rfc 6901");
    auto r = P(R"({"foo":["bar","baz"],"":0,"a/b":1,"c%d":2,"e^f":3,"g|h":4,"i\\j":5,"k\"l":6," ":7,"m~n":8})");
    sb::require_true(r.is_first());
    auto root = r.cast<cjson::doc>().root();

    sb::require_true(root.at_pointer(cjson::as_strv("")).type() == cjson::kind::object);
    sb::require(root.at_pointer(cjson::as_strv("/foo")).size(), static_cast<usize>(2));
    sb::require(root.at_pointer(cjson::as_strv("/foo/0")).str_or().len, static_cast<usize>(3));
    sb::require(root.at_pointer(cjson::as_strv("/")).i64_or(-1), static_cast<i64>(0));
    sb::require(root.at_pointer(cjson::as_strv("/a~1b")).i64_or(-1), static_cast<i64>(1));
    sb::require(root.at_pointer(cjson::as_strv("/c%d")).i64_or(-1), static_cast<i64>(2));
    sb::require(root.at_pointer(cjson::as_strv("/e^f")).i64_or(-1), static_cast<i64>(3));
    sb::require(root.at_pointer(cjson::as_strv("/g|h")).i64_or(-1), static_cast<i64>(4));
    sb::require(root.at_pointer(cjson::as_strv("/ ")).i64_or(-1), static_cast<i64>(7));
    sb::require(root.at_pointer(cjson::as_strv("/m~0n")).i64_or(-1), static_cast<i64>(8));

    // misses
    sb::require_true(root.at_pointer(cjson::as_strv("/nope")).type() == cjson::kind::none);
    sb::require_true(root.at_pointer(cjson::as_strv("/foo/9")).type() == cjson::kind::none);
    sb::require_true(root.at_pointer(cjson::as_strv("/foo/x")).type() == cjson::kind::none);
    sb::end_test_case();
  }
  {
    // duplicate names: all kept, first wins. rfc s4 leaves this open; COMPLIANCE.md
    // records the choice, and this is where it is enforced.
    sb::test_case("duplicate names are all kept and the first one wins");
    auto r = P(R"({"k":1,"k":2,"k":3,"other":9})");
    sb::require_true(r.is_first());
    auto root = r.cast<cjson::doc>().root();
    sb::require(root.size(), static_cast<usize>(4));
    sb::require(root["k"].i64_or(-1), static_cast<i64>(1));
    sb::require(root["other"].i64_or(-1), static_cast<i64>(9));

    // iteration yields every one of them, in document order
    i64 seen[3] = { 0, 0, 0 };
    u32 idx = 0;
    for ( auto m : root.members() ) {
      if ( m.key.len == 1 and m.key.ptr[0] == 'k' and idx < 3 ) seen[idx++] = m.v.i64_or(-1);
    }
    sb::require(idx, static_cast<u32>(3));
    sb::require(seen[0], static_cast<i64>(1));
    sb::require(seen[1], static_cast<i64>(2));
    sb::require(seen[2], static_cast<i64>(3));
    sb::end_test_case();
  }
  {
    sb::test_case("the arena survives being moved: payloads are offsets, not pointers");
    auto r = P(R"({"a":{"b":[1,2,{"c":"deep"}]},"d":[[1],[2],[3]]})");
    sb::require_true(r.is_first());
    cjson::doc moved = micron::move(r.cast<cjson::doc>());
    sb::require_true(moved.alive());
    sb::require_true(moved.root()["a"]["b"].at(2)["c"].str_or().len == 4);
    sb::require(moved.root()["d"].size(), static_cast<usize>(3));

    // move again, into a vector, so the doc's own address changes too
    micron::vector<cjson::doc> v;
    v.push_back(micron::move(moved));
    sb::require_true(v[0].root()["a"]["b"].at(2)["c"].str_or().len == 4);
    sb::require(v[0].root()["d"].at(1).at(0).i64_or(-1), static_cast<i64>(2));
    sb::end_test_case();
  }
  {
    sb::test_case("sibling offsets stay consistent over the whole corpus");
    const char *files[] = { "sample/64kb.json", "sample/128KB.json", "sample/twitter.json", "sample/256KB.json", "sample/512KB.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      auto r = cjson::parse(tutil::view(data));
      sb::require_true(r.is_first());
      const usize visited = walk(r.cast<cjson::doc>().root());
      sb::require_greater(visited, static_cast<usize>(0));
    }
    sb::end_test_case();
  }
  {
    sb::test_case("wide and deep documents navigate correctly");
    // wide: 20000 members
    {
      micron::vector<u8> d;
      d.push_back(u8('{'));
      for ( u32 i = 0; i < 20000; ++i ) {
        if ( i ) d.push_back(u8(','));
        d.push_back(u8('"'));
        d.push_back(u8('k'));
        for ( u32 k = 0; k < 5; ++k ) d.push_back(u8('0' + ((i >> (k * 3)) & 7)));
        d.push_back(u8('"'));
        d.push_back(u8(':'));
        // NOTE the leading '1'. Emitting the octal digits alone produces values like
        // "01234", and s6 forbids leading zeros -- the document would be invalid and the
        // test would be measuring its own generator rather than the dom.
        d.push_back(u8('1'));
        for ( u32 k = 0; k < 5; ++k ) d.push_back(u8('0' + ((i >> (k * 3)) & 7)));
      }
      d.push_back(u8('}'));
      auto r = cjson::parse(cjson::bytes{ d.cbegin(), d.size() });
      sb::require_true(r.is_first());
      sb::require(r.cast<cjson::doc>().root().size(), static_cast<usize>(20000));
      usize count = 0;
      for ( auto m : r.cast<cjson::doc>().root().members() ) {
        (void)m;
        ++count;
      }
      sb::require(count, static_cast<usize>(20000));
    }
    // deep: to the limit, then read the innermost value back
    {
      const u32 n = cjson::depth_limit - 1;
      micron::vector<u8> d;
      for ( u32 i = 0; i < n; ++i ) d.push_back(u8('['));
      d.push_back(u8('7'));
      for ( u32 i = 0; i < n; ++i ) d.push_back(u8(']'));
      auto r = cjson::parse(cjson::bytes{ d.cbegin(), d.size() });
      sb::require_true(r.is_first());
      cjson::val v = r.cast<cjson::doc>().root();
      for ( u32 i = 0; i < n; ++i ) {
        sb::require_true(v.type() == cjson::kind::array);
        v = v.at(0);
      }
      sb::require(v.i64_or(-1), static_cast<i64>(7));
    }
    sb::end_test_case();
  }
  {
    sb::test_case("consumed reports where the root ended");
    auto r = PJ("  {\"a\":1}  ");
    sb::require_true(r.is_first());
    sb::require(r.cast<cjson::doc>().consumed(), static_cast<usize>(11));

    // under stop_when_done it is the cut point, not the input length
    const char *nd = "{\"a\":1}{\"b\":2}";
    auto r2 = cjson::parse(reinterpret_cast<const u8 *>(nd), 14, cjson::opts{ .stop_when_done = true });
    sb::require_true(r2.is_first());
    sb::require(r2.cast<cjson::doc>().consumed(), static_cast<usize>(7));
    sb::end_test_case();
  }
  return 1;
}
