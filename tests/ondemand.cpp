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

// on-demand cursor: zero-copy extraction over caller bytes must agree with the dom on
// every field it can reach; misuse chains to none; the jwt shape works from a stack
// buffer with zero heap traffic on the cjson side

namespace
{

cjson::scratch g_sc;

};      // namespace

int
main()
{
  {
    sb::test_case("cursor extraction agrees with the dom across a nested document");
    const char *j = R"({"a":{"b":[10,20,{"c":true}],"d":"x\ny"},"n":-5,"u":18446744073709551615,"f":2.5,"z":null})";
    usize n = 0;
    while ( j[n] ) ++n;
    auto rv = cjson::iterate(j, n, {}, g_sc);
    sb::require_true(rv.is_first());
    auto root = rv.cast<cjson::view>().root();
    sb::require_true(root["a"]["b"].at(1).i64_or(-1) == 20);
    sb::require_true(root["a"]["b"].at(2)["c"].bool_or(false));
    sb::require_true(root["n"].i64_or(0) == -5);
    sb::require_true(root["u"].u64_or(0) == 18446744073709551615ull);
    sb::require_true(root["f"].f64_or(0) == 2.5);
    sb::require_true(root["z"].is_null());
    sb::require_true(!root["missing"]);
    sb::require_true(!root["a"]["missing"]["chain"]);
    sb::require_true(root["a"]["b"].count() == 3);
    sb::require_true(root.count() == 5);
    // decoded string with escape
    u8 buf[16];
    const max_t w = root["a"]["d"].str(cjson::wbytes{ buf, sizeof(buf) });
    sb::require_true(w == 3 and buf[0] == 'x' and buf[1] == 0x0a and buf[2] == 'y');
    sb::end_test_case();
  }
  {
    sb::test_case("out-of-order and repeated gets work on the same view");
    const char *j = R"({"one":1,"two":2,"three":3})";
    usize n = 0;
    while ( j[n] ) ++n;
    auto rv = cjson::iterate(j, n, {}, g_sc);
    sb::require_true(rv.is_first());
    auto root = rv.cast<cjson::view>().root();
    sb::require_true(root["three"].i64_or(0) == 3);
    sb::require_true(root["one"].i64_or(0) == 1);
    sb::require_true(root["two"].i64_or(0) == 2);
    sb::require_true(root["one"].i64_or(0) == 1);      // repeat: cursors are independent
    sb::end_test_case();
  }
  {
    sb::test_case("the jwt shape extracts claims from a stack buffer");
    // a decoded jwt payload lives in a small stack buffer, never the heap
    const char payload[] = R"({"sub":"user-42","iss":"ox","exp":1735689600,"admin":false})";
    u8 stackbuf[sizeof(payload)];
    for ( usize i = 0; i + 1 < sizeof(payload); i++ ) stackbuf[i] = u8(payload[i]);
    auto rv = cjson::iterate(cjson::bytes{ stackbuf, sizeof(payload) - 1 }, {}, g_sc);
    sb::require_true(rv.is_first());
    auto root = rv.cast<cjson::view>().root();
    sb::require_true(root["exp"].i64_or(0) == 1735689600);
    auto sub = root["sub"].str_raw();
    sb::require(sub.len, static_cast<usize>(7));
    sb::require_true(sub.ptr[0] == 'u' and sub.ptr[5] == '4');
    sb::require_true(!root["admin"].bool_or(true));
    sb::end_test_case();
  }
  {
    sb::test_case("cursor gets agree with the dom on twitter fields");
    auto tw = tutil::slurp("sample/twitter.json");
    sb::require_greater(tw.size(), static_cast<usize>(0));
    auto rd = cjson::parse(tutil::view(tw));
    auto rv = cjson::iterate(tutil::view(tw), {}, g_sc);
    sb::require_true(rd.is_first() and rv.is_first());
    const cjson::doc &d = rd.cast<cjson::doc>();
    auto vroot = rv.cast<cjson::view>().root();
    auto droot = d.root();
    sb::require_true(vroot["statuses"].count() == droot["statuses"].size());
    for ( usize i = 0; i < 5; i++ ) {
      const u64 a = vroot["statuses"].at(i)["id"].u64_or(0);
      const u64 b = droot["statuses"][i]["id"].u64_or(1);
      sb::require_true(a == b and a != 0);
      // string equality via raw compare on a clean field
      auto sa = vroot["statuses"].at(i)["id_str"].str_raw();
      auto sb_ = droot["statuses"][i]["id_str"].str_or();
      sb::require(sa.len, sb_.len);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("type misuse yields defaults never crashes");
    const char *j = R"({"s":"x","n":1})";
    auto rv = cjson::iterate(j, 15, {}, g_sc);
    sb::require_true(rv.is_first());
    auto root = rv.cast<cjson::view>().root();
    sb::require_true(root["s"].i64_or(-7) == -7);
    sb::require_true(root["n"].str_raw().len == 0);
    sb::require_true(root["n"].at(0).peek_kind() == cjson::kind::none);
    sb::require_true(root.at(0).peek_kind() == cjson::kind::none);      // object not array
    u8 tiny[1];
    sb::require_true(root["s"].str(cjson::wbytes{ tiny, 1 }) == 1);
    sb::end_test_case();
  }
  {
    sb::test_case("iterate rejects structurally hopeless input early");
    const char *j = "\"unclosed";
    auto rv = cjson::iterate(j, 9, {}, g_sc);
    sb::require_true(rv.is_second());
    const char *w = "   ";
    auto rw = cjson::iterate(w, 3, {}, g_sc);
    sb::require_true(rw.is_second());
    sb::end_test_case();
  }
  {
    sb::test_case("items() walks an array forward and agrees with at()");
    const char *j = R"([10,20,30,40])";
    auto rv = cjson::iterate(j, 13, {}, g_sc);
    sb::require_true(rv.is_first());
    auto root = rv.cast<cjson::view>().root();

    usize n = 0;
    i64 sum = 0;
    for ( auto e : root.items() ) {
      sb::require_true(e.i64_or(-1) == root.at(n).i64_or(-2));
      sum += e.i64_or(0);
      ++n;
    }
    sb::require(n, usize(4));
    sb::require_true(sum == 100);
    sb::require(root.count(), usize(4));
    sb::end_test_case();
  }
  {
    sb::test_case("items() handles mixed and nested elements without materializing");
    const char *j = R"([1,"two",[3,4],{"k":5},null,true])";
    auto rv = cjson::iterate(j, 33, {}, g_sc);
    sb::require_true(rv.is_first());
    auto root = rv.cast<cjson::view>().root();

    cjson::kind seen[6]{};
    usize n = 0;
    for ( auto e : root.items() )
      if ( n < 6 ) seen[n++] = e.peek_kind();

    sb::require(n, usize(6));
    sb::require_true(seen[0] == cjson::kind::number);
    sb::require_true(seen[1] == cjson::kind::string);
    sb::require_true(seen[2] == cjson::kind::array);
    sb::require_true(seen[3] == cjson::kind::object);
    sb::require_true(seen[4] == cjson::kind::null);
    sb::require_true(seen[5] == cjson::kind::boolean);

    // the nested container is itself walkable
    usize inner = 0;
    for ( auto e : root.at(2).items() ) {
      (void)e;
      ++inner;
    }
    sb::require(inner, usize(2));
    sb::end_test_case();
  }
  {
    sb::test_case("members() yields raw keys paired with their values");
    const char *j = R"({"a":1,"bb":"x","ccc":[1,2],"d":{"e":9}})";
    auto rv = cjson::iterate(j, 40, {}, g_sc);
    sb::require_true(rv.is_first());
    auto root = rv.cast<cjson::view>().root();

    usize n = 0;
    usize keylen_total = 0;
    for ( auto m : root.members() ) {
      keylen_total += m.key.len;
      // every member's value must match what operator[] finds for the same key
      sb::require_true(m.v.peek_kind() == root[m.key].peek_kind());
      ++n;
    }
    sb::require(n, usize(4));
    sb::require(keylen_total, usize(1 + 2 + 3 + 1));
    sb::require(root.count(), usize(4));
    sb::require_true(root["d"].members().begin() != root["d"].members().end());
    sb::end_test_case();
  }
  {
    sb::test_case("empty containers and wrong kinds yield empty ranges");
    const char *j = R"({"e":[],"o":{},"n":7})";
    auto rv = cjson::iterate(j, 21, {}, g_sc);
    sb::require_true(rv.is_first());
    auto root = rv.cast<cjson::view>().root();

    usize n = 0;
    for ( auto e : root["e"].items() ) {
      (void)e;
      ++n;
    }
    for ( auto m : root["o"].members() ) {
      (void)m;
      ++n;
    }
    for ( auto e : root["n"].items() ) {      // number: not an array
      (void)e;
      ++n;
    }
    for ( auto m : root["n"].members() ) {      // number: not an object
      (void)m;
      ++n;
    }
    for ( auto e : root["missing"].items() ) {      // absent: a dead cursor
      (void)e;
      ++n;
    }
    sb::require(n, usize(0));
    sb::end_test_case();
  }
  {
    sb::test_case("members() over escaped keys hands back raw undecoded bytes");
    const char *j = R"({"a\nb":1,"c":2})";
    auto rv = cjson::iterate(j, 16, {}, g_sc);
    sb::require_true(rv.is_first());
    auto root = rv.cast<cjson::view>().root();

    auto it = root.members().begin();
    // raw: the two-byte escape sequence is still \ + n, exactly like str_raw()
    sb::require((*it).key.len, usize(4));
    sb::require_true((*it).key.ptr[1] == '\\');
    sb::require_true((*it).v.i64_or(0) == 1);
    ++it;
    sb::require((*it).key.len, usize(1));
    sb::require_true((*it).v.i64_or(0) == 2);
    sb::end_test_case();
  }
  {
    sb::test_case("a walked range is single pass — re-taking it restarts the walk");
    const char *j = R"([1,2,3])";
    auto rv = cjson::iterate(j, 7, {}, g_sc);
    sb::require_true(rv.is_first());
    auto root = rv.cast<cjson::view>().root();

    i64 first = 0, second = 0;
    for ( auto e : root.items() ) first += e.i64_or(0);
    for ( auto e : root.items() ) second += e.i64_or(0);
    sb::require_true(first == 6 and second == 6);
    sb::end_test_case();
  }
  return 1;
}
