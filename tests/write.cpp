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

// writer semantics: parse-write fixpoint over the corpus, itoa digit boundaries, dtoa
// shortest-round-trip (our strtod-verified parser is the decoder), pretty reparses equal

namespace
{

micron::string
W(const char *s, cjson::style st = {})
{
  usize n = 0;
  while ( s[n] ) ++n;
  auto r = cjson::parse(s, n);
  if ( r.is_second() ) return micron::string{};
  return cjson::write_str(r.cast<cjson::doc>(), st);
}

bool
text_is(const char *jdoc, const char *want)
{
  micron::string got = W(jdoc);
  usize wn = 0;
  while ( want[wn] ) ++wn;
  if ( got.size() != wn ) return false;
  for ( usize i = 0; i < wn; i++ )
    if ( got[i] != want[i] ) return false;
  return true;
}

// semantic equality via re-serialization comparison
bool
fixpoint(const u8 *p, usize n)
{
  auto r1 = cjson::parse(p, n);
  if ( r1.is_second() ) return false;
  micron::string w1 = cjson::write_str(r1.cast<cjson::doc>());
  auto r2 = cjson::parse(w1.c_str(), w1.size());
  if ( r2.is_second() ) return false;
  micron::string w2 = cjson::write_str(r2.cast<cjson::doc>());
  if ( w1.size() != w2.size() ) return false;
  for ( usize i = 0; i < w1.size(); i++ )
    if ( w1[i] != w2[i] ) return false;
  return true;
}

};      // namespace

int
main()
{
  {
    sb::test_case("scalar and container shapes serialize to canonical text");
    sb::require_true(text_is("null", "null"));
    sb::require_true(text_is("true", "true"));
    sb::require_true(text_is("false", "false"));
    sb::require_true(text_is("[]", "[]"));
    sb::require_true(text_is("{}", "{}"));
    sb::require_true(text_is(" [ 1 , 2 , 3 ] ", "[1,2,3]"));
    sb::require_true(text_is(R"({"a":1,"b":[true,{}],"c":"x"})", R"({"a":1,"b":[true,{}],"c":"x"})"));
    sb::require_true(text_is(R"("caf\u00e9")", "\"caf\xc3\xa9\""));
    sb::require_true(text_is(R"("q\"b\\s\nn")", R"("q\"b\\s\nn")"));
    sb::require_true(text_is("\"\\u0001\"", "\"\\u0001\""));
    sb::end_test_case();
  }
  {
    sb::test_case("integers write at every digit-count boundary");
    sb::require_true(text_is("0", "0"));
    sb::require_true(text_is("-0", "0"));      // -0 parses to int 0 (yyjson semantics)
    sb::require_true(text_is("9", "9"));
    sb::require_true(text_is("10", "10"));
    sb::require_true(text_is("99", "99"));
    sb::require_true(text_is("100", "100"));
    sb::require_true(text_is("9999", "9999"));
    sb::require_true(text_is("10000", "10000"));
    sb::require_true(text_is("999999", "999999"));
    sb::require_true(text_is("1000000", "1000000"));
    sb::require_true(text_is("99999999", "99999999"));
    sb::require_true(text_is("100000000", "100000000"));
    sb::require_true(text_is("9999999999999999", "9999999999999999"));
    sb::require_true(text_is("10000000000000000", "10000000000000000"));
    sb::require_true(text_is("18446744073709551615", "18446744073709551615"));
    sb::require_true(text_is("-9223372036854775808", "-9223372036854775808"));
    sb::require_true(text_is("-1", "-1"));
    sb::end_test_case();
  }
  {
    sb::test_case("doubles write shortest forms in the yyjson style");
    sb::require_true(text_is("1.0", "1.0"));
    sb::require_true(text_is("0.5", "0.5"));
    sb::require_true(text_is("0.1", "0.1"));
    sb::require_true(text_is("3.14159", "3.14159"));
    sb::require_true(text_is("-2.5", "-2.5"));
    sb::require_true(text_is("1e2", "100.0"));
    sb::require_true(text_is("1.5e300", "1.5e300"));
    sb::require_true(text_is("1e-7", "1e-7"));
    sb::require_true(text_is("0.0001", "0.0001"));
    sb::require_true(text_is("5e-324", "5e-324"));
    sb::require_true(text_is("123456789012345680000.0", "123456789012345680000.0"));
    sb::end_test_case();
  }
  {
    sb::test_case("random doubles round-trip bit-identically through write then parse");
    tutil::rng rg;
    u32 tested = 0;
    for ( u32 iter = 0; iter < 400000; iter++ ) {
      const u64 bits = rg.next();
      if ( (bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull ) continue;
      const f64 x = __builtin_bit_cast(f64, bits);
      u8 buf[48];
      u8 *end = cjson::__dtoa::write_f64(buf, x);
      auto r = cjson::parse(reinterpret_cast<const char *>(buf), usize(end - buf));
      if ( r.is_second() ) {
        sb::print("dtoa text unparseable at iter ", iter);
        sb::require_true(false);
      }
      const f64 back = r.cast<cjson::doc>().root().f64_or(1.5);
      if ( __builtin_bit_cast(u64, back) != bits ) {
        sb::print("dtoa round-trip mismatch at iter ", iter);
        sb::require_true(false);
      }
      ++tested;
    }
    sb::require_greater(tested, 300000u);
    sb::end_test_case();
  }
  {
    sb::test_case("the corpus reaches a serialization fixpoint");
    const char *files[] = { "sample/64kb.json", "sample/128KB.json", "sample/twitter.json", "sample/1MB.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      sb::require_true(fixpoint(data.cbegin(), data.size()));
    }
    sb::end_test_case();
  }
  {
    sb::test_case("the stage-2 bound accumulators equal the bound walks exactly");
    const char *files[] = { "sample/64kb.json", "sample/128KB.json", "sample/twitter.json", "sample/1MB.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      auto r = cjson::parse(data.cbegin(), data.size(), cjson::opts{ .with_write_bound = true });
      sb::require_true(r.is_first());
      const cjson::doc &d = r.cast<cjson::doc>();
      const u8 indents[] = { 0, 2, 4 };
      for ( const u8 ind : indents ) {
        const usize walk = cjson::__write::bound_slots(d.root().__raw(), d.size()) + cjson::__write::pretty_extra(d.root().__raw(), ind);
        sb::require(cjson::write_bound(d, cjson::style{ .indent = ind }), walk);
      }
      // without the opt-in the doc carries no parts and write_bound falls back to the
      // walks — same value either way
      auto r2 = cjson::parse(data.cbegin(), data.size());
      sb::require_true(r2.is_first());
      const cjson::doc &d2 = r2.cast<cjson::doc>();
      sb::require(cjson::write_bound(d2, cjson::style{}),
                  cjson::__write::bound_slots(d2.root().__raw(), d2.size()) + cjson::__write::pretty_extra(d2.root().__raw(), 0));
    }
    // raw-numbers docs land in bound_slots' default arm — pin that mirror too
    {
      const char *j = R"({"n":[1,2.5e10,-3],"s":"x"})";
      auto r = cjson::parse(j, 27, cjson::opts{ .numbers_as_raw = true, .with_write_bound = true });
      sb::require_true(r.is_first());
      const cjson::doc &d = r.cast<cjson::doc>();
      const usize walk = cjson::__write::bound_slots(d.root().__raw(), d.size()) + cjson::__write::pretty_extra(d.root().__raw(), 2);
      sb::require(cjson::write_bound(d, cjson::style{ .indent = 2 }), walk);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("pretty output reparses to the same document");
    const char *j = R"({"a":{"b":[1,2,{"c":null}],"d":"x"},"e":[],"f":{},"g":[true,false]})";
    usize n = 0;
    while ( j[n] ) ++n;
    micron::string pretty = W(j, cjson::style{ .indent = 2 });
    sb::require_greater(pretty.size(), n);
    auto rp = cjson::parse(pretty.c_str(), pretty.size());
    sb::require_true(rp.is_first());
    micron::string mini1 = W(j);
    micron::string mini2 = cjson::write_str(rp.cast<cjson::doc>());
    sb::require(mini1.size(), mini2.size());
    bool same = true;
    for ( usize i = 0; i < mini1.size(); i++ ) same = same and mini1[i] == mini2[i];
    sb::require_true(same);
    sb::end_test_case();
  }
  {
    sb::test_case("write_into respects the bound contract");
    const char *j = R"([1,2,3])";
    auto r = cjson::parse(j, 7);
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    u8 big[256];
    const max_t w = cjson::write_into(d, cjson::wbytes{ big, sizeof(big) });
    sb::require_true(w == 7);
    u8 tiny[4];
    sb::require_true(cjson::write_into(d, cjson::wbytes{ tiny, sizeof(tiny) }) == cjson::fail(cjson::error::short_output));
    sb::end_test_case();
  }
  {
    sb::test_case("comptime writer agrees with runtime");
    constexpr auto ct = []() consteval -> bool {
      u8 buf[48];
      u8 *e = cjson::__dtoa::write_f64(buf, 0.1);
      const char want[] = "0.1";
      if ( usize(e - buf) != 3 ) return false;
      for ( usize i = 0; i < 3; ++i )
        if ( buf[i] != u8(want[i]) ) return false;
      u8 ib[24];
      u8 *ie = cjson::__itoa::write_u64(ib, 18446744073709551615ull);
      return usize(ie - ib) == 20;
    };
    static_assert(ct());
    sb::require_true(true);
    sb::end_test_case();
  }
  return 1;
}
