// 04_functional.cpp
// cjson's functional layer — lazy pipelines over json values.
//
// See also:
//   examples/02_dom.cpp                  — the ranges these pipelines consume
//   examples/03_ondemand.cpp             — the forward-only cursor they also work on
//   micron/examples/algorithm_fp.cpp     — micron's fp:: layer, whose idiom this follows
// Two spellings, as in micron:
//   eager, function-first   fmap(fn, range)      fold(range, init, fn)
//   curried, range-last     range | fmap_c(fn) | fold_c(init, fn)
//
// micron's ocaml-style operator| (|>) is what makes the second one read left to right.

#include "_ex_common.hpp"

#include <micron/vector.hpp>
#include <micron/std.hpp>

namespace cj = cjson;

int
main()
{
  constexpr const char k_src[] = R"({"users":[
    {"name":"ada", "age":36, "active":true,  "tags":["math","logic"]},
    {"name":"bob", "age":24, "active":false, "tags":[]},
    {"name":"cy",  "age":41, "active":true,  "tags":["ml"]},
    {"name":"dee", "age":19, "active":true,  "tags":["ui","ux","a11y"]}
  ]})";
  constexpr usize k_len = sizeof(k_src) - 1;

  auto r = cjson::parse(k_src, k_len);
  if ( r.is_second() ) return 1;
  const cjson::doc &d = r.cast<cjson::doc>();
  const cjson::val users = d.root()["users"];

  // fold
  ex::head("fold");

  // the loop
  i64 by_hand = 0;
  for ( auto u : users.items() )
    if ( u["active"].bool_or(false) ) by_hand += u["age"].i64_or(0);

  // the pipeline (piped, curried)
  const i64 piped
      = users.items() | cj::filter_c(cj::field_bool("active")) | cj::pluck_c("age") | cj::fmap_c(cj::to_i64) | cj::fold_c(i64(0), cj::plus);

  // the pipeline (nested calls, function-first)
  const i64 nested
      = cj::fold(cj::fmap(cj::to_i64, cj::pluck("age", cj::filter(cj::field_bool("active"), users.items()))), i64(0), cj::plus);

  mc::echo("sum of active ages: loop=", by_hand, " piped=", piped, " nested=", nested);
  mc::echo("(zero cost abstraction; see benches/fp_bench.cpp)");

  // selection
  ex::head("filter / reject / take / drop");

  auto adult = [](cjson::val v) { return v["age"].i64_or(0) >= 25; };
  mc::echo("adults          = ", cj::count(cj::filter(adult, users.items())));
  mc::echo("non-adults      = ", cj::count(cj::reject(adult, users.items())));
  mc::echo("first two       = ", cj::count(cj::take(2, users.items())));
  mc::echo("all but first   = ", cj::count(cj::drop(1, users.items())));
  // take_while stops at the first failure; filter keeps scanning
  mc::echo("take_while adult= ", cj::count(cj::take_while(adult, users.items())), " (stops at bob)");

  // predicates and searches
  ex::head("predicates");

  mc::echo("any over 40   : ", cj::any_of([](cjson::val v) { return v["age"].i64_or(0) > 40; }, users.items()) ? "yes" : "no");
  mc::echo("all have name : ", cj::all_of(cj::has("name"), users.items()) ? "yes" : "no");
  mc::echo("none is string: ", cj::none_of(cj::is_kind(cjson::kind::string), users.items()) ? "yes" : "no");

  // find_first is the one place in the library that produces error::no_such_field
  auto found = cj::find_first([](cjson::val v) { return v["age"].i64_or(0) < 20; }, users.items());
  if ( found.is_first() ) ex::show("youngest under 20: ", found.cast<cjson::val>()["name"].str_or());

  auto missing = cj::find_first([](cjson::val v) { return v["age"].i64_or(0) > 200; }, users.items());
  mc::echo("nobody over 200 -> ", cjson::error_name(missing.cast<cjson::error>()));

  // max_by/min_by need an ordered projection
  auto oldest = cj::max_by(cj::field_i64("age"), users.items());
  if ( oldest.is_first() ) ex::show("oldest: ", oldest.cast<cjson::val>()["name"].str_or());

  // objects
  ex::head("objects");

  const cjson::val ada = users.at(0);
  mc::echo("ada's keys:");
  for ( auto k : cj::keys(ada.members()) ) ex::show("  ", k);
  mc::echo("string-valued fields: ", cj::count_if(cj::is_kind(cjson::kind::string), cj::values(ada.members())));

  // flat_map
  // bob has no tags, so his inner range simply vanishes rather than stalling the walk
  ex::head("flat_map");

  mc::echo("total tags across all users = ", cj::count(cj::flat_map([](cjson::val v) { return v["tags"].items(); }, users.items())));

  // collect
  ex::head("collect");

  auto names = users.items() | cj::pluck_c("name") | cj::fmap_c(cj::to_str) | cj::collect_into_c<micron::vector<cjson::strv>>();
  mc::echo("collected ", names.size(), " names:");
  for ( auto n : names ) ex::show("  ", n);

  // WARNING: micron's string types define a member operator| (simd bitwise-or) that _outranks_ the free pipe

  // laziness, made visible
  ex::head("laziness");

  usize touched = 0;
  auto chain = users.items() | cj::fmap_c([&](cjson::val v) {
                 ++touched;
                 return v;
               });
  mc::echo("after building the chain, fn ran ", touched, " times");

  // count() advances the walk but never dereferences it, so the map still does not run
  mc::echo("count() = ", cj::count(chain), ", fn ran ", touched, " times");

  // a terminal that reads values does run it, once each
  cj::for_each([](cjson::val) { }, chain);
  mc::echo("after for_each, fn ran ", touched, " times");

  // and take() stops the source early
  touched = 0;
  cj::for_each([](cjson::val) { }, cj::take(2, users.items() | cj::fmap_c([&](cjson::val v) {
                                                 ++touched;
                                                 return v;
                                               })));
  mc::echo("take(2) over 4 users: fn ran ", touched, " times");

  // same pipelines on the on-demand cursor
  ex::head("on-demand");

  cjson::scratch sc;
  auto rv = cjson::iterate(k_src, k_len, sc);
  if ( rv.is_first() ) {
    auto ousers = rv.cast<cjson::view>().root()["users"];
    const i64 sum = ousers.items() | cj::filter_c(cj::field_bool("active")) | cj::pluck_c("age") | cj::fmap_c(cj::to_i64)
                    | cj::fold_c(i64(0), cj::plus);
    mc::echo("same pipeline, no dom: ", sum);
  }

  mc::echo("");
  return 0;
}
