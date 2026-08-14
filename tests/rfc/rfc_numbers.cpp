//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// rfc 8259 s6, the number ABNF, exactly:
//
//   number = [ minus ] int [ frac ] [ exp ]
//   int    = zero / ( digit1-9 *DIGIT )      frac = decimal-point 1*DIGIT
//   exp    = e [ minus / plus ] 1*DIGIT      zero = %x30
//
// "Leading zeros are not allowed." "Numeric values that cannot be represented in the
// grammar below (such as Infinity and NaN) are not permitted."
//
// Accepted values are checked BIT-EXACT against compiler literals. micron's Ryu is a
// known-wrong oracle for some doubles (see CLAUDE.md) and is never used here.
//
// Also carries F3: s6 lets an implementation "set limits on the range and precision of
// numbers accepted", and cjson exercises that right in `parse` but not in `validate` /
// `minify` / `numbers_as_raw`. Both sides are pinned side by side so the documented
// divergence cannot drift in either direction.

#include "rfc_cases.hpp"

#include <snowball/snowball.hpp>

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

using rfc::K;
using rfc::KD;
using rfc::verdict;

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// the grammar, accept side

const rfc::kase k_accept[] = {
  K("0", verdict::accept, "s6 int = zero"),
  K("-0", verdict::accept, "s6 minus zero is in the grammar"),
  K("1", verdict::accept, "s6 int = digit1-9"),
  K("-1", verdict::accept, "s6 minus int"),
  K("123", verdict::accept, "s6 int = digit1-9 *DIGIT"),
  K("-123", verdict::accept, "s6 minus int"),
  K("1.0", verdict::accept, "s6 frac = decimal-point 1*DIGIT"),
  K("0.0", verdict::accept, "s6 zero frac"),
  K("-0.0", verdict::accept, "s6 minus zero frac"),
  K("1.5", verdict::accept, "s6 frac"),
  K("0.000001", verdict::accept, "s6 long frac"),
  K("1e0", verdict::accept, "s6 exp, lowercase e, no sign"),
  K("1E0", verdict::accept, "s6 exp, uppercase E"),
  K("1e+2", verdict::accept, "s6 exp with plus"),
  K("1e-2", verdict::accept, "s6 exp with minus"),
  K("1E+2", verdict::accept, "s6 uppercase E with plus"),
  K("1E-2", verdict::accept, "s6 uppercase E with minus"),
  K("1.5e3", verdict::accept, "s6 frac and exp together"),
  K("-1.5e-10", verdict::accept, "s6 all four parts"),
  K("0e0", verdict::accept, "s6 zero with exponent"),
  K("-0e0", verdict::accept, "s6 minus zero with exponent"),
  K("1e00", verdict::accept, "s6 exp digits may have leading zeros; only `int` may not"),
  K("1e007", verdict::accept, "s6 leading zeros are banned in int, not in exp"),
  K("0.0000000000001", verdict::accept, "s6 long fraction"),
  K("123456789012345678901234567890", verdict::accept, "s6 int has no digit limit in the grammar"),
  K("[0,-0,1,-1,1.0,1e1]", verdict::accept, "s6 numbers as array elements"),
  K("{\"n\":-1.5e-10}", verdict::accept, "s6 numbers as member values"),
};

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// the grammar, reject side

const rfc::kase k_reject[] = {
  // leading zeros
  K("01", verdict::reject, "s6 leading zeros are not allowed"),
  K("-01", verdict::reject, "s6 leading zeros are not allowed"),
  K("00", verdict::reject, "s6 leading zeros are not allowed"),
  K("012", verdict::reject, "s6 leading zeros are not allowed"),
  K("0123", verdict::reject, "s6 leading zeros are not allowed"),
  K("-00.1", verdict::reject, "s6 leading zeros are not allowed"),
  K("[01]", verdict::reject, "s6 leading zero inside an array"),

  // the sign
  K("+1", verdict::reject, "s6 number = [ minus ] int -- there is no leading plus"),
  K("+0", verdict::reject, "s6 no leading plus"),
  K("+1.5", verdict::reject, "s6 no leading plus"),
  K("-", verdict::reject, "s6 minus must be followed by int"),
  K("--1", verdict::reject, "s6 one minus"),
  K("- 1", verdict::reject, "s6 minus binds directly to int"),

  // the fraction
  K(".5", verdict::reject, "s6 int is not optional"),
  K("-.5", verdict::reject, "s6 int is not optional"),
  K("1.", verdict::reject, "s6 frac = decimal-point 1*DIGIT"),
  K("0.", verdict::reject, "s6 frac needs at least one digit"),
  K("-1.", verdict::reject, "s6 frac needs at least one digit"),
  K("1.e5", verdict::reject, "s6 frac needs at least one digit before exp"),
  K("1..2", verdict::reject, "s6 one decimal-point"),
  K("1.2.3", verdict::reject, "s6 one decimal-point"),

  // the exponent
  K("1e", verdict::reject, "s6 exp = e [ minus / plus ] 1*DIGIT"),
  K("1E", verdict::reject, "s6 exp needs at least one digit"),
  K("1e+", verdict::reject, "s6 exp needs digits after the sign"),
  K("1e-", verdict::reject, "s6 exp needs digits after the sign"),
  K("1E-", verdict::reject, "s6 exp needs digits after the sign"),
  K("1e1e1", verdict::reject, "s6 one exp part"),
  K("1e+-1", verdict::reject, "s6 one sign in exp"),
  K("1e 1", verdict::reject, "s6 exp binds directly"),
  K("0e", verdict::reject, "s6 exp needs digits"),

  // not base ten, not this grammar
  K("0x1F", verdict::reject, "s6 a number is represented in base 10"),
  K("0X1F", verdict::reject, "s6 base 10 only"),
  K("0b101", verdict::reject, "s6 base 10 only"),
  K("0o17", verdict::reject, "s6 base 10 only"),
  K("1_000", verdict::reject, "s6 no digit separators"),
  K("1,0", verdict::reject, "s2 a value-separator is not part of a number"),
  K("1'000", verdict::reject, "s6 no digit separators"),

  // s6: "Numeric values that cannot be represented in the grammar below (such as
  // Infinity and NaN) are not permitted."
  K("Infinity", verdict::reject, "s6 Infinity is not permitted"),
  K("-Infinity", verdict::reject, "s6 -Infinity is not permitted"),
  K("infinity", verdict::reject, "s6 Infinity is not permitted"),
  K("inf", verdict::reject, "s6 inf is not permitted"),
  K("NaN", verdict::reject, "s6 NaN is not permitted"),
  K("nan", verdict::reject, "s6 NaN is not permitted"),
  K("-NaN", verdict::reject, "s6 NaN is not permitted"),
  K("[Infinity]", verdict::reject, "s6 Infinity inside an array"),
  K("[NaN]", verdict::reject, "s6 NaN inside an array"),
  K("{\"a\":NaN}", verdict::reject, "s6 NaN as a member value"),

  // run-ons
  K("1abc", verdict::reject, "s6 a number ends where the grammar ends"),
  K("1e5x", verdict::reject, "s6 a number ends where the grammar ends"),
  K("123f", verdict::reject, "s6 a number ends where the grammar ends"),
  K("1\"", verdict::reject, "s6 a quotation mark does not terminate a number"),
};

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// bit-exact values. the RHS is a compiler literal, compared as bits so that signed zero
// and the -0.0 / 0.0 distinction survive.

struct fcase {
  const char *p;
  usize n;
  f64 want;
  const char *why;
};

template<usize N>
constexpr fcase
F(const char (&s)[N], f64 w, const char *why) noexcept
{
  return fcase{ s, N - 1, w, why };
}

const fcase k_exact[] = {
  F("0.0", 0.0, "zero"),
  F("-0.0", -0.0, "negative zero must stay negative"),
  F("1.0", 1.0, "one"),
  F("-1.0", -1.0, "minus one"),
  F("0.5", 0.5, "exactly representable"),
  F("1.5", 1.5, "exactly representable"),
  F("0.1", 0.1, "the classic inexact decimal"),
  F("0.2", 0.2, "inexact"),
  F("0.3", 0.3, "inexact"),
  F("1e0", 1e0, "exp zero"),
  F("1e1", 1e1, "clinger range"),
  F("1e22", 1e22, "the top of the clinger exact range"),
  F("1e23", 1e23, "one past the clinger range -- eisel-lemire"),
  F("1e-22", 1e-22, "the bottom of the clinger exact range"),
  F("1e-23", 1e-23, "one past the clinger range"),
  F("2.2250738585072014e-308", 2.2250738585072014e-308, "smallest normal double"),
  F("5e-324", 5e-324, "smallest subnormal double"),
  F("1.7976931348623157e308", 1.7976931348623157e308, "largest finite double"),
  F("-1.7976931348623157e308", -1.7976931348623157e308, "most negative finite double"),
  F("9007199254740991.0", 9007199254740991.0, "2^53 - 1, the s6 interoperability window"),
  F("9007199254740993.0", 9007199254740993.0, "2^53 + 1, not representable -- must round"),
  F("123456789012345678901234567890.0", 123456789012345678901234567890.0, "30 digits, truncated significand"),
  F("4539183550709394473162714279012", 4539183550709394473162714279012.0, "F4b: the bigint-halfway repro"),
  F("1e-400", 0.0, "s6 underflow is silent: rounds to +0.0, no error"),
  F("-1e-400", -0.0, "underflow keeps the sign"),
  F("2.4e-324", 0.0, "below the smallest subnormal"),
};

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// F3: `parse` limits range, the grammar-only entry points do not.
//
// s6: "This specification allows implementations to set limits on the range and
// precision of numbers accepted." So each side conforms; they simply disagree, and
// is_valid() is therefore NOT a sound pre-flight for parse().

struct rangecase {
  const char *p;
  usize n;
  bool grammar_ok;      // validate / minify / numbers_as_raw
  bool parse_ok;        // the converting path
  const char *why;
};

template<usize N>
constexpr rangecase
R(const char (&s)[N], bool g, bool pa, const char *why) noexcept
{
  return rangecase{ s, N - 1, g, pa, why };
}

const rangecase k_range[] = {
  R("1e309", true, false, "F3 beyond dbl_max: grammar-legal, parse rejects"),
  R("-1e309", true, false, "F3 beyond -dbl_max"),
  R("1e400", true, false, "F3 the rfc's own 1E400 interoperability example"),
  R("1E400", true, false, "F3 uppercase form of the same"),
  R("1e999999", true, false, "F3 absurd exponent"),
  R("[1e309]", true, false, "F3 inside an array"),
  R("{\"a\":1e309}", true, false, "F3 as a member value"),
  R("1e308", true, true, "F3 inside the range: both accept"),
  R("1.7976931348623157e308", true, true, "F3 largest finite: both accept"),
  R("1e-400", true, true, "F3 underflow is NOT a range error: both accept"),
  R("123456789012345678901234567890", true, true, "F3 wide integers degrade to double, not an error"),
};

void
check_exact(const fcase &c)
{
  auto r = cjson::parse(reinterpret_cast<const u8 *>(c.p), c.n);
  if ( !r.is_first() ) {
    snowball::print("rfc s6 exact FAILED (rejected): ", c.why);
    sb::require_true(false);
  }
  const f64 got = r.cast<cjson::doc>().root().f64_or(-1.0);
  if ( __builtin_bit_cast(u64, got) != __builtin_bit_cast(u64, c.want) ) {
    snowball::print("rfc s6 exact FAILED (bits): ", c.why);
    snowball::print("   got  ", got);
    snowball::print("   want ", c.want);
  }
  sb::require_true(__builtin_bit_cast(u64, got) == __builtin_bit_cast(u64, c.want));
}

void
check_range(const rangecase &c)
{
  const u8 *p = reinterpret_cast<const u8 *>(c.p);

  const bool ve = (cjson::validate(p, c.n) == cjson::error::ok);
  if ( ve != c.grammar_ok ) snowball::print("F3 FAILED (validate): ", c.why);
  sb::require_true(ve == c.grammar_ok);

  auto r = cjson::parse(p, c.n);
  if ( r.is_first() != c.parse_ok ) snowball::print("F3 FAILED (parse): ", c.why);
  sb::require_true(r.is_first() == c.parse_ok);

  // numbers_as_raw sits on the grammar-only side: no conversion, so no range check
  auto rr = cjson::parse(p, c.n, cjson::opts{ .numbers_as_raw = true });
  if ( rr.is_first() != c.grammar_ok ) snowball::print("F3 FAILED (numbers_as_raw): ", c.why);
  sb::require_true(rr.is_first() == c.grammar_ok);

  // minify walks the same grammar fsm as validate
  micron::vector<u8> out;
  out.reserve(cjson::minify_bound(c.n) + cjson::padding);
  const max_t m = cjson::minify(cjson::bytes{ p, c.n }, cjson::wbytes{ out.begin(), cjson::minify_bound(c.n) });
  if ( (m >= 0) != c.grammar_ok ) snowball::print("F3 FAILED (minify): ", c.why);
  sb::require_true((m >= 0) == c.grammar_ok);
}

};      // namespace

int
main()
{
  {
    sb::test_case("s6: every form the number ABNF admits");
    rfc::expect_all(k_accept);
    sb::end_test_case();
  }
  {
    sb::test_case("s6: every form the number ABNF excludes, Infinity and NaN included");
    rfc::expect_all(k_reject);
    sb::end_test_case();
  }
  {
    sb::test_case("s6: accepted numbers decode bit-exactly against compiler literals");
    for ( const fcase &c : k_exact ) check_exact(c);
    sb::end_test_case();
  }
  {
    sb::test_case("s6: integer typing at the u64 and i64 boundaries");

    struct icase {
      const char *p;
      i64 want;
    };

    const icase signed_cases[] = {
      { "0", 0 },
      { "-0", 0 },
      { "1", 1 },
      { "-1", -1 },
      { "9223372036854775807", 9223372036854775807LL },
      { "-9223372036854775808", -9223372036854775807LL - 1 },
      { "9007199254740991", 9007199254740991LL },
      { "-9007199254740991", -9007199254740991LL },
    };
    for ( const icase &c : signed_cases ) {
      usize n = 0;
      while ( c.p[n] ) ++n;
      auto r = cjson::parse(reinterpret_cast<const u8 *>(c.p), n);
      sb::require_true(r.is_first());
      sb::require(r.cast<cjson::doc>().root().i64_or(-999), c.want);
    }

    // u64 above i64 max stays exact
    {
      const char *p = "18446744073709551615";
      auto r = cjson::parse(reinterpret_cast<const u8 *>(p), 20);
      sb::require_true(r.is_first());
      sb::require(r.cast<cjson::doc>().root().u64_or(0), static_cast<u64>(18446744073709551615ULL));
    }
    // one past u64 max degrades to a correctly-rounded double, NOT an error (s6 permits)
    {
      const char *p = "18446744073709551616";
      auto r = cjson::parse(reinterpret_cast<const u8 *>(p), 20);
      sb::require_true(r.is_first());
      const f64 got = r.cast<cjson::doc>().root().f64_or(-1.0);
      sb::require_true(__builtin_bit_cast(u64, got) == __builtin_bit_cast(u64, 18446744073709551616.0));
    }
    sb::end_test_case();
  }
  {
    sb::test_case("s6: -0 is an integer zero, -0.0 is a negative real zero");
    auto ri = PJ("-0");
    sb::require_true(ri.is_first());
    sb::require(ri.cast<cjson::doc>().root().i64_or(-1), static_cast<i64>(0));

    auto rf = PJ("-0.0");
    sb::require_true(rf.is_first());
    const f64 z = rf.cast<cjson::doc>().root().f64_or(1.0);
    sb::require_true(__builtin_bit_cast(u64, z) == __builtin_bit_cast(u64, -0.0));
    sb::end_test_case();
  }
  {
    sb::test_case("F3: validate accepts what parse refuses on range, in both directions");
    for ( const rangecase &c : k_range ) check_range(c);
    sb::end_test_case();
  }
  {
    // the contract this pins: is_valid() is a GRAMMAR check, not a promise that parse
    // will succeed. documented in COMPLIANCE.md and README.
    sb::test_case("F3: is_valid is not a sound pre-flight for parse");
    const char *p = "1e309";
    sb::require_true(cjson::is_valid(reinterpret_cast<const u8 *>(p), 5));
    sb::require_true(cjson::parse(reinterpret_cast<const u8 *>(p), 5).is_second());
    sb::require_true(cjson::parse(reinterpret_cast<const u8 *>(p), 5).cast<cjson::error>() == cjson::error::bad_number);
    sb::end_test_case();
  }
  {
    // s6 caps the significand cjson will chew on; beyond max_big_digits (768) a sticky
    // digit preserves ordering. The boundary must not be a cliff into rejection.
    //
    // NOTE the magnitude has to stay modest: a 1200-DIGIT INTEGER is ~1e1200 and parse
    // rejects it on range (F3), which says nothing about significand length. Keep the
    // value near 1 and put all the digits in the fraction.
    sb::test_case("s6: very long significands are accepted, not refused");
    for ( u32 nd : { 700u, 767u, 768u, 769u, 800u, 2400u } ) {
      micron::vector<u8> big;
      big.push_back(u8('1'));
      big.push_back(u8('.'));
      for ( u32 i = 0; i < nd; ++i ) big.push_back(u8('0' + (i % 7)));
      auto r = cjson::parse(cjson::bytes{ big.cbegin(), big.size() });
      if ( !r.is_first() ) snowball::print("long significand rejected at nd=", nd);
      sb::require_true(r.is_first());
      sb::require_true(cjson::validate(big.cbegin(), big.size()) == cjson::error::ok);
      // the value is 1.0123... regardless of how much tail we bury past max_big_digits
      const f64 got = r.cast<cjson::doc>().root().f64_or(-1.0);
      sb::require_true(got > 1.0 and got < 1.1);
    }

    // the same digits as a huge INTEGER are grammar-legal but out of range: that is F3,
    // not a significand limit, and the two must not be confused
    {
      micron::vector<u8> huge;
      huge.push_back(u8('1'));
      for ( u32 i = 0; i < 1200; ++i ) huge.push_back(u8('0' + (i % 10)));
      sb::require_true(cjson::validate(huge.cbegin(), huge.size()) == cjson::error::ok);
      auto r = cjson::parse(cjson::bytes{ huge.cbegin(), huge.size() });
      sb::require_true(r.is_second());
      sb::require_true(r.cast<cjson::error>() == cjson::error::bad_number);
    }
    sb::end_test_case();
  }
  {
    // every accepted number must survive a write/re-parse round trip bit-exactly; that is
    // s10 ("the resulting text MUST strictly conform") applied to the number writer
    sb::test_case("s10: accepted numbers round-trip through the writer bit-exactly");
    for ( const fcase &c : k_exact ) {
      auto r = cjson::parse(reinterpret_cast<const u8 *>(c.p), c.n);
      sb::require_true(r.is_first());
      micron::string out = cjson::write_str(r.cast<cjson::doc>());
      auto r2 = cjson::parse(reinterpret_cast<const u8 *>(out.c_str()), out.size());
      if ( !r2.is_first() ) {
        snowball::print("s10 round trip FAILED to re-parse: ", c.why);
        snowball::print("   wrote ", out.c_str());
      }
      sb::require_true(r2.is_first());
      const f64 a = r.cast<cjson::doc>().root().f64_or(-1.0);
      const f64 b = r2.cast<cjson::doc>().root().f64_or(-2.0);
      if ( __builtin_bit_cast(u64, a) != __builtin_bit_cast(u64, b) ) {
        snowball::print("s10 round trip lost bits: ", c.why);
        snowball::print("   wrote ", out.c_str());
      }
      sb::require_true(__builtin_bit_cast(u64, a) == __builtin_bit_cast(u64, b));
    }
    sb::end_test_case();
  }
  return 1;
}
