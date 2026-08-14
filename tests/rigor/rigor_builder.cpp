//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// builder.hpp's API surface. tests/fuzz/fuzz_builder_scenario.cpp attacks it with random
// sequences; this file walks the documented behaviour deliberately -- every value
// overload, the balance rule, the sticky error, buffer recycling, and take() vs out().

#include "../../src/cjson/cjson.hpp"

#include <snowball/snowball.hpp>

#include <micron/string/strings.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

void
parses(const micron::string &s, const char *what)
{
  if ( cjson::validate(reinterpret_cast<const u8 *>(s.c_str()), s.size()) != cjson::error::ok ) {
    snowball::print("builder output does not parse [", what, "]: ", s.c_str());
    sb::require_true(false);
  }
}

};      // namespace

int
main()
{
  {
    sb::test_case("every value overload emits conforming text");
    cjson::builder b;
    b.obj()
        .key("null")
        .null()
        .key("bt")
        .value(true)
        .key("bf")
        .value(false)
        .key("i32")
        .value(static_cast<i32>(-2147483647 - 1))
        .key("u32")
        .value(static_cast<u32>(4294967295u))
        .key("i64")
        .value(static_cast<i64>(-9223372036854775807LL - 1))
        .key("u64")
        .value(static_cast<u64>(18446744073709551615ULL))
        .key("f64")
        .value(-1.5e-10)
        .key("cstr")
        .value("plain")
        .key("esc")
        .value("a\"b\\c\nd\te")
        .end();
    micron::string out = b.take();
    parses(out, "all value overloads");

    auto r = cjson::parse(reinterpret_cast<const u8 *>(out.c_str()), out.size());
    sb::require_true(r.is_first());
    auto root = r.cast<cjson::doc>().root();
    sb::require_true(root["null"].is_null());
    sb::require_true(root["bt"].bool_or(false));
    sb::require_false(root["bf"].bool_or(true));
    sb::require(root["i64"].i64_or(0), static_cast<i64>(-9223372036854775807LL - 1));
    sb::require(root["u64"].u64_or(0), static_cast<u64>(18446744073709551615ULL));
    sb::require_true(root["f64"].f64_or(0.0) == -1.5e-10);
    sb::require(root["cstr"].str_or().len, static_cast<usize>(5));
    sb::require(root["esc"].str_or().len, static_cast<usize>(9));
    sb::end_test_case();
  }
  {
    sb::test_case("nesting to every shape");
    cjson::builder b;
    b.arr().obj().key("a").arr().value(static_cast<i64>(1)).obj().key("b").null().end().end().end().arr().end().obj().end().end();
    micron::string out = b.take();
    parses(out, "nesting");
    auto r = cjson::parse(reinterpret_cast<const u8 *>(out.c_str()), out.size());
    sb::require_true(r.is_first());
    auto root = r.cast<cjson::doc>().root();
    sb::require(root.size(), static_cast<usize>(3));
    sb::require(root.at(0)["a"].size(), static_cast<usize>(2));
    sb::require_true(root.at(0)["a"].at(1)["b"].is_null());
    sb::require_true(root.at(1).type() == cjson::kind::array);
    sb::require_true(root.at(2).type() == cjson::kind::object);
    sb::end_test_case();
  }
  {
    sb::test_case("out() is empty until balanced, take() resets");
    cjson::builder b;
    sb::require(b.out().len, static_cast<usize>(0));
    b.obj();
    sb::require(b.out().len, static_cast<usize>(0));
    b.key("a").value(static_cast<i64>(1));
    sb::require(b.out().len, static_cast<usize>(0));
    b.end();
    sb::require_greater(b.out().len, static_cast<usize>(0));

    micron::string first = b.take();
    parses(first, "take");
    // after take the builder is reset and ready for a fresh document
    sb::require(b.out().len, static_cast<usize>(0));
    b.arr().value(static_cast<i64>(9)).end();
    micron::string second = b.take();
    parses(second, "take then reuse");
    sb::require_true(second.size() == 3);
    sb::end_test_case();
  }
  {
    sb::test_case("err is sticky and suppresses output");
    {
      cjson::builder b;
      b.end();
      const cjson::error e = b.err();
      sb::require_true(e != cjson::error::ok);
      b.obj().key("a").value(static_cast<i64>(1)).end();
      sb::require_true(b.err() == e);
      sb::require(b.out().len, static_cast<usize>(0));
    }
    {
      cjson::builder b;
      b.arr().key("a");      // a name inside an array
      sb::require_true(b.err() != cjson::error::ok);
    }
    {
      cjson::builder b;
      b.obj().value(static_cast<i64>(1));      // a value where a name belongs
      sb::require_true(b.err() != cjson::error::ok);
    }
    {
      cjson::builder b;
      b.obj().key("a").end();      // a name with no value
      sb::require_true(b.err() != cjson::error::ok);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("a recycled buffer produces the same bytes as a fresh one");
    micron::string reuse;
    reuse.reserve(4096);
    cjson::builder a;
    a.obj().key("k").value(static_cast<i64>(7)).end();
    micron::string want = a.take();

    cjson::builder b{ micron::move(reuse) };
    b.obj().key("k").value(static_cast<i64>(7)).end();
    micron::string got = b.take();
    sb::require(got.size(), want.size());
    for ( usize i = 0; i < want.size(); ++i ) sb::require_true(got[i] == want[i]);
    sb::end_test_case();
  }
  {
    sb::test_case("kv writes a name and value together");
    cjson::builder b;
    b.obj().kv("a", static_cast<i64>(1)).kv("b", "two").kv("c", true).end();
    micron::string out = b.take();
    parses(out, "kv");
    auto r = cjson::parse(reinterpret_cast<const u8 *>(out.c_str()), out.size());
    sb::require_true(r.is_first());
    sb::require(r.cast<cjson::doc>().root()["a"].i64_or(-1), static_cast<i64>(1));
    sb::require(r.cast<cjson::doc>().root()["b"].str_or().len, static_cast<usize>(3));
    sb::require_true(r.cast<cjson::doc>().root()["c"].bool_or(false));
    sb::end_test_case();
  }
  {
    sb::test_case("large documents build correctly");
    cjson::builder b;
    b.arr();
    for ( u32 i = 0; i < 20000; ++i ) b.value(static_cast<i64>(i));
    b.end();
    micron::string out = b.take();
    parses(out, "20000 elements");
    auto r = cjson::parse(reinterpret_cast<const u8 *>(out.c_str()), out.size());
    sb::require_true(r.is_first());
    sb::require(r.cast<cjson::doc>().root().size(), static_cast<usize>(20000));
    sb::require(r.cast<cjson::doc>().root().at(19999).i64_or(-1), static_cast<i64>(19999));
    sb::end_test_case();
  }
  {
    // raw() is caller-trusted by contract; conforming input in, conforming output out
    sb::test_case("raw splices preserialized json verbatim");
    cjson::builder b;
    b.obj().key("a").raw(cjson::as_strv(R"([1,2,{"n":null}])")).key("b").value(static_cast<i64>(1)).end();
    micron::string out = b.take();
    parses(out, "raw");
    auto r = cjson::parse(reinterpret_cast<const u8 *>(out.c_str()), out.size());
    sb::require_true(r.is_first());
    sb::require(r.cast<cjson::doc>().root()["a"].size(), static_cast<usize>(3));
    sb::require_true(r.cast<cjson::doc>().root()["a"].at(2)["n"].is_null());
    sb::end_test_case();
  }
  return 1;
}
