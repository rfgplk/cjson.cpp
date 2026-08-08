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

// dom semantics over the value arena: navigation, iteration order, sibling-offset
// integrity, duplicate keys, ndjson consumption, and the whole sample corpus

namespace
{

cjson::result<cjson::doc>
P(const char *s)
{
  usize n = 0;
  while ( s[n] ) ++n;
  return cjson::parse(s, n);
}

};      // namespace

int
main()
{
  {
    sb::test_case("scalar roots of every kind parse");
    sb::require_true(P("null").is_first());
    sb::require_true(P("true").is_first());
    sb::require_true(P("false").is_first());
    sb::require_true(P("0").is_first());
    sb::require_true(P("\"s\"").is_first());
    sb::require_true(P("  42  ").is_first());
    sb::end_test_case();
  }
  {
    sb::test_case("navigation reaches every corner of a nested document");
    auto r = P(R"({"a":{"b":{"c":[0,[1,2,{"d":true}]]}},"e":[[],{}],"f":"tail"})");
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    auto root = d.root();
    sb::require_true(root["a"]["b"]["c"][usize(1)][usize(2)]["d"].bool_or(false));
    sb::require(root["a"]["b"]["c"][usize(0)].i64_or(-1), static_cast<i64>(0));
    sb::require(root["e"].size(), static_cast<usize>(2));
    sb::require(root["e"][usize(0)].size(), static_cast<usize>(0));
    sb::require(root["e"][usize(1)].size(), static_cast<usize>(0));
    sb::require_true(root["f"].str_or().len == 4);
    sb::require_true(!root["a"]["missing"]["chain"]["safe"]);
    sb::require(root.at_pointer(cjson::strv{ "/a/b/c/1/2/d", 12 }).bool_or(false), true);
    sb::require_true(!root.at_pointer(cjson::strv{ "/a/x", 4 }));
    sb::end_test_case();
  }
  {
    sb::test_case("iteration preserves document order for objects and arrays");
    auto r = P(R"({"one":1,"two":2,"three":3,"arr":[10,20,30]})");
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    const char *keys[] = { "one", "two", "three", "arr" };
    usize ki = 0;
    for ( auto m : d.root().members() ) {
      usize kl = 0;
      while ( keys[ki][kl] ) ++kl;
      sb::require(m.key.len, kl);
      bool eq = true;
      for ( usize i = 0; i < kl; i++ ) eq = eq and m.key.ptr[i] == keys[ki][i];
      sb::require_true(eq);
      ++ki;
    }
    sb::require(ki, static_cast<usize>(4));
    i64 want = 10;
    for ( auto e : d.root()["arr"].items() ) {
      sb::require(e.i64_or(-1), want);
      want += 10;
    }
    sb::end_test_case();
  }
  {
    sb::test_case("duplicate keys are preserved and lookup returns the first");
    auto r = P(R"({"k":1,"k":2,"k":3})");
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    sb::require(d.root().size(), static_cast<usize>(3));
    sb::require(d.root()["k"].i64_or(-1), static_cast<i64>(1));
    sb::end_test_case();
  }
  {
    sb::test_case("flat arrays take the o1 index path and mixed arrays the walk");
    auto r = P(R"([[1,2,3,4,5],[1,[2],3]])");
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    auto flat = d.root()[usize(0)];
    auto mixed = d.root()[usize(1)];
    sb::require(flat[usize(4)].i64_or(-1), static_cast<i64>(5));
    sb::require(mixed[usize(2)].i64_or(-1), static_cast<i64>(3));
    sb::require(mixed[usize(1)][usize(0)].i64_or(-1), static_cast<i64>(2));
    sb::end_test_case();
  }
  {
    sb::test_case("wrong-type and out-of-range extractions surface the right errors");
    auto r = P(R"({"s":"x","n":-5,"big":18446744073709551615})");
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    sb::require_true(d.root()["s"].try_i64().is_second());
    sb::require_true(d.root()["s"].try_i64().cast<cjson::error>() == cjson::error::wrong_type);
    sb::require_true(d.root()["n"].try_u64().cast<cjson::error>() == cjson::error::out_of_range);
    sb::require_true(d.root()["big"].try_i64().cast<cjson::error>() == cjson::error::out_of_range);
    sb::require_true(d.root()["big"].try_u64().is_first());
    sb::end_test_case();
  }
  {
    sb::test_case("stop_when_done consumes one root and reports the cut for ndjson");
    const char *nd = "{\"a\":1}\n{\"a\":2}\n{\"a\":3}\n";
    usize len = 0;
    while ( nd[len] ) ++len;
    usize off = 0;
    i64 sum = 0;
    u32 docs = 0;
    while ( off < len ) {
      auto r = cjson::parse(nd + off, len - off, cjson::opts{ .stop_when_done = true });
      if ( r.is_second() ) break;
      const cjson::doc &d = r.cast<cjson::doc>();
      sum += d.root()["a"].i64_or(0);
      off += d.consumed();
      ++docs;
      while ( off < len and (nd[off] == '\n' or nd[off] == ' ') ) ++off;
    }
    sb::require(docs, 3u);
    sb::require(sum, static_cast<i64>(6));
    sb::end_test_case();
  }
  {
    sb::test_case("rejects mirror the validate fsm");
    sb::require_true(P("").is_second());
    sb::require_true(P("[1,]").is_second());
    sb::require_true(P("{\"a\":}").is_second());
    sb::require_true(P("[}").is_second());
    sb::require_true(P("{\"a\":1,}").is_second());
    sb::require_true(P("1 2").is_second());
    sb::require_true(P("tru").is_second());
    sb::require_true(P("[01]").is_second());
    sb::end_test_case();
  }
  {
    sb::test_case("the sample corpus parses and spot fields resolve");
    auto tw = tutil::slurp("sample/twitter.json");
    sb::require_greater(tw.size(), static_cast<usize>(0));
    auto r = cjson::parse(tutil::view(tw));
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    sb::require_greater(d.size(), static_cast<usize>(1000));
    auto results = d.root()["statuses"];
    sb::require_true(results.type() == cjson::kind::array);
    sb::require_greater(results.size(), static_cast<usize>(10));
    sb::require_greater(results[usize(0)]["id"].u64_or(0), static_cast<u64>(0));
    const char *others[] = { "sample/64kb.json", "sample/128KB.json", "sample/512KB.json", "sample/1MB.json" };
    for ( const char *f : others ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      sb::require_true(cjson::parse(tutil::view(data)).is_first());
    }
    // corpus reality, verified against an independent oracle: sample.json is broken json
    // (pre-existing working-tree damage) and flowers.json is ndjson
    {
      auto broken = tutil::slurp("sample/sample.json");
      sb::require_true(cjson::parse(tutil::view(broken)).is_second());
      auto nd = tutil::slurp("sample/flowers.json");
      sb::require_true(cjson::parse(tutil::view(nd)).is_second());
      sb::require_true(cjson::parse(tutil::view(nd), cjson::opts{ .stop_when_done = true }).is_first());
    }
    sb::end_test_case();
  }
  {
    sb::test_case("deep nesting parses to the limit and rejects one past it");
    micron::vector<u8> deep;
    const u32 n = cjson::depth_limit;
    for ( u32 i = 0; i < n; i++ ) deep.push_back(u8('['));
    deep.push_back(u8('1'));
    for ( u32 i = 0; i < n; i++ ) deep.push_back(u8(']'));
    sb::require_true(cjson::parse(cjson::bytes{ deep.cbegin(), deep.size() }).is_first());
    micron::vector<u8> over;
    for ( u32 i = 0; i < n + 1; i++ ) over.push_back(u8('['));
    over.push_back(u8('1'));
    for ( u32 i = 0; i < n + 1; i++ ) over.push_back(u8(']'));
    auto r = cjson::parse(cjson::bytes{ over.cbegin(), over.size() });
    sb::require_true(r.is_second());
    sb::require_true(r.cast<cjson::error>() == cjson::error::depth_exceeded);
    sb::end_test_case();
  }
  return 1;
}
