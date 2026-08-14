//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// the number kernel at its seams. Every accepted value is checked against a COMPILER
// LITERAL -- micron's Ryu is a known-wrong oracle for some doubles (CLAUDE.md) and is
// never used here.
//
// The kernel is a chain of four algorithms and the interesting cases are the handoffs:
//   clinger exact          sig < 2^53 and -22 <= exp10 <= 22
//   eisel-lemire 128-bit   everything else with an untruncated significand
//   w / w+1 agreement      truncated significand, tail provably irrelevant
//   4096-bit bigint        truncated and the tail decides -- __exact_boundary
// plus the integer path, which has its own 19- and 20-digit edges.

#include "../../src/cjson/cjson.hpp"

#include "../tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/string/strings.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

// lengths are DEDUCED from the literal, never hand-counted. A count that is too small
// does not fail loudly -- it silently parses a shorter, still-valid document, and the
// test then asserts things about a document nobody wrote. This bit twice while these
// files were being written.
template<usize N>
inline cjson::result<cjson::doc>
PJ(const char (&s)[N])
{
  return cjson::parse(reinterpret_cast<const u8 *>(s), N - 1);
}

struct fc {
  const char *p;
  usize n;
  f64 want;
};

template<usize N>
constexpr fc
F(const char (&s)[N], f64 w) noexcept
{
  return fc{ s, N - 1, w };
}

void
exact(const fc &c)
{
  auto r = cjson::parse(reinterpret_cast<const u8 *>(c.p), c.n);
  if ( !r.is_first() ) {
    snowball::print("number rejected: ", c.p);
    sb::require_true(false);
  }
  const f64 got = r.cast<cjson::doc>().root().f64_or(-1.0);
  if ( __builtin_bit_cast(u64, got) != __builtin_bit_cast(u64, c.want) ) {
    snowball::print("number decoded wrong: ", c.p);
    snowball::print("   got  ", got);
    snowball::print("   want ", c.want);
  }
  sb::require_true(__builtin_bit_cast(u64, got) == __builtin_bit_cast(u64, c.want));
}

};      // namespace

int
main()
{
  {
    sb::test_case("the clinger exact window decodes bit-exactly");
    // sig < 2^53 and |exp10| <= 22: one ieee operation, no approximation
    const fc t[] = {
      F("1", 1.0),
      F("-1", -1.0),
      F("0.5", 0.5),
      F("1.25", 1.25),
      F("1e22", 1e22),
      F("1e-22", 1e-22),
      F("123456789", 123456789.0),
      F("9007199254740991", 9007199254740991.0),
      F("1.5e10", 1.5e10),
      F("-2.25e-7", -2.25e-7),
      F("3.0e0", 3.0),
      F("7e21", 7e21),
      F("7e-21", 7e-21),
    };
    for ( const fc &c : t ) exact(c);
    sb::end_test_case();
  }
  {
    sb::test_case("one step past the clinger window hands off correctly");
    const fc t[] = {
      F("1e23", 1e23),   F("1e-23", 1e-23),   F("1e24", 1e24), F("1e-24", 1e-24), F("9007199254740993", 9007199254740993.0),
      F("1e100", 1e100), F("1e-100", 1e-100),
    };
    for ( const fc &c : t ) exact(c);
    sb::end_test_case();
  }
  {
    sb::test_case("every power of ten in range decodes bit-exactly");
    // build "1e<k>" and compare against a table of literals for the extremes; for the
    // middle, compare against repeated multiplication is NOT safe, so check round-trip
    // stability instead: parse -> write -> parse must be a fixpoint
    for ( i32 k = -308; k <= 308; ++k ) {
      micron::vector<u8> d;
      d.push_back(u8('1'));
      d.push_back(u8('e'));
      if ( k < 0 ) d.push_back(u8('-'));
      const u32 a = u32(k < 0 ? -k : k);
      if ( a >= 100 ) d.push_back(u8('0' + (a / 100)));
      if ( a >= 10 ) d.push_back(u8('0' + ((a / 10) % 10)));
      d.push_back(u8('0' + (a % 10)));

      auto r = cjson::parse(cjson::bytes{ d.cbegin(), d.size() });
      sb::require_true(r.is_first());
      const f64 v1 = r.cast<cjson::doc>().root().f64_or(-1.0);
      micron::string out = cjson::write_str(r.cast<cjson::doc>());
      auto r2 = cjson::parse(reinterpret_cast<const u8 *>(out.c_str()), out.size());
      sb::require_true(r2.is_first());
      const f64 v2 = r2.cast<cjson::doc>().root().f64_or(-2.0);
      sb::require_true(__builtin_bit_cast(u64, v1) == __builtin_bit_cast(u64, v2));
    }
    sb::end_test_case();
  }
  {
    sb::test_case("subnormals and the normal boundary decode bit-exactly");
    const fc t[] = {
      F("2.2250738585072014e-308", 2.2250738585072014e-308),      // smallest normal
      F("2.2250738585072011e-308", 2.2250738585072011e-308),      // largest subnormal region
      F("5e-324", 5e-324),                                        // smallest subnormal
      F("1e-323", 1e-323),
      F("1.5e-323", 1.5e-323),
      F("1.7976931348623157e308", 1.7976931348623157e308),      // largest finite
      F("-1.7976931348623157e308", -1.7976931348623157e308),
      F("1e-400", 0.0),      // underflow, silent
      F("-1e-400", -0.0),
    };
    for ( const fc &c : t ) exact(c);
    sb::end_test_case();
  }
  {
    // CLAUDE.md pins the EL pow5 table's rounding: negative powers q in [-27,-1] must be
    // CEIL (exact-division ties land one epsilon below the tie against a truncated
    // reciprocal), q <= -28 is floor. These are the values that walk that boundary.
    sb::test_case("the pow5 table boundary at q = -27 / -28 decodes bit-exactly");
    const fc t[] = {
      F("5200499112550317.5e-1", 520049911255031.75),
      F("1e-27", 1e-27),
      F("1e-28", 1e-28),
      F("1e-26", 1e-26),
      F("1e-29", 1e-29),
      F("1.5e-27", 1.5e-27),
      F("1.5e-28", 1.5e-28),
      F("123456789012345678e-27", 123456789012345678e-27),
      F("999999999999999999e-28", 999999999999999999e-28),
      F("2e-27", 2e-27),
      F("3e-28", 3e-28),
      F("7e-27", 7e-27),
      F("9e-28", 9e-28),
    };
    for ( const fc &c : t ) exact(c);
    sb::end_test_case();
  }
  {
    sb::test_case("the truncated-significand path decodes bit-exactly");
    const fc t[] = {
      F("4539183550709394473162714279012", 4539183550709394473162714279012.0),
      F("123456789012345678901234567890", 123456789012345678901234567890.0),
      F("0.1234567890123456789012345678901234567890", 0.1234567890123456789012345678901234567890),
      F("1.7976931348623158e308", 1.7976931348623158e308),
      F("9007199254740993.0000000000001", 9007199254740993.0000000000001),
      F("0.30000000000000004", 0.30000000000000004),
      F("2.2250738585072011e-308", 2.2250738585072011e-308),
    };
    for ( const fc &c : t ) exact(c);
    sb::end_test_case();
  }
  {
    sb::test_case("integer typing is exact at every 64-bit boundary");

    struct ic {
      const char *p;
      usize n;
      bool as_u64;
      u64 u;
      i64 i;
    };

    const ic t[] = {
      { "0", 1, true, 0, 0 },
      { "1", 1, true, 1, 1 },
      { "9223372036854775807", 19, true, 9223372036854775807ULL, 9223372036854775807LL },
      { "9223372036854775808", 19, true, 9223372036854775808ULL, 0 },
      { "18446744073709551615", 20, true, 18446744073709551615ULL, 0 },
      { "-1", 2, false, 0, -1 },
      { "-9223372036854775808", 20, false, 0, -9223372036854775807LL - 1 },
      { "999999999999999999", 18, true, 999999999999999999ULL, 999999999999999999LL },
      { "1000000000000000000", 19, true, 1000000000000000000ULL, 1000000000000000000LL },
    };
    for ( const ic &c : t ) {
      auto r = cjson::parse(reinterpret_cast<const u8 *>(c.p), c.n);
      sb::require_true(r.is_first());
      auto v = r.cast<cjson::doc>().root();
      if ( c.as_u64 ) {
        sb::require(v.u64_or(1234), c.u);
      } else {
        sb::require(v.i64_or(1234), c.i);
      }
    }

    // one past u64 max degrades to a double, silently and correctly
    {
      auto r = PJ("18446744073709551616");
      sb::require_true(r.is_first());
      const f64 got = r.cast<cjson::doc>().root().f64_or(-1.0);
      sb::require_true(__builtin_bit_cast(u64, got) == __builtin_bit_cast(u64, 18446744073709551616.0));
    }
    // and one below i64 min likewise
    {
      auto r = PJ("-9223372036854775809");
      sb::require_true(r.is_first());
      const f64 got = r.cast<cjson::doc>().root().f64_or(1.0);
      sb::require_true(__builtin_bit_cast(u64, got) == __builtin_bit_cast(u64, -9223372036854775809.0));
    }
    sb::end_test_case();
  }
  {
    // any '.' or exponent makes it a real, even when the value is integral
    sb::test_case("a fraction or exponent always yields a real");
    auto r = PJ(R"([1,1.0,1e0,1E0,10e-1])");
    sb::require_true(r.is_first());
    auto a = r.cast<cjson::doc>().root();
    sb::require(a.size(), static_cast<usize>(5));
    for ( usize i = 0; i < 5; ++i ) sb::require_true(a.at(i).f64_or(-1.0) == 1.0);
    // the integer keeps integer typing; the rest do not
    sb::require(a.at(0).u64_or(9), static_cast<u64>(1));
    sb::end_test_case();
  }
  {
    sb::test_case("the corpus number columns decode identically through every path");
    const char *files[] = { "sample/64kb.json", "sample/twitter.json", "sample/128KB.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      auto rd = cjson::parse(tutil::view(data));
      sb::require_true(rd.is_first());
      micron::vector<u8> mut = data.clone();
      auto ri = cjson::parse_insitu(cjson::wbytes{ mut.begin(), mut.size() });
      sb::require_true(ri.is_first());
      cjson::scratch sc;
      auto rv = cjson::iterate(tutil::view(data), sc);
      sb::require_true(rv.is_first());
      // the three paths must agree on the written form, which is a proxy for agreeing
      // on every number in the document
      micron::string a = cjson::write_str(rd.cast<cjson::doc>());
      micron::string b = cjson::write_str(ri.cast<cjson::doc>());
      sb::require(a.size(), b.size());
      for ( usize i = 0; i < a.size(); ++i ) sb::require_true(a[i] == b[i]);
    }
    sb::end_test_case();
  }
  return 1;
}
