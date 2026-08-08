//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include "../src/cjson/cjson.hpp"

#include <snowball/snowball.hpp>

#include <micron/types.hpp>

// builder semantics: composed text parses back to what was composed, misuse is a
// sticky error not a crash, buffers recycle through take()

namespace
{

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

};      // namespace

int
main()
{
  {
    sb::test_case("a composed response parses back with every field intact");
    cjson::builder b;
    b.obj()
        .kv("id", u64(12345))
        .kv("name", "widget")
        .kv("price", 19.99)
        .kv("active", true)
        .key("tags")
        .arr()
        .value("a")
        .value("b")
        .end()
        .key("meta")
        .obj()
        .kv("rev", i64(-3))
        .end()
        .key("nothing")
        .null()
        .end();
    sb::require_true(b.err() == cjson::error::ok);
    auto s = b.out();
    sb::require_greater(s.len, static_cast<usize>(0));
    auto r = cjson::parse(s.ptr, s.len);
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    sb::require_true(d.root()["id"].u64_or(0) == 12345);
    sb::require_true(d.root()["price"].f64_or(0) == 19.99);
    sb::require_true(d.root()["tags"][usize(1)].str_or().len == 1);
    sb::require_true(d.root()["meta"]["rev"].i64_or(0) == -3);
    sb::require_true(d.root()["nothing"].is_null());
    sb::end_test_case();
  }
  {
    sb::test_case("exact text shapes match the comma-erase discipline");
    cjson::builder b;
    b.arr().value(i64(1)).value(i64(2)).end();
    sb::require_true(same(b.out(), "[1,2]"));
    cjson::builder b2;
    b2.obj().end();
    sb::require_true(same(b2.out(), "{}"));
    cjson::builder b3;
    b3.value("just a string");
    sb::require_true(same(b3.out(), "\"just a string\""));
    sb::end_test_case();
  }
  {
    sb::test_case("keys and values escape on the way out");
    cjson::builder b;
    b.obj().kv("ta\tb", "line\nquote\"back\\slash").end();
    auto s = b.out();
    auto r = cjson::parse(s.ptr, s.len);
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    const char key[] = { 't', 'a', 0x09, 'b', 0 };
    auto v = d.root()[key].str_or();
    sb::require(v.len, static_cast<usize>(21));
    sb::require_true(v.ptr[4] == 0x0a and v.ptr[10] == '"' and v.ptr[15] == '\\');
    sb::end_test_case();
  }
  {
    sb::test_case("raw injects a pre-serialized fragment verbatim");
    cjson::builder b;
    b.obj().key("cfg").raw(cjson::strv{ R"({"port":8080})", 13 }).end();
    auto s = b.out();
    auto r = cjson::parse(s.ptr, s.len);
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    sb::require_true(d.root()["cfg"]["port"].i64_or(0) == 8080);
    sb::end_test_case();
  }
  {
    sb::test_case("misuse is a sticky error and out() refuses to lie");
    cjson::builder b;
    b.arr().key("nope");      // key outside an object
    sb::require_true(b.err() != cjson::error::ok);
    sb::require_true(b.out().len == 0);
    cjson::builder b2;
    b2.obj().value(i64(1));      // value with no key
    sb::require_true(b2.err() != cjson::error::ok);
    cjson::builder b3;
    b3.end();      // underflow
    sb::require_true(b3.err() != cjson::error::ok);
    cjson::builder b4;
    b4.arr();      // unbalanced at out()
    sb::require_true(b4.out().len == 0);
    cjson::builder b5;
    b5.value(i64(1)).value(i64(2));      // two roots
    sb::require_true(b5.err() != cjson::error::ok);
    sb::end_test_case();
  }
  {
    sb::test_case("take recycles the buffer across responses");
    cjson::builder b;
    b.obj().kv("n", i64(1)).end();
    micron::string first = b.take();
    sb::require_greater(first.size(), static_cast<usize>(0));
    cjson::builder b2(micron::move(first));
    b2.obj().kv("n", i64(2)).end();
    auto s = b2.out();
    auto r = cjson::parse(s.ptr, s.len);
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    sb::require_true(d.root()["n"].i64_or(0) == 2);
    sb::end_test_case();
  }
  return 1;
}
