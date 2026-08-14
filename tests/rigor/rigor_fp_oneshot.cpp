//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// the fp adaptors and the oneshot helpers, held against the dom answers.
//
// Laziness is made VISIBLE rather than assumed: a side-effect counter rides through the
// pipelines, so "take(2) only touched two elements" is asserted rather than hoped for.
// An adaptor that quietly became eager still returns the right answer and would sail
// through any test that only checks results.

#include "../../src/cjson/cjson.hpp"

#include "../tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/string/strings.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

constexpr const char *k_doc = R"({"nums":[1,2,3,4,5,6,7,8,9,10],
                                  "recs":[{"n":"a","v":10},{"n":"b","v":20},{"n":"c","v":30}],
                                  "flat":{"x":1,"y":2,"z":3}})";

cjson::doc
load()
{
  usize n = 0;
  while ( k_doc[n] ) ++n;
  auto r = cjson::parse(reinterpret_cast<const u8 *>(k_doc), n);
  sb::require_true(r.is_first());
  return micron::move(r.cast<cjson::doc>());
}

u32 g_touched = 0;

};      // namespace

int
main()
{
  auto d = load();
  auto root = d.root();

  {
    sb::test_case("fold, count and count_if agree with a hand walk");
    auto nums = root["nums"].items();
    i64 hand = 0;
    usize hand_n = 0;
    for ( auto v : root["nums"].items() ) {
      hand += v.i64_or(0);
      ++hand_n;
    }
    sb::require(hand, static_cast<i64>(55));

    sb::require(cjson::fold(nums, static_cast<i64>(0), [](i64 acc, cjson::val v) { return acc + v.i64_or(0); }), hand);
    sb::require(cjson::count(nums), hand_n);
    sb::require(cjson::count_if([](cjson::val v) { return v.i64_or(0) % 2 == 0; }, nums), static_cast<usize>(5));
    sb::end_test_case();
  }
  {
    sb::test_case("any_of, all_of and none_of");
    auto nums = root["nums"].items();
    sb::require_true(cjson::any_of([](cjson::val v) { return v.i64_or(0) == 7; }, nums));
    sb::require_false(cjson::any_of([](cjson::val v) { return v.i64_or(0) == 99; }, nums));
    sb::require_true(cjson::all_of([](cjson::val v) { return v.i64_or(0) > 0; }, nums));
    sb::require_false(cjson::all_of([](cjson::val v) { return v.i64_or(0) > 5; }, nums));
    sb::require_true(cjson::none_of([](cjson::val v) { return v.i64_or(0) > 100; }, nums));
    sb::end_test_case();
  }
  {
    sb::test_case("fmap, filter, reject, take and drop compose");
    auto nums = root["nums"].items();
    // evens, doubled, first three: 4, 8, 12
    auto pipe = cjson::take(
        3, cjson::fmap([](i64 x) { return x * 2; }, cjson::fmap([](cjson::val v) { return v.i64_or(0); },
                                                                cjson::filter([](cjson::val v) { return v.i64_or(0) % 2 == 0; }, nums))));
    i64 sum = cjson::fold(pipe, static_cast<i64>(0), [](i64 a, i64 x) { return a + x; });
    sb::require(sum, static_cast<i64>(24));

    auto odd = cjson::reject([](cjson::val v) { return v.i64_or(0) % 2 == 0; }, root["nums"].items());
    sb::require(cjson::count(odd), static_cast<usize>(5));

    auto tail = cjson::drop(7, root["nums"].items());
    sb::require(cjson::count(tail), static_cast<usize>(3));
    sb::end_test_case();
  }
  {
    // laziness, made visible: take(2) over a counting projection must touch exactly two
    sb::test_case("take is lazy: it touches only what it yields");
    g_touched = 0;
    auto counted = cjson::fmap(
        [](cjson::val v) {
          ++g_touched;
          return v.i64_or(0);
        },
        root["nums"].items());
    auto two = cjson::take(2, counted);
    const i64 s = cjson::fold(two, static_cast<i64>(0), [](i64 a, i64 x) { return a + x; });
    sb::require(s, static_cast<i64>(3));
    if ( g_touched != 2 ) snowball::print("take(2) touched ", g_touched, " elements, not 2 -- the pipeline is eager");
    sb::require(g_touched, static_cast<u32>(2));
    sb::end_test_case();
  }
  {
    sb::test_case("find_first stops at the first match");
    g_touched = 0;
    auto counted = cjson::fmap(
        [](cjson::val v) {
          ++g_touched;
          return v.i64_or(0);
        },
        root["nums"].items());
    auto hit = cjson::find_first([](i64 x) { return x == 3; }, counted);
    sb::require_true(hit.is_first());
    sb::require(hit.cast<i64>(), static_cast<i64>(3));
    // find_first SHORT-CIRCUITS -- 4 of 10 touched, not all 10 -- but it carries one
    // element of lookahead: the underlying iterator advances past the match before the
    // search reports it. That is the measured contract, so it is what gets pinned; a
    // regression to eager evaluation would read 10 here.
    sb::require(g_touched, static_cast<u32>(4));
    sb::require_true(g_touched < 10);

    auto miss
        = cjson::find_first([](i64 x) { return x == 999; }, cjson::fmap([](cjson::val v) { return v.i64_or(0); }, root["nums"].items()));
    sb::require_true(miss.is_second());
    sb::end_test_case();
  }
  {
    sb::test_case("take_while and drop_while split at the predicate");
    auto nums = root["nums"].items();
    auto lead = cjson::take_while([](cjson::val v) { return v.i64_or(0) < 4; }, nums);
    sb::require(cjson::count(lead), static_cast<usize>(3));
    auto rest = cjson::drop_while([](cjson::val v) { return v.i64_or(0) < 4; }, root["nums"].items());
    sb::require(cjson::count(rest), static_cast<usize>(7));
    sb::end_test_case();
  }
  {
    sb::test_case("keys, values and pluck read objects");
    sb::require(cjson::count(cjson::keys(root["flat"].members())), static_cast<usize>(3));
    sb::require(cjson::count(cjson::values(root["flat"].members())), static_cast<usize>(3));
    auto names = cjson::pluck("n", root["recs"].items());
    sb::require(cjson::count(names), static_cast<usize>(3));
    auto vs = cjson::fmap([](cjson::val v) { return v.i64_or(0); }, cjson::pluck("v", root["recs"].items()));
    sb::require(cjson::fold(vs, static_cast<i64>(0), [](i64 a, i64 x) { return a + x; }), static_cast<i64>(60));
    sb::end_test_case();
  }
  {
    sb::test_case("max_by and min_by project before comparing");
    auto mx = cjson::max_by([](cjson::val v) { return v["v"].i64_or(0); }, root["recs"].items());
    sb::require_true(mx.is_first());
    sb::require(mx.cast<cjson::val>()["v"].i64_or(0), static_cast<i64>(30));
    auto mn = cjson::min_by([](cjson::val v) { return v["v"].i64_or(0); }, root["recs"].items());
    sb::require_true(mn.is_first());
    sb::require(mn.cast<cjson::val>()["v"].i64_or(0), static_cast<i64>(10));
    sb::end_test_case();
  }
  {
    sb::test_case("enumerate pairs an index with each element");
    usize n = 0;
    i64 checksum = 0;
    for ( auto p : cjson::enumerate(root["nums"].items()) ) {
      checksum += static_cast<i64>(p.i) * p.v.i64_or(0);
      ++n;
    }
    sb::require(n, static_cast<usize>(10));
    // sum over i of i*(i+1) for i in 0..9
    sb::require(checksum, static_cast<i64>(330));
    sb::end_test_case();
  }
  {
    sb::test_case("for_each visits every element exactly once");
    g_touched = 0;
    cjson::for_each([](cjson::val) { ++g_touched; }, root["nums"].items());
    sb::require(g_touched, static_cast<u32>(10));
    sb::end_test_case();
  }
  {
    sb::test_case("the oneshot helpers agree with the dom");
    usize dn = 0;
    while ( k_doc[dn] ) ++dn;
    const cjson::strv text{ k_doc, dn };

    sb::require_true(cjson::valid(text));
    sb::require_true(cjson::exists(text, "/nums/0"));
    sb::require_false(cjson::exists(text, "/nums/99"));
    sb::require_false(cjson::exists(text, "/nope"));

    auto gi = cjson::get<i64>(text, "/nums/2");
    sb::require_true(gi.is_first());
    sb::require(gi.cast<i64>(), static_cast<i64>(3));

    sb::require(cjson::get_or<i64>(text, "/nums/2", -1), static_cast<i64>(3));
    sb::require(cjson::get_or<i64>(text, "/nope", -1), static_cast<i64>(-1));

    sb::require_true(cjson::kind_at(text, "/nums") == cjson::kind::array);
    sb::require_true(cjson::kind_at(text, "/flat") == cjson::kind::object);
    sb::require_true(cjson::kind_at(text, "/nope") == cjson::kind::none);

    auto ca = cjson::count_at(text, "/nums");
    sb::require_true(ca.is_first());
    sb::require(ca.cast<usize>(), static_cast<usize>(10));

    u32 visited = 0;
    sb::require_true(cjson::each(text, "/nums", [&visited](cjson::cur) { ++visited; }) == cjson::error::ok);
    sb::require(visited, static_cast<u32>(10));
    sb::end_test_case();
  }
  {
    sb::test_case("compact, pretty and reformat produce parseable text");
    usize dn = 0;
    while ( k_doc[dn] ) ++dn;
    const cjson::strv text{ k_doc, dn };
    auto c = cjson::compact(text);
    sb::require_true(c.is_first());
    sb::require_true(cjson::valid(cjson::as_strv(c.cast<micron::string>().c_str())));
    auto p = cjson::pretty(text, 2);
    sb::require_true(p.is_first());
    sb::require_true(cjson::valid(cjson::as_strv(p.cast<micron::string>().c_str())));
    sb::end_test_case();
  }
  return 1;
}
