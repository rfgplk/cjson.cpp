//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include "../src/cjson/cjson.hpp"

#include <snowball/snowball.hpp>

#include <micron/maps/robin.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

// the porcelain seam: every json kind marshals to exactly one pun alternative, containers
// become 16-byte handles rather than recursive copies, and a map round-trips back to json
// through the builder. the number arms are the regression this layer was written to fix —
// they used to yield u64 for signed and i64 for unsigned

namespace
{

// pins the pack: micron::string dominates the inline storage, so the container handles
// added here are free. if this trips, sizeof(pun) moved and the docs are stale
static_assert(sizeof(cjson::vref) == 16);
static_assert(sizeof(cjson::pun) == 56);

static_assert(cjson::pun_map<cjson::object_map>);
static_assert(cjson::pun_map<micron::robin_map<micron::string, cjson::pun>>);
static_assert(cjson::pun_map<micron::hswiss<cjson::key_view, cjson::pun>>);
static_assert(cjson::pun_seq<cjson::array_vec>);
static_assert(!cjson::pun_map<micron::vector<cjson::pun>>);

// key_view carries exactly the shape micron::hash needs and NOTHING else — adding
// operator[] would satisfy is_iterable_container and drag it into the parse overload set
static_assert(micron::is_container_or_string<cjson::key_view>);
static_assert(!micron::is_string<cjson::key_view>);
static_assert(!cjson::byte_source<cjson::key_view>);
static_assert(!cjson::text_source<cjson::key_view>);

const char k_src[] = R"({"name":"svc","workers":4,"neg":-7,"big":18446744073709551615,"ratio":0.25,
  "on":true,"nil":null,"limits":{"rps":2500,"burst":400},"peers":["a","b"],"dup":1,"dup":2})";

cjson::result<cjson::doc>
P()
{
  return cjson::parse(k_src, sizeof(k_src) - 1);
}

micron::string
S(const char *s)
{
  micron::string m{};
  usize n = 0;
  while ( s[n] ) ++n;
  m.append(s, n);
  return m;
}

bool
str_is(const micron::string &got, const char *want)
{
  usize n = 0;
  while ( want[n] ) ++n;
  if ( got.size() != n ) return false;
  for ( usize i = 0; i < n; ++i )
    if ( got.c_str()[i] != want[i] ) return false;
  return true;
}

};      // namespace

int
main()
{
  {
    sb::test_case("every scalar kind marshals to the alternative its json type implies");
    auto r = P();
    sb::require_true(r.is_first());
    auto root = r.cast<cjson::doc>().root();

    cjson::pun s = root["name"], w = root["workers"], n = root["neg"], b = root["big"];
    cjson::pun f = root["ratio"], o = root["on"], z = root["nil"];

    sb::require_true(s.is<micron::string>());
    sb::require_true(w.is<u64>() and w.cast<u64>() == 4);
    // the regression: a negative used to come back as u64 clamped to zero
    sb::require_true(n.is<i64>() and n.cast<i64>() == -7);
    // and a value above i64_max used to come back as a negative i64
    sb::require_true(b.is<u64>() and b.cast<u64>() == 18446744073709551615ull);
    sb::require_true(f.is<f64>() and f.cast<f64>() == 0.25);
    sb::require_true(o.is<bool>() and o.cast<bool>());
    sb::end_test_case();
  }
  {
    sb::test_case("json null is a value and a miss is not, and they are told apart");
    auto r = P();
    auto root = r.cast<cjson::doc>().root();
    cjson::pun z = root["nil"], miss = root["nope"];
    sb::require_true(z.is<cjson::jnull>());
    sb::require_true(z.has_value());
    sb::require_false(miss.has_value());
    sb::end_test_case();
  }
  {
    sb::test_case("a dom container marshals to a navigable handle, not to a copy");
    auto r = P();
    auto root = r.cast<cjson::doc>().root();
    cjson::pun o = root["limits"], a = root["peers"];
    sb::require_true(o.is<cjson::vref>());
    sb::require_true(a.is<cjson::vref>());
    // the whole point: one call and you have the full navigation api back
    auto nested = cjson::as_val(o.cast<cjson::vref>());
    sb::require(nested["rps"].i64_or(0), i64(2500));
    sb::require(nested.size(), usize(2));
    sb::require(cjson::as_val(a.cast<cjson::vref>()).at(1).str_or().len, usize(1));
    // and the round trip through vref is exact
    sb::require_true(cjson::as_vref(nested).v == nested.__raw());
    sb::end_test_case();
  }
  {
    sb::test_case("an on-demand container marshals to a source span that outlives the scratch");
    cjson::scratch sc{};
    auto rv = cjson::iterate(k_src, sizeof(k_src) - 1, sc);
    sb::require_true(rv.is_first());
    cjson::pun o = rv.cast<cjson::view>().root()["limits"];
    sb::require_true(o.is<cjson::jraw>());
    const cjson::strv span = o.cast<cjson::jraw>().text;
    sb::require_true(span.len != 0 and span.ptr[0] == '{' and span.ptr[span.len - 1] == '}');
    // the span points into the CALLER's text, so it re-parses independently
    auto rr = cjson::parse(span.ptr, span.len);
    sb::require_true(rr.is_first());
    sb::require(rr.cast<cjson::doc>().root()["burst"].i64_or(0), i64(400));
    sb::end_test_case();
  }
  {
    sb::test_case("a oneshot get on a container returns the subtree instead of nothing");
    // this used to be an explicit stub returning a valueless any
    auto g = cjson::get(k_src, "/limits");
    sb::require_true(g.is_first());
    sb::require_true(g.cast<cjson::pun>().is<cjson::jraw>());
    const cjson::strv span = g.cast<cjson::pun>().cast<cjson::jraw>().text;
    sb::require_true(cjson::parse(span.ptr, span.len).is_first());
    sb::end_test_case();
  }
  {
    sb::test_case("a flat object becomes a map whose nested members stay handles");
    auto r = P();
    auto root = r.cast<cjson::doc>().root();
    auto mr = cjson::to_map(root);
    sb::require_true(mr.is_first());
    auto &m = mr.cast<cjson::object_map>();

    sb::require_true(m.find(S("workers")) != nullptr);
    sb::require(m.find(S("workers"))->cast<u64>(), u64(4));
    sb::require_true(m.find(S("nil"))->is<cjson::jnull>());
    sb::require_true(m.find(S("limits"))->is<cjson::vref>());
    sb::require_true(m.find(S("peers"))->is<cjson::vref>());
    sb::require_true(m.find(S("nope")) == nullptr);
    sb::require(cjson::as_val(m.find(S("limits"))->cast<cjson::vref>())["rps"].i64_or(0), i64(2500));
    sb::end_test_case();
  }
  {
    sb::test_case("duplicate keys resolve first-wins, matching how lookup already behaves");
    auto r = P();
    auto root = r.cast<cjson::doc>().root();
    auto mr = cjson::to_map(root);
    auto &m = mr.cast<cjson::object_map>();
    // the tape keeps both pairs; the map keeps the first, as val::operator[] does
    sb::require(root.size(), usize(11));
    sb::require(m.size(), usize(10));
    sb::require(m.find(S("dup"))->cast<u64>(), u64(1));
    sb::require(root["dup"].i64_or(0), i64(1));

    // and a fixed-capacity map, whose own insert is LAST-wins, must agree — the explicit
    // find-then-insert in to_map_into is what makes that true
    micron::robin_map<micron::string, cjson::pun> rb(cjson::map_slots(root));
    sb::require(int(cjson::to_map_into(root, rb)), int(cjson::error::ok));
    sb::require(rb.size(), usize(10));
    sb::require(rb.find(S("dup"))->cast<u64>(), u64(1));
    sb::end_test_case();
  }
  {
    sb::test_case("map_slots sizes a fixed-capacity map past its load-factor guard");
    // robin_map throws once length >= slots * 7/8, so the exact pair count is not enough
    auto r = P();
    auto root = r.cast<cjson::doc>().root();
    sb::require_true(cjson::map_slots(root) >= 16);
    sb::require_true(cjson::map_slots(root) * 7 / 8 > root.size());
    sb::end_test_case();
  }
  {
    sb::test_case("for_each visits exactly the entries the map reports");
    // for_each, not range-for: heap_swiss's iterator yields pair<const K &, V &>, which
    // needs micron's forwarding pair ctor. range-for over it works on a current micron
    // and is what porcelain.hpp itself avoids depending on — the layer uses for_each
    auto r = P();
    auto mr = cjson::to_map(r.cast<cjson::doc>().root());
    auto &m = mr.cast<cjson::object_map>();
    usize seen = 0;
    bool found_nested = false;
    m.for_each([&](const auto &, const auto &v) {
      ++seen;
      if ( v.template is<cjson::vref>() ) found_nested = true;
    });
    sb::require(seen, m.size());
    sb::require_true(found_nested);
    sb::end_test_case();
  }
  {
    sb::test_case("borrowed keys hash and find without copying the key bytes");
    auto r = P();
    auto root = r.cast<cjson::doc>().root();
    micron::hswiss<cjson::key_view, cjson::pun> kv{};
    sb::require(int(cjson::to_map_into(root, kv)), int(cjson::error::ok));
    sb::require_true(kv.find(cjson::key_view{ cjson::as_strv("workers") }) != nullptr);
    sb::require(kv.find(cjson::key_view{ cjson::as_strv("workers") })->cast<u64>(), u64(4));
    sb::end_test_case();
  }
  {
    sb::test_case("an array becomes a vector in order, with mixed kinds preserved");
    auto r = cjson::parse(R"([1,-2,"three",null,true,{"a":1}])", 32);
    sb::require_true(r.is_first());
    auto vr = cjson::to_vector(r.cast<cjson::doc>().root());
    sb::require_true(vr.is_first());
    auto &v = vr.cast<cjson::array_vec>();
    sb::require(v.size(), usize(6));
    sb::require_true(v[0].is<u64>());
    sb::require_true(v[1].is<i64>() and v[1].cast<i64>() == -2);
    sb::require_true(v[2].is<micron::string>());
    sb::require_true(v[3].is<cjson::jnull>());
    sb::require_true(v[4].is<bool>());
    sb::require_true(v[5].is<cjson::vref>());
    sb::end_test_case();
  }
  {
    sb::test_case("a kind mismatch is an error rather than an empty container");
    auto r = P();
    auto root = r.cast<cjson::doc>().root();
    // an empty map would be indistinguishable from a successful {}
    sb::require_true(cjson::to_map(root["name"]).is_second());
    sb::require(int(cjson::to_map(root["name"]).cast<cjson::error>()), int(cjson::error::wrong_type));
    sb::require(int(cjson::to_map(root["nope"]).cast<cjson::error>()), int(cjson::error::no_such_field));
    sb::require(int(cjson::to_vector(root["limits"]).cast<cjson::error>()), int(cjson::error::wrong_type));
    // an empty object is a success, and an empty map
    auto e = cjson::parse(R"({})", 2);
    sb::require_true(cjson::to_map(e.cast<cjson::doc>().root()).is_first());
    sb::require(cjson::to_map(e.cast<cjson::doc>().root()).cast<cjson::object_map>().size(), usize(0));
    sb::end_test_case();
  }
  {
    sb::test_case("a map serialises back to json with nested subtrees intact");
    auto r = P();
    auto mr = cjson::to_map(r.cast<cjson::doc>().root());
    auto &m = mr.cast<cjson::object_map>();
    auto j = cjson::to_json(m);
    sb::require_true(j.is_first());
    const micron::string &js = j.cast<micron::string>();

    auto back = cjson::parse(js.c_str(), js.size());
    sb::require_true(back.is_first());
    auto b = back.cast<cjson::doc>().root();
    sb::require(b.size(), usize(10));
    sb::require(b["workers"].i64_or(0), i64(4));
    sb::require(b["neg"].i64_or(0), i64(-7));
    sb::require(b["big"].u64_or(0), u64(18446744073709551615ull));
    sb::require_true(b["nil"].is_null());
    sb::require_true(b["on"].bool_or(false));
    // the nested handles were serialised as real subtrees, not dropped
    sb::require(b["limits"]["rps"].i64_or(0), i64(2500));
    sb::require(b["limits"]["burst"].i64_or(0), i64(400));
    sb::require(b["peers"].size(), usize(2));
    sb::require_true(b["peers"].at(0).str_or().len == 1);
    sb::end_test_case();
  }
  {
    sb::test_case("a subtree serialises on its own, minified and pretty");
    auto r = P();
    auto root = r.cast<cjson::doc>().root();
    const micron::string sub = cjson::write_str(root["limits"]);
    sb::require_true(str_is(sub, R"({"rps":2500,"burst":400})"));
    const micron::string pre = cjson::write_str(root["limits"], cjson::style{ .indent = 2 });
    // the pretty form must still reparse to the same values
    auto rp = cjson::parse(pre.c_str(), pre.size());
    sb::require_true(rp.is_first());
    sb::require(rp.cast<cjson::doc>().root()["burst"].i64_or(0), i64(400));
    sb::require_true(cjson::write_bound(root["limits"]) >= sub.size());
    sb::end_test_case();
  }
  {
    sb::test_case("a vector serialises back to a json array");
    auto r = cjson::parse(R"([1,"two",null,{"a":1}])", 22);
    auto vr = cjson::to_vector(r.cast<cjson::doc>().root());
    auto j = cjson::to_json_seq(vr.cast<cjson::array_vec>());
    sb::require_true(j.is_first());
    const micron::string &js = j.cast<micron::string>();
    sb::require_true(str_is(js, R"([1,"two",null,{"a":1}])"));
    sb::end_test_case();
  }
  return 1;
}
