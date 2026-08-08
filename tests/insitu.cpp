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

// insitu contract: values alias the caller's buffer (no pool copy), strings unescape
// and nul-terminate in place, the resulting doc is semantically identical to copy-mode,
// and the raw-numbers option stores tokens byte-exact

int
main()
{
  {
    sb::test_case("insitu parses in place and aliases the caller buffer");
    const char *j = R"({"name":"café","n":42,"list":[1,2]})";
    micron::vector<u8> buf;
    for ( usize i = 0; j[i]; i++ ) buf.push_back(u8(j[i]));
    const usize n = buf.size();
    auto r = cjson::parse_insitu(cjson::wbytes{ buf.begin(), n });
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    sb::require_true(d.pool() == buf.begin());      // aliased, not copied
    auto name = d.root()["name"].str_or();
    sb::require(name.len, static_cast<usize>(5));
    sb::require_true(u8(name.ptr[3]) == 0xc3 and u8(name.ptr[4]) == 0xa9);
    sb::require_true(name.ptr[name.len] == 0);      // nul-terminated in the caller buffer
    sb::require_true(d.root()["n"].i64_or(0) == 42);
    sb::require_true(d.root()["list"][usize(1)].i64_or(0) == 2);
    sb::end_test_case();
  }
  {
    sb::test_case("insitu and copy mode agree semantically over the corpus");
    const char *files[] = { "sample/64kb.json", "sample/twitter.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      auto rc = cjson::parse(tutil::view(data));
      micron::vector<u8> mut = data.clone();
      auto ri = cjson::parse_insitu(cjson::wbytes{ mut.begin(), mut.size() });
      sb::require_true(rc.is_first() and ri.is_first());
      micron::string wc = cjson::write_str(rc.cast<cjson::doc>());
      micron::string wi = cjson::write_str(ri.cast<cjson::doc>());
      sb::require(wc.size(), wi.size());
      bool same = true;
      for ( usize i = 0; i < wc.size(); i++ ) same = same and wc[i] == wi[i];
      sb::require_true(same);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("numbers_as_raw stores tokens byte-exact");
    const char *j = R"([1e2, -0.500, 18446744073709551616, 0])";
    usize n = 0;
    while ( j[n] ) ++n;
    auto r = cjson::parse(j, n, cjson::opts{ .numbers_as_raw = true });
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    auto a0 = d.root()[usize(0)].raw_str();
    sb::require(a0.len, static_cast<usize>(3));
    sb::require_true(a0.ptr[0] == '1' and a0.ptr[1] == 'e' and a0.ptr[2] == '2');
    auto a1 = d.root()[usize(1)].raw_str();
    sb::require(a1.len, static_cast<usize>(6));
    auto a2 = d.root()[usize(2)].raw_str();
    sb::require(a2.len, static_cast<usize>(20));      // wider than u64, preserved as text
    sb::require_true(d.root()[usize(3)].raw_str().len == 1);
    // typed getters on raw values yield defaults, not conversions
    sb::require_true(d.root()[usize(0)].i64_or(-1) == -1);
    // bad grammar still rejects in raw mode
    sb::require_true(cjson::parse("[01]", 4, cjson::opts{ .numbers_as_raw = true }).is_second());
    sb::end_test_case();
  }
  {
    sb::test_case("warm scratch reuses its index buffer across parses");
    cjson::scratch sc;
    const char *j = R"({"a":[1,2,3],"b":"x"})";
    usize n = 0;
    while ( j[n] ) ++n;
    auto r1 = cjson::parse(j, n, {}, sc);
    sb::require_true(r1.is_first());
    const u32 *idx_before = sc.idx;
    const usize cap_before = sc.idx_cap;
    for ( u32 i = 0; i < 1000; i++ ) {
      auto r = cjson::parse(j, n, {}, sc);
      sb::require_true(r.is_first());
    }
    sb::require_true(sc.idx == idx_before and sc.idx_cap == cap_before);
    // iterate mode allocates nothing at all once the scratch is warm
    for ( u32 i = 0; i < 1000; i++ ) {
      auto rv = cjson::iterate(j, n, {}, sc);
      sb::require_true(rv.is_first());
    }
    sb::require_true(sc.idx == idx_before);
    sb::end_test_case();
  }
  {
    sb::test_case("reuse mode retains all three buffers across parses");
    cjson::scratch sc;
    const char *j = R"({"a":[1,2,3],"b":"x","c":{"d":true}})";
    usize n = 0;
    while ( j[n] ) ++n;
    {
      auto r1 = cjson::parse_reuse(j, n, {}, sc);
      sb::require_true(r1.is_first());
      sb::require_true(r1.cast<cjson::doc>().borrowed());
    }
    const u32 *idx0 = sc.idx;
    const usize idxc0 = sc.idx_cap;
    const u8 *pool0 = sc.pool;
    const usize poolc0 = sc.pool_cap;
    const cjson::value *vals0 = sc.vals;
    const usize valsc0 = sc.vals_cap;
    sb::require_true(pool0 != nullptr and vals0 != nullptr);
    for ( u32 i = 0; i < 1000; i++ ) {
      auto r = cjson::parse_reuse(j, n, {}, sc);
      sb::require_true(r.is_first());
      sb::require_true(r.cast<cjson::doc>().root()["a"][usize(2)].i64_or(0) == 3);
    }
    // the whole point: a warm scratch does no allocator work at all
    sb::require_true(sc.idx == idx0 and sc.idx_cap == idxc0);
    sb::require_true(sc.pool == pool0 and sc.pool_cap == poolc0);
    sb::require_true(sc.vals == vals0 and sc.vals_cap == valsc0);
    sb::end_test_case();
  }
  {
    sb::test_case("owning parse leaves the reuse buffers alone");
    // the three buffers must release independently: if scratch::ensure() still shared one
    // release(), growing idx here would free the pool and vals under the live doc below
    cjson::scratch sc;
    const char *small = R"({"a":1})";
    usize sn = 0;
    while ( small[sn] ) ++sn;
    auto rr = cjson::parse_reuse(small, sn, {}, sc);
    sb::require_true(rr.is_first());
    const u8 *pool0 = sc.pool;
    const cjson::value *vals0 = sc.vals;
    const usize poolc0 = sc.pool_cap;
    const usize valsc0 = sc.vals_cap;

    micron::vector<u8> big;
    big.push_back(u8('['));
    for ( u32 i = 0; i < 4000; i++ ) {
      big.push_back(u8('1'));
      big.push_back(u8(','));
    }
    big.push_back(u8('1'));
    big.push_back(u8(']'));
    {
      auto ro = cjson::parse(tutil::view(big), {}, sc);      // forces idx growth
      sb::require_true(ro.is_first());
      sb::require_true(!ro.cast<cjson::doc>().borrowed());      // owning doc frees its own
    }
    sb::require_true(sc.pool == pool0 and sc.pool_cap == poolc0);
    sb::require_true(sc.vals == vals0 and sc.vals_cap == valsc0);
    sb::require_true(rr.cast<cjson::doc>().root()["a"].i64_or(0) == 1);      // still readable
    sb::end_test_case();
  }
  {
    sb::test_case("a borrowed doc survives its own move");
    cjson::scratch sc;
    const char *j = R"({"k":"value","n":7})";
    usize n = 0;
    while ( j[n] ) ++n;
    auto r = cjson::parse_reuse(j, n, {}, sc);
    sb::require_true(r.is_first());
    cjson::doc d2 = micron::move(r.cast<cjson::doc>());
    sb::require_true(d2.borrowed());
    sb::require_true(!r.cast<cjson::doc>().borrowed());      // flag left the source
    sb::require_true(d2.root()["n"].i64_or(0) == 7);
    cjson::doc d3;
    d3 = micron::move(d2);
    sb::require_true(d3.borrowed() and !d2.borrowed());
    sb::require_true(d3.root()["k"].str_or().len == 5);
    sb::end_test_case();      // both destructors run here: a lost flag is a double free
  }
  {
    sb::test_case("a shrinking reuse parse never sees the previous document's pad");
    // stage 1 masks only the structurals it RETURNS: prev_in_string, ctrl_err and the utf-8
    // state all accumulate from the unmasked tail block, so stale bytes in [len, len+64)
    // of a retained pool would fabricate bad_string / bad_utf8 on a shorter input
    cjson::scratch sc;
    micron::vector<u8> big;
    big.push_back(u8('['));
    for ( u32 i = 0; i < 300; i++ ) {
      big.push_back(u8('"'));
      big.push_back(u8('c'));
      big.push_back(u8(0xc3));      // cafe-style 2-byte sequence, so the checker has state
      big.push_back(u8(0xa9));
      big.push_back(u8('\\'));
      big.push_back(u8('"'));
      big.push_back(u8('"'));
      big.push_back(u8(','));
    }
    big.push_back(u8('1'));
    big.push_back(u8(']'));
    sb::require_true(cjson::parse_reuse(tutil::view(big), {}, sc).is_first());

    // sweep every length whose tail block reaches into the stale region, both utf8 modes.
    // "[" + "1"*(len-2) + "]" is valid json at every len >= 2
    for ( usize len = 2; len <= 200; len++ ) {
      micron::vector<u8> small;
      small.push_back(u8('['));
      for ( usize i = 0; i + 2 < len; i++ ) small.push_back(u8('1'));
      small.push_back(u8(']'));
      sb::require(small.size(), len);
      auto ra = cjson::parse_reuse(tutil::view(small), {}, sc);
      sb::require_true(ra.is_first());
      auto rb = cjson::parse_reuse(tutil::view(small), cjson::opts{ .skip_utf8 = true }, sc);
      sb::require_true(rb.is_first());
      sb::require_true(cjson::parse_reuse(tutil::view(big), {}, sc).is_first());      // re-dirty
    }
    sb::end_test_case();
  }
  {
    sb::test_case("reuse mode agrees with owning mode over the corpus");
    cjson::scratch sc;
    const char *files[] = { "sample/64kb.json", "sample/128KB.json", "sample/twitter.json", "sample/5MB.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      auto ro = cjson::parse(tutil::view(data));
      auto rr = cjson::parse_reuse(tutil::view(data), {}, sc);
      sb::require_true(ro.is_first() and rr.is_first());
      micron::string wo = cjson::write_str(ro.cast<cjson::doc>());
      micron::string wr = cjson::write_str(rr.cast<cjson::doc>());
      sb::require(wo.size(), wr.size());
      bool same = true;
      for ( usize i = 0; i < wo.size(); i++ ) same = same and wo[i] == wr[i];
      sb::require_true(same);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("reuse mode rejects identically and stays usable afterwards");
    cjson::scratch sc;
    const char *good = R"({"ok":[1,2]})";
    usize gn = 0;
    while ( good[gn] ) ++gn;
    const char *bad[] = { "[01]", "{\"a\":}", "[1,]", "\"unterminated", "[1 2]", "{'a':1}", "[\x01\"]", "tru" };
    for ( const char *b : bad ) {
      usize bn = 0;
      while ( b[bn] ) ++bn;
      auto ro = cjson::parse(b, bn);
      auto rr = cjson::parse_reuse(b, bn, {}, sc);
      sb::require_true(ro.is_second() and rr.is_second());
      sb::require_true(ro.cast<cjson::error>() == rr.cast<cjson::error>());
      // a failed reuse parse must leave the scratch fit for the next one
      auto rg = cjson::parse_reuse(good, gn, {}, sc);
      sb::require_true(rg.is_first());
      sb::require_true(rg.cast<cjson::doc>().root()["ok"][usize(1)].i64_or(0) == 2);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("insitu reuse borrows the slab and aliases the caller buffer");
    cjson::scratch sc;
    const char *j = R"({"name":"café","list":[1,2,3]})";
    micron::vector<u8> buf;
    for ( usize i = 0; j[i]; i++ ) buf.push_back(u8(j[i]));
    auto r = cjson::parse_insitu_reuse(cjson::wbytes{ buf.begin(), buf.size() }, {}, sc);
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    sb::require_true(d.borrowed());
    sb::require_true(d.pool() == buf.begin());                        // aliased, no pool involved
    sb::require_true(sc.vals != nullptr and sc.pool == nullptr);      // slab only, no pool
    sb::require_true(d.root()["list"][usize(2)].i64_or(0) == 3);
    sb::end_test_case();
  }
  return 1;
}
