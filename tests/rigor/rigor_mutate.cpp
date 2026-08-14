//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// every setter in mutate.hpp, with the invariant applied after each one: the document
// re-serialises, re-parses, and reads back what was written -- and the UNTOUCHED
// neighbours are still exactly where they were.
//
// That last clause is the point. A setter that writes the right value but disturbs a
// sibling offset or fails to grow the pool passes a naive "did it store the value" check
// and corrupts everything after it.

#include "../../src/cjson/cjson.hpp"

#include "../tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/string/strings.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

constexpr const char *k_base = R"({"before":11,"t":"seed","after":22,"arr":[1,2,3],"obj":{"n":1}})";

cjson::doc
fresh()
{
  usize n = 0;
  while ( k_base[n] ) ++n;
  auto r = cjson::parse(reinterpret_cast<const u8 *>(k_base), n);
  sb::require_true(r.is_first());
  return micron::move(r.cast<cjson::doc>());
}

// re-serialise, re-parse, and check the neighbours survived
cjson::doc
settle(cjson::doc &d, const char *what)
{
  if ( d.mut_error() != cjson::error::ok ) {
    snowball::print("mutation reported an error: ", what);
    sb::require_true(false);
  }
  micron::string out = cjson::write_str(d);
  auto r = cjson::parse(reinterpret_cast<const u8 *>(out.c_str()), out.size());
  if ( !r.is_first() ) {
    snowball::print("mutated document does not re-parse: ", what);
    snowball::print("   ", out.c_str());
    sb::require_true(false);
  }
  return micron::move(r.cast<cjson::doc>());
}

void
neighbours_intact(const cjson::doc &d, const char *what)
{
  auto root = d.root();
  if ( root["before"].i64_or(-1) != 11 ) snowball::print("neighbour `before` disturbed by ", what);
  sb::require(root["before"].i64_or(-1), static_cast<i64>(11));
  sb::require(root["after"].i64_or(-1), static_cast<i64>(22));
  sb::require(root["arr"].size(), static_cast<usize>(3));
  sb::require(root["obj"]["n"].i64_or(-1), static_cast<i64>(1));
}

};      // namespace

int
main()
{
  {
    sb::test_case("scalar setters store the value and leave the neighbours alone");
    {
      auto d = fresh();
      d.edit()["t"] = static_cast<i64>(-4242);
      auto o = settle(d, "set i64");
      sb::require(o.root()["t"].i64_or(0), static_cast<i64>(-4242));
      neighbours_intact(o, "set i64");
    }
    {
      auto d = fresh();
      d.edit()["t"] = static_cast<u64>(18446744073709551615ULL);
      auto o = settle(d, "set u64");
      sb::require(o.root()["t"].u64_or(0), static_cast<u64>(18446744073709551615ULL));
      neighbours_intact(o, "set u64");
    }
    {
      auto d = fresh();
      d.edit()["t"] = -1.5e-10;
      auto o = settle(d, "set f64");
      sb::require_true(o.root()["t"].f64_or(0.0) == -1.5e-10);
      neighbours_intact(o, "set f64");
    }
    {
      auto d = fresh();
      d.edit()["t"] = true;
      auto o = settle(d, "set bool");
      sb::require_true(o.root()["t"].bool_or(false));
      neighbours_intact(o, "set bool");
    }
    {
      auto d = fresh();
      sb::require_true(d.edit()["t"].set_null() == cjson::error::ok);
      auto o = settle(d, "set_null");
      sb::require_true(o.root()["t"].is_null());
      neighbours_intact(o, "set_null");
    }
    sb::end_test_case();
  }
  {
    // a string set that GROWS has to move the pool; one that shrinks must not leave a
    // stale length behind. Both directions, at many sizes.
    sb::test_case("string sets that grow and shrink keep the document intact");
    for ( u32 len : { 0u, 1u, 3u, 4u, 15u, 16u, 17u, 63u, 64u, 65u, 200u, 1000u, 4096u } ) {
      micron::string s;
      for ( u32 i = 0; i < len; ++i ) s.push_back(char('a' + (i % 26)));
      auto d = fresh();
      d.edit()["t"] = cjson::strv{ s.c_str(), s.size() };
      auto o = settle(d, "set string");
      auto got = o.root()["t"].str_or();
      sb::require(got.len, static_cast<usize>(len));
      for ( u32 i = 0; i < len; ++i ) sb::require_true(got.ptr[i] == s[i]);
      neighbours_intact(o, "set string");
    }
    sb::end_test_case();
  }
  {
    sb::test_case("set_number goes through the parser's own kernel");
    auto d = fresh();
    sb::require_true(d.edit()["t"].set_number(cjson::bytes{ reinterpret_cast<const u8 *>("-1.25e3"), 7 }) == cjson::error::ok);
    auto o = settle(d, "set_number");
    sb::require_true(o.root()["t"].f64_or(0.0) == -1250.0);
    neighbours_intact(o, "set_number");

    // a malformed token must be refused rather than stored
    auto d2 = fresh();
    sb::require_true(d2.edit()["t"].set_number(cjson::bytes{ reinterpret_cast<const u8 *>("01"), 2 }) != cjson::error::ok);
    sb::end_test_case();
  }
  {
    sb::test_case("insert creates or finds, and insert_object/array seed containers");
    {
      auto d = fresh();
      d.edit().insert(cjson::as_strv("brand")) = static_cast<i64>(5);
      auto o = settle(d, "insert new");
      sb::require(o.root()["brand"].i64_or(-1), static_cast<i64>(5));
      sb::require(o.root().size(), static_cast<usize>(6));
      neighbours_intact(o, "insert new");
    }
    {
      // inserting an existing name FINDS it rather than duplicating
      auto d = fresh();
      d.edit().insert(cjson::as_strv("t")) = static_cast<i64>(9);
      auto o = settle(d, "insert existing");
      sb::require(o.root().size(), static_cast<usize>(5));
      sb::require(o.root()["t"].i64_or(-1), static_cast<i64>(9));
      neighbours_intact(o, "insert existing");
    }
    {
      auto d = fresh();
      auto m = d.edit().insert_object(cjson::as_strv("no"));
      m.insert(cjson::as_strv("deep")) = static_cast<i64>(3);
      auto o = settle(d, "insert_object");
      sb::require(o.root()["no"]["deep"].i64_or(-1), static_cast<i64>(3));
      neighbours_intact(o, "insert_object");
    }
    {
      auto d = fresh();
      auto a = d.edit().insert_array(cjson::as_strv("na"));
      a.push_back() = static_cast<i64>(7);
      a.push_back() = static_cast<i64>(8);
      auto o = settle(d, "insert_array");
      sb::require(o.root()["na"].size(), static_cast<usize>(2));
      sb::require(o.root()["na"].at(1).i64_or(-1), static_cast<i64>(8));
      neighbours_intact(o, "insert_array");
    }
    sb::end_test_case();
  }
  {
    sb::test_case("rename keeps the member in place");
    auto d = fresh();
    sb::require_true(d.edit().rename(cjson::as_strv("t"), cjson::as_strv("renamed")) == cjson::error::ok);
    auto o = settle(d, "rename");
    sb::require(o.root().size(), static_cast<usize>(5));
    sb::require_true(o.root()["t"].type() == cjson::kind::none);
    sb::require(o.root()["renamed"].str_or().len, static_cast<usize>(4));
    neighbours_intact(o, "rename");

    // renaming to a longer and to a shorter name both work
    for ( const char *to : { "x", "a_much_longer_name_than_before" } ) {
      auto d2 = fresh();
      sb::require_true(d2.edit().rename(cjson::as_strv("t"), cjson::as_strv(to)) == cjson::error::ok);
      auto o2 = settle(d2, "rename resize");
      sb::require(o2.root()[cjson::as_strv(to)].str_or().len, static_cast<usize>(4));
      neighbours_intact(o2, "rename resize");
    }
    sb::end_test_case();
  }
  {
    sb::test_case("erase removes exactly one member and reports a miss");
    auto d = fresh();
    sb::require_true(d.edit().erase(cjson::as_strv("t")) == cjson::error::ok);
    auto o = settle(d, "erase");
    sb::require(o.root().size(), static_cast<usize>(4));
    sb::require_true(o.root()["t"].type() == cjson::kind::none);
    neighbours_intact(o, "erase");

    auto d2 = fresh();
    sb::require_true(d2.edit().erase(cjson::as_strv("nope")) == cjson::error::no_such_field);
    sb::end_test_case();
  }
  {
    sb::test_case("array push and erase maintain the element order");
    auto d = fresh();
    auto arr = d.edit()["arr"];
    arr.push_back() = static_cast<i64>(4);
    arr.push_back() = static_cast<i64>(5);
    auto o = settle(d, "push_back");
    sb::require(o.root()["arr"].size(), static_cast<usize>(5));
    sb::require(o.root()["arr"].size(), static_cast<usize>(5));
    for ( usize i = 0; i < 5; ++i ) sb::require(o.root()["arr"].at(i).i64_or(-1), static_cast<i64>(i + 1));

    auto d2 = fresh();
    const cjson::error ee = d2.edit()["arr"].erase(static_cast<usize>(1));
    if ( ee != cjson::error::ok ) snowball::print("erase(index) returned ", cjson::error_name(ee));
    sb::require_true(ee == cjson::error::ok);
    auto o2 = settle(d2, "erase index");
    sb::require(o2.root()["arr"].size(), static_cast<usize>(2));
    sb::require(o2.root()["arr"].at(0).i64_or(-1), static_cast<i64>(1));
    sb::require(o2.root()["arr"].at(1).i64_or(-1), static_cast<i64>(3));
    // NOT neighbours_intact(): that helper expects arr to still hold 3, and this block
    // deliberately erased one. Check the members that really were untouched.
    sb::require(o2.root()["before"].i64_or(-1), static_cast<i64>(11));
    sb::require(o2.root()["after"].i64_or(-1), static_cast<i64>(22));
    sb::require(o2.root()["obj"]["n"].i64_or(-1), static_cast<i64>(1));

    auto d3 = fresh();
    auto a3 = d3.edit()["arr"];
    a3.push_object().insert(cjson::as_strv("k")) = static_cast<i64>(1);
    a3.push_array().push_back() = static_cast<i64>(2);
    auto o3 = settle(d3, "push_object/push_array");
    sb::require(o3.root()["arr"].size(), static_cast<usize>(5));
    sb::require(o3.root()["arr"].at(3)["k"].i64_or(-1), static_cast<i64>(1));
    sb::require(o3.root()["arr"].at(4).at(0).i64_or(-1), static_cast<i64>(2));
    sb::end_test_case();
  }
  {
    sb::test_case("clear empties either container kind");
    {
      auto d = fresh();
      sb::require_true(d.edit()["arr"].clear() == cjson::error::ok);
      auto o = settle(d, "clear array");
      sb::require(o.root()["arr"].size(), static_cast<usize>(0));
      sb::require_true(o.root()["arr"].type() == cjson::kind::array);
      // neighbours_intact() expects arr to still hold 3, so check them directly here
      sb::require(o.root()["before"].i64_or(-1), static_cast<i64>(11));
      sb::require(o.root()["after"].i64_or(-1), static_cast<i64>(22));
      sb::require(o.root()["obj"]["n"].i64_or(-1), static_cast<i64>(1));
    }
    {
      auto d = fresh();
      sb::require_true(d.edit()["obj"].clear() == cjson::error::ok);
      auto o = settle(d, "clear object");
      sb::require(o.root()["obj"].size(), static_cast<usize>(0));
      sb::require_true(o.root()["obj"].type() == cjson::kind::object);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("mut_error is sticky until cleared");
    auto d = fresh();
    sb::require_true(d.mut_error() == cjson::error::ok);
    const cjson::error e = d.edit().erase(cjson::as_strv("nope"));
    sb::require_true(e == cjson::error::no_such_field);
    // a failed lookup through operator[] sets the sticky error
    d.edit()["nope"] = static_cast<i64>(1);
    sb::require_true(d.mut_error() != cjson::error::ok);
    d.clear_mut_error();
    sb::require_true(d.mut_error() == cjson::error::ok);
    sb::end_test_case();
  }
  {
    // long sequences on ONE document, which is where offsets accumulate damage
    sb::test_case("a long mutation sequence on one document stays consistent");
    auto d = fresh();
    for ( u32 i = 0; i < 400; ++i ) {
      micron::string k;
      k.push_back('m');
      k.push_back(char('0' + (i / 100) % 10));
      k.push_back(char('0' + (i / 10) % 10));
      k.push_back(char('0' + i % 10));
      d.edit().insert(cjson::strv{ k.c_str(), k.size() }) = static_cast<i64>(i);
      sb::require_true(d.mut_error() == cjson::error::ok);
    }
    micron::string out = cjson::write_str(d);
    auto r = cjson::parse(reinterpret_cast<const u8 *>(out.c_str()), out.size());
    sb::require_true(r.is_first());
    auto root = r.cast<cjson::doc>().root();
    sb::require(root.size(), static_cast<usize>(405));
    for ( u32 i = 0; i < 400; ++i ) {
      micron::string k;
      k.push_back('m');
      k.push_back(char('0' + (i / 100) % 10));
      k.push_back(char('0' + (i / 10) % 10));
      k.push_back(char('0' + i % 10));
      sb::require(root[cjson::strv{ k.c_str(), k.size() }].i64_or(-1), static_cast<i64>(i));
    }
    sb::require(root["before"].i64_or(-1), static_cast<i64>(11));
    sb::require(root["after"].i64_or(-1), static_cast<i64>(22));
    sb::end_test_case();
  }
  return 1;
}
