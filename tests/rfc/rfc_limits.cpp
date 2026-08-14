//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// rfc 8259 s9, the parser's licence to be finite:
//
//   "A JSON parser MUST accept all texts that conform to the JSON grammar. A JSON parser
//    MAY accept non-JSON forms or extensions.
//    An implementation may set limits on the size of texts that it accepts. An
//    implementation may set limits on the maximum depth of nesting. An implementation may
//    set limits on the range and precision of numbers. An implementation may set limits
//    on the length and character contents of strings."
//
// Every limit cjson imposes and every extension it offers is therefore conforming -- but
// only if it is DOCUMENTED and the boundary sits where the docs say. That is what this
// file pins, so tests/rfc/COMPLIANCE.md can never drift away from the code.

#include "rfc_cases.hpp"

#include "../tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

// n opens, a scalar, n closes -- with `open`/`close` chosen by the caller
micron::vector<u8>
nest(u32 n, u8 open, u8 close, const char *core)
{
  micron::vector<u8> d;
  d.reserve(2 * n + 16);
  for ( u32 i = 0; i < n; ++i ) d.push_back(open);
  for ( usize i = 0; core[i]; ++i ) d.push_back(u8(core[i]));
  for ( u32 i = 0; i < n; ++i ) d.push_back(close);
  return d;
}

// n nested objects: {"a":{"a":...{"a":1}...}}
micron::vector<u8>
nest_obj(u32 n)
{
  micron::vector<u8> d;
  d.reserve(8 * n + 16);
  for ( u32 i = 0; i < n; ++i ) {
    d.push_back(u8('{'));
    d.push_back(u8('"'));
    d.push_back(u8('a'));
    d.push_back(u8('"'));
    d.push_back(u8(':'));
  }
  d.push_back(u8('1'));
  for ( u32 i = 0; i < n; ++i ) d.push_back(u8('}'));
  return d;
}

};      // namespace

int
main()
{
  {
    // s9 permits a depth cap. cjson's is CJSON_DEPTH_LIMIT, default 1024, and it must
    // land on error::depth_exceeded rather than a generic syntax error -- a caller
    // needs to tell "too deep" from "malformed".
    sb::test_case("s9: the nesting cap is exactly depth_limit, for arrays");
    const u32 n = cjson::depth_limit;
    {
      auto ok = nest(n, u8('['), u8(']'), "1");
      sb::require_true(cjson::validate(ok.cbegin(), ok.size()) == cjson::error::ok);
      sb::require_true(cjson::parse(cjson::bytes{ ok.cbegin(), ok.size() }).is_first());
    }
    {
      auto over = nest(n + 1, u8('['), u8(']'), "1");
      sb::require_true(cjson::validate(over.cbegin(), over.size()) == cjson::error::depth_exceeded);
      auto r = cjson::parse(cjson::bytes{ over.cbegin(), over.size() });
      sb::require_true(r.is_second());
      sb::require_true(r.cast<cjson::error>() == cjson::error::depth_exceeded);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("s9: the nesting cap is exactly depth_limit, for objects");
    const u32 n = cjson::depth_limit;
    {
      auto ok = nest_obj(n);
      sb::require_true(cjson::validate(ok.cbegin(), ok.size()) == cjson::error::ok);
      sb::require_true(cjson::parse(cjson::bytes{ ok.cbegin(), ok.size() }).is_first());
    }
    {
      auto over = nest_obj(n + 1);
      sb::require_true(cjson::validate(over.cbegin(), over.size()) == cjson::error::depth_exceeded);
      auto r = cjson::parse(cjson::bytes{ over.cbegin(), over.size() });
      sb::require_true(r.is_second());
      sb::require_true(r.cast<cjson::error>() == cjson::error::depth_exceeded);
    }
    sb::end_test_case();
  }
  {
    // DOCUMENTED off-by-one, pinned here as well as in tests/comptime.cpp: the {} and []
    // fast paths short-circuit BEFORE the depth counter increments (simdjson semantics),
    // so a nest whose innermost value is an EMPTY container survives one level deeper
    // than one whose innermost value is a scalar.
    sb::test_case("s9: the empty-container fast path is one level deeper, by design");
    const u32 n = cjson::depth_limit;
    {
      auto d = nest(n + 1, u8('['), u8(']'), "");      // innermost is []
      sb::require_true(cjson::validate(d.cbegin(), d.size()) == cjson::error::ok);
    }
    {
      auto d = nest(n + 2, u8('['), u8(']'), "");
      sb::require_true(cjson::validate(d.cbegin(), d.size()) == cjson::error::depth_exceeded);
    }
    sb::end_test_case();
  }
  {
    // every entry point must agree about where the cap is; a validate that accepts what
    // parse refuses on DEPTH would be a far worse divergence than the documented one on
    // number range (F3), because depth is not a representability question
    sb::test_case("s9: every entry point agrees on the depth cap");
    for ( u32 delta : { u32(0), u32(1) } ) {
      auto d = nest(cjson::depth_limit + delta, u8('['), u8(']'), "1");
      const cjson::bytes v{ d.cbegin(), d.size() };
      const bool want = (delta == 0);

      sb::require_true((cjson::validate(v) == cjson::error::ok) == want);
      sb::require_true(cjson::is_valid(v) == want);
      sb::require_true(cjson::parse(v).is_first() == want);

      micron::vector<u8> out;
      out.reserve(cjson::minify_bound(d.size()) + cjson::padding);
      sb::require_true((cjson::minify(v, cjson::wbytes{ out.begin(), cjson::minify_bound(d.size()) }) >= 0) == want);

      cjson::scratch sc;
      sb::require_true(cjson::iterate(v, cjson::opts{ .check_grammar = true }, sc).is_first() == want);

      micron::vector<u8> mut = d.clone();
      sb::require_true(cjson::parse_insitu(cjson::wbytes{ mut.begin(), mut.size() }).is_first() == want);
    }
    sb::end_test_case();
  }
  {
    // s9 permits a cap on string length; cjson imposes none short of the input cap, so
    // long strings must simply work
    sb::test_case("s9: cjson sets no string-length limit short of the input cap");
    for ( u32 len : { 1u, 64u, 65u, 4095u, 4096u, 65536u } ) {
      micron::vector<u8> d;
      d.reserve(len + 8);
      d.push_back(u8('"'));
      for ( u32 i = 0; i < len; ++i ) d.push_back(u8('a' + (i % 26)));
      d.push_back(u8('"'));
      auto r = cjson::parse(cjson::bytes{ d.cbegin(), d.size() });
      sb::require_true(r.is_first());
      sb::require(r.cast<cjson::doc>().root().str_or().len, static_cast<usize>(len));
    }
    sb::end_test_case();
  }
  {
    // s9 permits a cap on the number of members / elements too; there is none
    sb::test_case("s9: cjson sets no element-count limit");
    micron::vector<u8> d;
    d.push_back(u8('['));
    const u32 n = 50000;
    for ( u32 i = 0; i < n; ++i ) {
      if ( i ) d.push_back(u8(','));
      d.push_back(u8('0' + (i % 10)));
    }
    d.push_back(u8(']'));
    auto r = cjson::parse(cjson::bytes{ d.cbegin(), d.size() });
    sb::require_true(r.is_first());
    sb::require(r.cast<cjson::doc>().root().size(), static_cast<usize>(n));
    sb::end_test_case();
  }
  {
    // s9: "A JSON parser MAY accept non-JSON forms or extensions." stop_when_done is
    // exactly such an extension -- OFF by default, so the default configuration remains
    // strictly conforming, and ON it consumes one root and reports where it stopped.
    sb::test_case("s9: stop_when_done is an opt-in extension, off by default");
    const char *nd = "{\"a\":1}\n{\"b\":2}\n{\"c\":3}\n";
    usize n = 0;
    while ( nd[n] ) ++n;
    const u8 *p = reinterpret_cast<const u8 *>(nd);

    // strict by default
    sb::require_true(cjson::validate(p, n) == cjson::error::trailing_garbage);
    sb::require_true(cjson::parse(p, n).is_second());

    // and with the extension, one record at a time
    usize off = 0;
    u32 seen = 0;
    while ( off < n ) {
      auto r = cjson::parse(p + off, n - off, cjson::opts{ .stop_when_done = true });
      if ( !r.is_first() ) break;
      const cjson::doc &d = r.cast<cjson::doc>();
      sb::require_greater(d.consumed(), static_cast<usize>(0));
      off += d.consumed();
      ++seen;
      while ( off < n and (p[off] == u8('\n') or p[off] == u8(' ')) ) ++off;
    }
    sb::require(seen, static_cast<u32>(3));

    // the corpus's own ndjson file behaves the same way
    auto flowers = tutil::slurp("sample/flowers.json");
    sb::require_greater(flowers.size(), static_cast<usize>(0));
    sb::require_true(cjson::parse(tutil::view(flowers)).is_second());
    sb::require_true(cjson::parse(tutil::view(flowers), cjson::opts{ .stop_when_done = true }).is_first());
    sb::end_test_case();
  }
  {
    // s9 range limits again, from the other side: numbers_as_raw declines to convert, so
    // it declines to impose a range -- an extension that WIDENS what is accepted
    sb::test_case("s9: numbers_as_raw is an opt-in extension that lifts the range limit");
    const char *p = "[1e309,1e-400,123456789012345678901234567890123456789012345678901234567890]";
    usize n = 0;
    while ( p[n] ) ++n;
    const u8 *b = reinterpret_cast<const u8 *>(p);

    sb::require_true(cjson::parse(b, n).is_second());
    auto r = cjson::parse(b, n, cjson::opts{ .numbers_as_raw = true });
    sb::require_true(r.is_first());
    sb::require(r.cast<cjson::doc>().root().size(), static_cast<usize>(3));

    // the raw token is preserved byte for byte, which is the point of the mode
    auto first = r.cast<cjson::doc>().root()[usize(0)];
    sb::require(first.type(), cjson::kind::raw);
    sb::end_test_case();
  }
  {
    // s9's grammar duty runs the other way too: "A JSON parser MUST accept all texts that
    // conform to the JSON grammar." Whatever limits are set, the whole valid corpus is
    // inside them.
    sb::test_case("s9: every conforming corpus document is accepted");
    const char *files[] = { "sample/64kb.json", "sample/128KB.json", "sample/256KB.json",   "sample/512KB.json",
                            "sample/1MB.json",  "sample/5MB.json",   "sample/twitter.json", "sample/large-file.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      const cjson::error e = cjson::validate(tutil::view(data));
      if ( e != cjson::error::ok ) snowball::print("s9 MUST-accept FAILED: ", f, " -> ", cjson::error_name(e));
      sb::require_true(e == cjson::error::ok);
      sb::require_true(cjson::parse(tutil::view(data)).is_first());
    }
    sb::end_test_case();
  }
  return 1;
}
