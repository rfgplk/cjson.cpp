//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include "../src/cjson/cjson.hpp"

#include <snowball/snowball.hpp>

#include <micron/string/strings.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace cj = cjson;

namespace
{

constexpr const char k_users[] = R"({"users":[
  {"name":"ada","age":36,"active":true,"tags":["math","logic"]},
  {"name":"bob","age":24,"active":false,"tags":[]},
  {"name":"cy","age":41,"active":true,"tags":["ml"]},
  {"name":"dee","age":19,"active":true,"tags":["ui","ux","a11y"]}
]})";
constexpr usize k_users_len = sizeof(k_users) - 1;

cjson::scratch g_sc;

bool
same(cjson::strv a, const char *b)
{
  usize n = 0;
  while ( b[n] ) ++n;
  if ( a.len != n ) return false;
  for ( usize i = 0; i < n; i++ )
    if ( a.ptr[i] != b[i] ) return false;
  return true;
}

constexpr i64
ct_sum_active_ages()
{
  constexpr const char src[] = R"([{"a":1,"on":true},{"a":2,"on":false},{"a":4,"on":true}])";
  u8 *buf = new u8[sizeof(src)];
  for ( usize i = 0; i < sizeof(src) - 1; ++i ) buf[i] = u8(src[i]);
  cjson::scratch sc{};
  cjson::doc d{};
  const max_t r = cjson::__parse_into(d, cjson::bytes{ buf, sizeof(src) - 1 }, cjson::opts{}, sc);
  i64 out = -1;

  if ( r > 0 )
    out = cj::fold(cj::fmap(cj::to_i64, cj::pluck("a", cj::filter([](cjson::val v) { return v["on"].bool_or(false); }, d.root().items()))),
                   i64(0), cj::plus);
  d.release();
  sc.release();
  delete[] buf;
  return out;
}

static_assert(ct_sum_active_ages() == 5);

constexpr usize
ct_count_take_drop()
{
  constexpr const char src[] = R"([1,2,3,4,5,6,7])";
  u8 *buf = new u8[sizeof(src)];
  for ( usize i = 0; i < sizeof(src) - 1; ++i ) buf[i] = u8(src[i]);
  cjson::scratch sc{};
  cjson::doc d{};
  const max_t r = cjson::__parse_into(d, cjson::bytes{ buf, sizeof(src) - 1 }, cjson::opts{}, sc);
  usize out = 0;
  if ( r > 0 ) out = cj::count(cj::take(3, cj::drop(2, d.root().items())));
  d.release();
  sc.release();
  delete[] buf;
  return out;
}

static_assert(ct_count_take_drop() == 3);

};      // namespace

int
main()
{
  auto rd = cjson::parse(k_users, k_users_len);
  if ( rd.is_second() ) return 0;
  const cjson::doc &d = rd.cast<cjson::doc>();
  const cjson::val users = d.root()["users"];

  {
    sb::test_case("a fold pipeline agrees with the loop it replaces");

    i64 want = 0;
    for ( auto u : users.items() )
      if ( u["active"].bool_or(false) ) want += u["age"].i64_or(0);

    const i64 got = users.items() | cj::filter_c([](cjson::val v) { return v["active"].bool_or(false); })
                    | cj::fmap_c([](cjson::val v) { return v["age"].i64_or(0); }) | cj::fold_c(i64(0), cj::plus);

    const i64 got2
        = cj::fold(cj::fmap(cj::to_i64, cj::pluck("age", cj::filter(cj::field_bool("active"), users.items()))), i64(0), cj::plus);

    sb::require_true(want == 36 + 41 + 19);
    sb::require_true(got == want);
    sb::require_true(got2 == want);
    sb::end_test_case();
  }
  {
    sb::test_case("adaptors are lazy — nothing runs until a terminal pulls a value");
    usize touched = 0;
    auto chain = users.items() | cj::fmap_c([&](cjson::val v) {
                   ++touched;
                   return v;
                 });
    sb::require(touched, usize(0));

    sb::require(cj::count(chain), usize(4));
    sb::require(touched, usize(0));

    cj::for_each([](cjson::val) { }, chain);
    sb::require(touched, usize(4));
    sb::end_test_case();
  }
  {
    sb::test_case("take short-circuits — the source is not walked past the cut");
    usize touched = 0;
    usize n = 0;
    cj::for_each([&](cjson::val) { ++n; }, cj::take(2, cj::fmap(
                                                           [&](cjson::val v) {
                                                             ++touched;
                                                             return v;
                                                           },
                                                           users.items())));
    sb::require(n, usize(2));
    sb::require(touched, usize(2));
    sb::end_test_case();
  }
  {
    sb::test_case("filter, reject, take_while and drop_while partition the same way");
    auto is_adult = [](cjson::val v) { return v["age"].i64_or(0) >= 25; };
    sb::require(cj::count_if(is_adult, users.items()), usize(2));
    sb::require(cj::count(cj::filter(is_adult, users.items())), usize(2));
    sb::require(cj::count(cj::reject(is_adult, users.items())), usize(2));

    sb::require(cj::count(cj::take_while(is_adult, users.items())), usize(1));

    sb::require(cj::count(cj::drop_while(is_adult, users.items())), usize(3));
    sb::end_test_case();
  }
  {
    sb::test_case("take and drop clamp instead of running off the end");
    sb::require(cj::count(cj::take(99, users.items())), usize(4));
    sb::require(cj::count(cj::drop(99, users.items())), usize(0));
    sb::require(cj::count(cj::take(0, users.items())), usize(0));
    sb::require(cj::count(cj::drop(0, users.items())), usize(4));
    sb::end_test_case();
  }
  {
    sb::test_case("enumerate pairs each element with its position");
    usize seen = 0;
    for ( auto e : cj::enumerate(users.items()) ) {
      sb::require_true(e.v.__raw() == users.at(e.i).__raw());
      ++seen;
    }
    sb::require(seen, usize(4));
    sb::end_test_case();
  }
  {
    sb::test_case("flat_map concatenates inner ranges and skips the empty ones");

    const usize tags = cj::count(cj::flat_map([](cjson::val v) { return v["tags"].items(); }, users.items()));
    sb::require(tags, usize(2 + 0 + 1 + 3));

    auto first
        = cj::find_first([](cjson::val) { return true; }, cj::flat_map([](cjson::val v) { return v["tags"].items(); }, users.items()));
    sb::require_true(first.is_first());
    sb::require_true(same(first.cast<cjson::val>().str_or(), "math"));
    sb::end_test_case();
  }
  {
    sb::test_case("predicates and searches over the whole range");
    sb::require_true(cj::any_of([](cjson::val v) { return v["age"].i64_or(0) > 40; }, users.items()));
    sb::require_true(cj::all_of(cj::has("name"), users.items()));
    sb::require_true(cj::none_of(cj::is_kind(cjson::kind::string), users.items()));

    auto oldest = cj::max_by(cj::field_i64("age"), users.items());
    sb::require_true(oldest.is_first());
    sb::require_true(same(oldest.cast<cjson::val>()["name"].str_or(), "cy"));

    auto youngest = cj::min_by([](cjson::val v) { return v["age"].i64_or(0); }, users.items());
    sb::require_true(youngest.is_first());
    sb::require_true(same(youngest.cast<cjson::val>()["name"].str_or(), "dee"));
    sb::end_test_case();
  }
  {
    sb::test_case("find_first misses produce error::no_such_field, the one site that does");
    auto miss = cj::find_first([](cjson::val v) { return v["age"].i64_or(0) > 1000; }, users.items());
    sb::require_true(miss.is_second());
    sb::require_true(miss.cast<cjson::error>() == cjson::error::no_such_field);

    auto empty = cj::max_by(cj::to_i64, users.at(1)["tags"].items());
    sb::require_true(empty.is_second());
    sb::require_true(empty.cast<cjson::error>() == cjson::error::no_such_field);
    sb::end_test_case();
  }
  {
    sb::test_case("object ranges project to keys and values");
    const cjson::val ada = users.at(0);
    sb::require(cj::count(ada.members()), usize(4));

    usize keybytes = 0;
    for ( auto k : cj::keys(ada.members()) ) keybytes += k.len;
    sb::require(keybytes, usize(4 + 3 + 6 + 4));

    sb::require(cj::count_if(cj::is_kind(cjson::kind::string), cj::values(ada.members())), usize(1));
    auto found = cj::find_first(cj::key_is("age"), ada.members());
    sb::require_true(found.is_first());
    sb::require_true(found.cast<cjson::member>().v.i64_or(0) == 36);
    sb::end_test_case();
  }
  {
    sb::test_case("collect_into materializes only where asked");
    auto names = users.items() | cj::pluck_c("name") | cj::fmap_c(cj::to_str) | cj::collect_into_c<micron::vector<cjson::strv>>();
    sb::require(names.size(), usize(4));
    sb::require_true(same(names[0], "ada"));
    sb::require_true(same(names[3], "dee"));

    auto ages = users.items() | cj::fmap_c([](cjson::val v) { return v["age"].i64_or(0); }) | cj::collect_into_c<micron::vector<i64>>();
    sb::require(ages.size(), usize(4));
    sb::require_true(ages[2] == 41);
    sb::end_test_case();
  }
  {
    sb::test_case("the same pipelines run on the on-demand cursor");
    cjson::scratch sc;
    auto rv = cjson::iterate(k_users, k_users_len, sc);
    sb::require_true(rv.is_first());
    auto root = rv.cast<cjson::view>().root();
    auto ousers = root["users"];

    const i64 got = ousers.items() | cj::filter_c([](cjson::cur v) { return v["active"].bool_or(false); }) | cj::fmap_c(cj::to_i64)
                    | cj::fold_c(i64(0), cj::plus);

    sb::require_true(got == 0);

    const i64 ages = ousers.items() | cj::filter_c([](cjson::cur v) { return v["active"].bool_or(false); }) | cj::pluck_c("age")
                     | cj::fmap_c(cj::to_i64) | cj::fold_c(i64(0), cj::plus);
    sb::require_true(ages == 36 + 41 + 19);

    sb::require(cj::count(ousers.items()), usize(4));
    sb::require(cj::count(cj::flat_map([](cjson::cur v) { return v["tags"].items(); }, ousers.items())), usize(6));
    sb::require_true(cj::all_of(cj::has("name"), ousers.items()));

    auto oldest = cj::max_by(cj::field_i64("age"), ousers.items());
    sb::require_true(oldest.is_first());
    sb::require_true(same(oldest.cast<cjson::cur>()["name"].str_raw(), "cy"));
    sb::end_test_case();
  }
  {
    sb::test_case("on-demand object ranges project like the dom ones");
    cjson::scratch sc;
    auto rv = cjson::iterate(k_users, k_users_len, sc);
    sb::require_true(rv.is_first());
    auto ada = rv.cast<cjson::view>().root()["users"].at(0);

    sb::require(cj::count(ada.members()), usize(4));
    auto found = cj::find_first(cj::key_is("active"), ada.members());
    sb::require_true(found.is_first());
    sb::require_true(found.cast<cjson::cur_member>().v.bool_or(false));

    usize keybytes = 0;
    for ( auto k : cj::keys(ada.members()) ) keybytes += k.len;
    sb::require(keybytes, usize(4 + 3 + 6 + 4));
    sb::end_test_case();
  }
  {
    sb::test_case("empty, absent and wrong-kind sources yield empty pipelines");
    const cjson::val nothing = d.root()["nope"];
    sb::require(cj::count(nothing.items()), usize(0));
    sb::require(cj::count(cj::fmap(cj::to_i64, nothing.items())), usize(0));
    sb::require(cj::count(cj::filter(cj::is_truthy, nothing.items())), usize(0));
    sb::require_true(cj::fold(cj::fmap(cj::to_i64, nothing.items()), i64(7), cj::plus) == 7);
    sb::require_true(cj::all_of(cj::is_truthy, nothing.items()));
    sb::require_true(!cj::any_of(cj::is_truthy, nothing.items()));

    const cjson::val empty_tags = users.at(1)["tags"];
    sb::require(cj::count(empty_tags.items()), usize(0));
    sb::end_test_case();
  }
  {
    sb::test_case("is_truthy distinguishes absent, null and false from present values");
    constexpr const char src[] = R"([null,false,true,0,"",[],{}])";
    auto r = cjson::parse(src, sizeof(src) - 1);
    sb::require_true(r.is_first());
    auto a = r.cast<cjson::doc>().root();
    sb::require_true(!cj::is_truthy(a.at(0)));
    sb::require_true(!cj::is_truthy(a.at(1)));
    sb::require_true(cj::is_truthy(a.at(2)));
    sb::require_true(cj::is_truthy(a.at(3)));
    sb::require_true(cj::is_truthy(a.at(4)));
    sb::require_true(!cj::is_truthy(a.at(99)));
    sb::end_test_case();
  }
  {
    sb::test_case("chains compose in either spelling and to any depth");

    auto v1 = users.items() | cj::drop_c(1) | cj::take_c(2) | cj::pluck_c("name") | cj::fmap_c(cj::to_str)
              | cj::collect_into_c<micron::vector<cjson::strv>>();
    sb::require(v1.size(), usize(2));
    sb::require_true(same(v1[0], "bob"));
    sb::require_true(same(v1[1], "cy"));

    auto v2
        = cj::collect_into<micron::vector<cjson::strv>>(cj::fmap(cj::to_str, cj::pluck("name", cj::take(2, cj::drop(1, users.items())))));
    sb::require(v2.size(), usize(2));
    sb::require_true(same(v2[0], "bob"));

    micron::string kname{};
    kname.append("name", 4);
    sb::require(cj::count(cj::pluck(kname, users.items())), usize(4));
    sb::require_true(cj::all_of(cj::has(kname), users.items()));
    sb::require_true(cj::find_first(cj::key_is(kname), users.at(0).members()).is_first());
    sb::end_test_case();
  }
  {
    sb::test_case("for_each visits every element exactly once");
    usize n = 0;
    i64 sum = 0;
    cj::for_each(
        [&](cjson::val v) {
          ++n;
          sum += v["age"].i64_or(0);
        },
        users.items());
    sb::require(n, usize(4));
    sb::require_true(sum == 36 + 24 + 41 + 19);
    sb::end_test_case();
  }
  (void)g_sc;
  return 1;
}
