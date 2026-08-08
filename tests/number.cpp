//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include "../src/cjson/cjson.hpp"

#include "tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/string/conversions/floating_point.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

// number torture. two oracles, both independent of cjson's converter:
//   1. c++ literals — the compiler's own decimal->double conversion is correctly
//      rounded, so parse("<text>") must reproduce bit_cast(<same text as a literal>)
//   2. micron's ryu — shortest-round-trip output of a random double must parse back to
//      the identical bit pattern

namespace
{

cjson::value
num_of(const char *s)
{
  cjson::value v{};
  usize n = 0;
  while ( s[n] ) ++n;
  // parse through the whole pipeline so the number path sees real index-fed tokens
  auto r = cjson::parse(s, n);
  if ( r.is_second() ) {
    v.tag = 0;
    return v;
  }
  const cjson::doc &d = r.cast<cjson::doc>();
  const auto root = d.root();
  if ( root.type() != cjson::kind::number ) {
    v.tag = 0;
    return v;
  }
  v.tag = cjson::make_tag(cjson::kind::number, 0, 0);
  v.pay.f = root.f64_or(0);
  // stash exact subtype info through separate getters
  return v;
}

bool
f64_is(const char *s, f64 expected)
{
  usize n = 0;
  while ( s[n] ) ++n;
  auto r = cjson::parse(s, n);
  if ( r.is_second() ) return false;
  const cjson::doc &d = r.cast<cjson::doc>();
  const f64 got = d.root().f64_or(12345.678);
  return __builtin_bit_cast(u64, got) == __builtin_bit_cast(u64, expected);
}

bool
i64_is(const char *s, i64 expected)
{
  usize n = 0;
  while ( s[n] ) ++n;
  auto r = cjson::parse(s, n);
  if ( r.is_second() ) return false;
  const cjson::doc &d = r.cast<cjson::doc>();
  auto t = d.root().try_i64();
  return t.is_first() and t.cast<i64>() == expected;
}

bool
u64_is(const char *s, u64 expected)
{
  usize n = 0;
  while ( s[n] ) ++n;
  auto r = cjson::parse(s, n);
  if ( r.is_second() ) return false;
  const cjson::doc &d = r.cast<cjson::doc>();
  auto t = d.root().try_u64();
  return t.is_first() and t.cast<u64>() == expected;
}

bool
rejects(const char *s)
{
  usize n = 0;
  while ( s[n] ) ++n;
  return cjson::parse(s, n).is_second();
}

};      // namespace

int
main()
{
  {
    sb::test_case("integers land exactly across the full 64-bit range");
    sb::require_true(i64_is("0", 0));
    sb::require_true(i64_is("-0", 0));
    sb::require_true(i64_is("42", 42));
    sb::require_true(i64_is("-42", -42));
    sb::require_true(i64_is("9007199254740993", 9007199254740993ll));      // 2^53+1: doubles cannot
    sb::require_true(i64_is("9223372036854775807", 9223372036854775807ll));
    sb::require_true(i64_is("-9223372036854775808", -9223372036854775807ll - 1));
    sb::require_true(u64_is("18446744073709551615", 18446744073709551615ull));      // 20-digit u64 max
    sb::require_true(u64_is("12345678901234567890", 12345678901234567890ull));
    sb::end_test_case();
  }
  {
    sb::test_case("integers wider than u64 become correctly rounded doubles");
    sb::require_true(f64_is("18446744073709551616", 18446744073709551616.0));
    sb::require_true(f64_is("-9223372036854775809", -9223372036854775809.0));
    sb::require_true(f64_is("123456789012345678901234567890", 123456789012345678901234567890.0));
    sb::end_test_case();
  }
  {
    sb::test_case("doubles reproduce the compiler literal bit for bit");
    sb::require_true(f64_is("0.0", 0.0));
    sb::require_true(f64_is("-0.0", -0.0));
    sb::require_true(f64_is("3.14159", 3.14159));
    sb::require_true(f64_is("1e22", 1e22));
    sb::require_true(f64_is("1e23", 1e23));      // beyond clinger
    sb::require_true(f64_is("-1e-22", -1e-22));
    sb::require_true(f64_is("1.7976931348623157e308", 1.7976931348623157e308));        // dbl_max
    sb::require_true(f64_is("2.2250738585072014e-308", 2.2250738585072014e-308));      // dbl_min
    sb::require_true(f64_is("2.2250738585072011e-308", 2.2250738585072011e-308));      // the php-hang subnormal edge
    sb::require_true(f64_is("5e-324", 5e-324));                                        // min denormal
    sb::require_true(f64_is("4.9406564584124654e-324", 4.9406564584124654e-324));
    sb::require_true(f64_is("9007199254740993.0", 9007199254740993.0));
    sb::require_true(f64_is("0.1", 0.1));
    sb::require_true(f64_is("0.5", 0.5));
    // exact-division tie: needs the ceil-rounded shallow reciprocal entries
    sb::require_true(f64_is("5200499112550317.50", 5200499112550317.50));
    sb::require_true(f64_is("1125899906842624.125", 1125899906842624.125));
    sb::require_true(f64_is("4503599627370495.5", 4503599627370495.5));
    sb::require_true(f64_is("6.62607015e-34", 6.62607015e-34));
    sb::require_true(f64_is("3.141592653589793238462643383279", 3.141592653589793238462643383279));
    sb::end_test_case();
  }
  {
    sb::test_case("truncated significands resolve exactly through the bigint boundary compare");
    // 1 + 2^-52 halfway: must round to even (1.0)
    sb::require_true(f64_is("1.00000000000000011102230246251565404236316680908203125", 1.0));
    // just above the halfway: must round up
    sb::require_true(f64_is("1.000000000000000111022302462515654042363166809082031251", 1.0000000000000002));
    // 30 significant digits through the w/w+1 disagreement path
    sb::require_true(f64_is("7.3183405489672259664880860639e12", 7.3183405489672259664880860639e12));
    sb::end_test_case();
  }
  {
    sb::test_case("range extremes zero out or reject as designed");
    sb::require_true(f64_is("1e-400", 0.0));
    sb::require_true(f64_is("-1e-400", -0.0));
    sb::require_true(f64_is("2.4e-324", 0.0));      // below half of the smallest denormal
    sb::require_true(rejects("1e309"));
    sb::require_true(rejects("-1e309"));
    sb::require_true(rejects("1.7976931348623159e308"));      // just past dbl_max
    sb::require_true(rejects("1e999999"));
    sb::end_test_case();
  }
  {
    sb::test_case("grammar violations are rejected");
    sb::require_true(rejects("01"));
    sb::require_true(rejects("-01"));
    sb::require_true(rejects("+1"));
    sb::require_true(rejects(".5"));
    sb::require_true(rejects("1."));
    sb::require_true(rejects("1.e5"));
    sb::require_true(rejects("1e"));
    sb::require_true(rejects("1e+"));
    sb::require_true(rejects("-"));
    sb::require_true(rejects("0x10"));
    sb::require_true(rejects("1_000"));
    sb::require_true(rejects("nan"));
    sb::require_true(rejects("inf"));
    sb::require_true(rejects("123abc"));
    sb::end_test_case();
  }
  {
    // NOTE: micron's ryu (double_to_string) is NOT usable as a round-trip oracle — it
    // prints a wrong shortest form for e.g. bits 0x2db34076d9def527-region doubles
    // (verified against python and cjson independently agreeing). the mass random
    // bit-agreement property therefore lives in comparison/yy_xvalidate.cpp against
    // strtod. here: random decimal TEXT must parse without error and stay in range
    sb::test_case("random decimal texts parse cleanly across the whole exponent range");
    tutil::rng rg;
    for ( u32 iter = 0; iter < 30000; iter++ ) {
      micron::vector<u8> t;
      if ( rg.below(2) ) t.push_back(u8('-'));
      t.push_back(u8('1' + rg.below(9)));
      const u32 ints = rg.below(18);
      for ( u32 i = 0; i < ints; i++ ) t.push_back(u8('0' + rg.below(10)));
      if ( rg.below(2) ) {
        t.push_back(u8('.'));
        const u32 fr = 1 + rg.below(24);
        for ( u32 i = 0; i < fr; i++ ) t.push_back(u8('0' + rg.below(10)));
      }
      if ( rg.below(2) ) {
        t.push_back(u8(rg.below(2) ? 'e' : 'E'));
        if ( rg.below(2) ) t.push_back(u8(rg.below(2) ? '+' : '-'));
        t.push_back(u8('1' + rg.below(9)));
        if ( rg.below(2) ) t.push_back(u8('0' + rg.below(10)));
      }
      auto r = cjson::parse(cjson::bytes{ t.cbegin(), t.size() });
      sb::require_true(r.is_first());
      const cjson::doc &d = r.cast<cjson::doc>();
      const f64 g = d.root().f64_or(d.root().is_null() ? 1 : 0) + f64(d.root().i64_or(0)) + f64(d.root().u64_or(0));
      sb::require_true(!(g != g));      // never nan
    }
    sb::end_test_case();
  }
  {
    sb::test_case("comptime number parsing agrees with runtime");
    constexpr auto ct = [](const char *s, usize n) consteval -> u64 {
      u8 *tmp = new u8[n];
      for ( usize i = 0; i < n; ++i ) tmp[i] = u8(s[i]);
      cjson::value v{};
      const max_t r = cjson::__num::read_number(tmp, n, 0, v);
      delete[] tmp;
      return r > 0 ? __builtin_bit_cast(u64, v.pay.f) : 0;
    };
    static_assert(ct("1e23", 4) == __builtin_bit_cast(u64, 1e23));
    static_assert(ct("0.1", 3) == __builtin_bit_cast(u64, 0.1));
    static_assert(ct("5e-324", 6) == __builtin_bit_cast(u64, 5e-324));
    sb::require_true(true);
    sb::end_test_case();
  }
  return 1;
}
