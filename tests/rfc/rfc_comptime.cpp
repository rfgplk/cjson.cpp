//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// Every rfc case, twice: once inside constant evaluation and once at runtime, with the
// two verdicts required to match.
//
// That makes this file two tests at once. Under `consteval` the machine paths behind
// `if !consteval` are unreachable, so the SCALAR twins run -- the lookup4 utf-8 decoder,
// the Kogge-Stone prefix xor, the byte-at-a-time classifier. At runtime the avx2 kernels
// run instead. The scalar twin is the oracle (CLAUDE.md rule 4), so any disagreement here
// is a broken simd kernel, and every rfc case doubles as a twin-identity case.
//
// Building this file IS most of the test: the static_asserts run the whole stage-1 sweep
// and grammar fsm at compile time. If it compiles, the comptime half already passed.
//
// NOTE cjson::validate(const char *, usize) carries an `if consteval` arm that copies
// through a transient new[]/delete[], because reinterpret_cast is not a constant
// expression. That is why the table holds `const char *` and never `const u8 *`.

#include "../../src/cjson/cjson.hpp"

#include <snowball/snowball.hpp>

#include <micron/types.hpp>

namespace
{

struct ct_kase {
  const char *p;
  usize n;
  bool accept;
  const char *why;
};

// length deduced, so embedded nuls survive
template<usize N>
constexpr ct_kase
C(const char (&s)[N], bool a, const char *why) noexcept
{
  return ct_kase{ s, N - 1, a, why };
}

constexpr ct_kase table[] = {
  // s2 -- JSON-text = ws value ws
  C("{}", true, "s2 object root"),
  C("[]", true, "s2 array root"),
  C(" \t\r\n{}\t\r\n ", true, "s2 ws on both sides"),
  C("", false, "s2 empty input"),
  C("   ", false, "s2 ws only"),
  C("{}{}", false, "s2 two roots"),
  C("{} x", false, "s2 trailing garbage"),

  // s2 -- the ws set is exactly four bytes
  C("\x0b{}", false, "s2 vertical tab is not ws"),
  C("\x0c{}", false, "s2 form feed is not ws"),
  C("\x1a{}", false, "s2 0x1a is not ws"),

  // s3 -- literal names, lowercase only
  C("null", true, "s3 null"),
  C("true", true, "s3 true"),
  C("false", true, "s3 false"),
  C("True", false, "s3 literals MUST be lowercase"),
  C("NULL", false, "s3 literals MUST be lowercase"),
  C("nul", false, "s3 truncated literal"),
  C("truex", false, "s3 literal run-on"),

  // s4 -- objects
  C("{\"a\":1}", true, "s4 one member"),
  C("{\"a\":1,\"b\":2}", true, "s4 two members"),
  C("{\"\":1}", true, "s4 empty name"),
  C("{,}", false, "s4 lone separator"),
  C("{\"a\":}", false, "s4 missing value"),
  C("{\"a\":1,}", false, "s4 trailing separator"),
  C("{a:1}", false, "s4 unquoted name"),
  C("{'a':1}", false, "s4 apostrophes do not quote"),
  C("{\"a\" 1}", false, "s4 missing name-separator"),

  // s5 -- arrays
  C("[1]", true, "s5 one element"),
  C("[1,2,3]", true, "s5 several"),
  C("[null,true,\"s\",1,{},[]]", true, "s5 mixed types"),
  C("[,]", false, "s5 lone separator"),
  C("[1,]", false, "s5 trailing separator"),
  C("[1,,2]", false, "s5 doubled separator"),
  C("[1 2]", false, "s5 missing separator"),
  C("[1}", false, "s5 mismatched closer"),

  // s6 -- numbers
  C("0", true, "s6 zero"),
  C("-0", true, "s6 minus zero"),
  C("1e0", true, "s6 exponent"),
  C("1E+2", true, "s6 uppercase exponent with sign"),
  C("-1.5e-10", true, "s6 all parts"),
  C("01", false, "s6 leading zero"),
  C("-01", false, "s6 leading zero"),
  C("+1", false, "s6 no leading plus"),
  C(".5", false, "s6 int is not optional"),
  C("1.", false, "s6 frac needs a digit"),
  C("1e", false, "s6 exp needs a digit"),
  C("1e+", false, "s6 exp needs a digit after the sign"),
  C("0x1F", false, "s6 base 10 only"),
  C("Infinity", false, "s6 Infinity is not permitted"),
  C("NaN", false, "s6 NaN is not permitted"),
  C("1abc", false, "s6 run-on"),

  // s7 -- strings and escapes
  C("\"\"", true, "s7 empty string"),
  C("\"abc\"", true, "s7 ordinary content"),
  C("\"\\\"\"", true, "s7 escaped quote"),
  C("\"\\\\\"", true, "s7 escaped reverse solidus"),
  C("\"\\/\"", true, "s7 escaped solidus"),
  C("\"\\b\\f\\n\\r\\t\"", true, "s7 the control escapes"),
  C("\"\\u0041\"", true, "s7 \\uXXXX"),
  C("\"\\u0000\"", true, "s7 escaped nul is legal"),
  C("\"\\x41\"", false, "s7 \\x is not an escape"),
  C("\"\\'\"", false, "s7 \\' is not an escape"),
  C("\"\\u12\"", false, "s7 \\u needs four hex digits"),
  C("\"\\u123g\"", false, "s7 g is not hex"),
  C("\"abc", false, "s7 unterminated"),
  C("\"", false, "s7 unterminated"),

  // s7/s8.2 -- surrogates
  C("\"\\uD834\\uDD1E\"", true, "s7 the G clef pair"),
  C("\"\\uD800\\uDC00\"", true, "s7 lowest pair"),
  C("\"\\uDBFF\\uDFFF\"", true, "s7 highest pair"),
  C("\"\\uDEAD\"", false, "s8.2 the spec's unpaired-surrogate example"),
  C("\"\\uD800\"", false, "s8.2 lone high half"),
  C("\"\\uDC00\"", false, "s8.2 lone low half"),
  C("\"\\uD800x\"", false, "s8.2 high half then an ordinary char"),
  C("\"\\uDC00\\uD800\"", false, "s8.2 reversed pair"),

  // s7 -- raw control characters
  C("\"\x01\"", false, "s7 raw 0x01 must be escaped"),
  C("\"\x1f\"", false, "s7 raw 0x1f must be escaped"),
  C("\"\x7f\"", true, "s7 0x7f del is ordinary content"),

  // s8.1 -- utf-8
  C("\"\xc3\xa9\"", true, "s8.1 u+00e9"),
  C("\"\xe2\x82\xac\"", true, "s8.1 u+20ac"),
  C("\"\xf0\x9d\x84\x9e\"", true, "s8.1 u+1d11e"),
  C("\"\xc0\x80\"", false, "s8.1 overlong"),
  C("\"\xe0\x80\x80\"", false, "s8.1 overlong"),
  C("\"\xed\xa0\x80\"", false, "s8.1 surrogate encoded in utf-8"),
  C("\"\xf4\x90\x80\x80\"", false, "s8.1 beyond u+10ffff"),
  C("\"\xff\"", false, "s8.1 0xff is never utf-8"),
  C("\"\x80\"", false, "s8.1 stray continuation"),
  C("\"\xe2\x82\"", false, "s8.1 truncated sequence"),

  // s8.1 -- the byte order mark (discretionary: s8.1 makes ignoring it a MAY)
  C("\xef\xbb\xbf{}", false, "s8.1 cjson rejects a leading bom"),

  // F1 -- the nul hole. THIS is the group that regressed the scalar twin as well as the
  // simd path, because the defect lived in a shared table rather than a kernel.
  C("1\0", false, "F1 nul after a number"),
  C("1\0garbage", false, "F1 nul then trailing bytes"),
  C("true\0junk", false, "F1 nul after a literal"),
  C("[1\0]", false, "F1 interior nul in an array"),
  C("{\"a\":1\0}", false, "F1 interior nul in an object"),
  C("{}\0", false, "F1 control: nul after end-object"),
  C("\"s\"\0", false, "F1 control: nul after a string"),
  C("\0{}", false, "F1 control: nul in value position"),
  C("\"\0\"", false, "s7 raw nul inside a string"),
  C("\"\\u0000\"", true, "s7 escaped nul inside a string"),
};

constexpr usize table_n = sizeof(table) / sizeof(table[0]);

// the whole table, inside constant evaluation. returns the number of disagreements so a
// failure names a count rather than just exploding.
consteval usize
ct_mismatches() noexcept
{
  usize bad = 0;
  for ( usize i = 0; i < table_n; ++i ) {
    const bool ok = (cjson::validate(table[i].p, table[i].n) == cjson::error::ok);
    if ( ok != table[i].accept ) ++bad;
  }
  return bad;
}

static_assert(ct_mismatches() == 0, "an rfc case disagrees with its expected verdict under constant evaluation");

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// exact error codes at compile time, in the style of tests/comptime.cpp

template<usize N>
consteval cjson::error
V(const char (&s)[N]) noexcept
{
  return cjson::validate(s, N - 1);
}

static_assert(V("") == cjson::error::empty_input);
static_assert(V("   ") == cjson::error::empty_input);
static_assert(V("{} x") == cjson::error::trailing_garbage);
static_assert(V("1 2") == cjson::error::trailing_garbage);
static_assert(V("[1,,2]") == cjson::error::bad_syntax);
static_assert(V("+1") == cjson::error::bad_syntax);
static_assert(V(".5") == cjson::error::bad_syntax);
static_assert(V("Infinity") == cjson::error::bad_syntax);
static_assert(V("01") == cjson::error::bad_number);
static_assert(V("1e") == cjson::error::bad_number);
static_assert(V("1.") == cjson::error::bad_number);
static_assert(V("\"abc") == cjson::error::bad_string);
static_assert(V("\"\x01\"") == cjson::error::bad_string);
static_assert(V("\"\\x41\"") == cjson::error::bad_escape);
static_assert(V("\"\\u12\"") == cjson::error::bad_escape);
static_assert(V("\"\\uD800\"") == cjson::error::bad_surrogate);
static_assert(V("\"\\uDEAD\"") == cjson::error::bad_surrogate);
static_assert(V("\"\xc0\x80\"") == cjson::error::bad_utf8);
static_assert(V("\"\xed\xa0\x80\"") == cjson::error::bad_utf8);

// F1 at compile time, with its diagnosis. A nul glued to a number breaks the NUMBER, so
// the code is bad_number; glued to a literal it breaks the LITERAL, so it is bad_syntax.
static_assert(V("1\0") == cjson::error::bad_number);
static_assert(V("1\0garbage") == cjson::error::bad_number);
static_assert(V("[1\0]") == cjson::error::bad_number);
static_assert(V("true\0junk") == cjson::error::bad_syntax);
static_assert(V("null\0") == cjson::error::bad_syntax);
static_assert(V("{}\0") == cjson::error::trailing_garbage);
static_assert(V("\0{}") == cjson::error::bad_syntax);

// depth, with transient comptime allocation -- an unbalanced new[] is a hard error inside
// constant evaluation, so this also proves the comptime scratch is balanced
consteval cjson::error
deep(u32 n) noexcept
{
  u8 *buf = new u8[2 * n];
  for ( u32 i = 0; i < n; ++i ) buf[i] = u8('[');
  for ( u32 i = 0; i < n; ++i ) buf[n + i] = u8(']');
  const cjson::error e = cjson::validate(buf, 2 * n);
  delete[] buf;
  return e;
}

static_assert(deep(64) == cjson::error::ok);
static_assert(deep(cjson::depth_limit + 1) == cjson::error::ok);
static_assert(deep(cjson::depth_limit + 2) == cjson::error::depth_exceeded);

};      // namespace

int
main()
{
  {
    // the runtime half of the twin seam: the same table, the avx2 kernels, the same
    // verdicts. the comptime half already passed by virtue of compiling.
    sb::test_case("every rfc case agrees between the scalar twin and the simd kernels");
    for ( usize i = 0; i < table_n; ++i ) {
      const cjson::error e = cjson::validate(table[i].p, table[i].n);
      const bool ok = (e == cjson::error::ok);
      if ( ok != table[i].accept ) {
        snowball::print("twin FAILED at runtime: ", table[i].why);
        snowball::print("   got ", cjson::error_name(e), ", wanted ", table[i].accept ? "accept" : "reject");
      }
      sb::require_true(ok == table[i].accept);
    }
    sb::end_test_case();
  }
  {
    // not just the verdict -- the exact error code must match across the seam too, or a
    // simd kernel is diagnosing the same byte differently from the oracle
    sb::test_case("error codes are identical across the comptime/runtime seam");
    sb::require_true(cjson::validate("", 0) == cjson::error::empty_input);
    sb::require_true(cjson::validate("   ", 3) == cjson::error::empty_input);
    sb::require_true(cjson::validate("{} x", 4) == cjson::error::trailing_garbage);
    sb::require_true(cjson::validate("[1,,2]", 6) == cjson::error::bad_syntax);
    sb::require_true(cjson::validate("01", 2) == cjson::error::bad_number);
    sb::require_true(cjson::validate("1e", 2) == cjson::error::bad_number);
    sb::require_true(cjson::validate("\"abc", 4) == cjson::error::bad_string);
    sb::require_true(cjson::validate("\"\x01\"", 3) == cjson::error::bad_string);
    sb::require_true(cjson::validate("\"\\x41\"", 6) == cjson::error::bad_escape);
    sb::require_true(cjson::validate("\"\\uD800\"", 8) == cjson::error::bad_surrogate);
    sb::require_true(cjson::validate("\"\xc0\x80\"", 4) == cjson::error::bad_utf8);

    // F1 codes, across the seam
    sb::require_true(cjson::validate("1\0", 2) == cjson::error::bad_number);
    sb::require_true(cjson::validate("1\0garbage", 9) == cjson::error::bad_number);
    sb::require_true(cjson::validate("true\0junk", 9) == cjson::error::bad_syntax);
    sb::require_true(cjson::validate("{}\0", 3) == cjson::error::trailing_garbage);
    sb::end_test_case();
  }
  {
    sb::test_case("the depth cap is identical across the seam");
    for ( u32 n : { 64u, cjson::depth_limit + 1, cjson::depth_limit + 2 } ) {
      micron::vector<u8> d;
      d.reserve(2 * n);
      for ( u32 i = 0; i < n; ++i ) d.push_back(u8('['));
      for ( u32 i = 0; i < n; ++i ) d.push_back(u8(']'));
      const cjson::error e = cjson::validate(d.cbegin(), d.size());
      const cjson::error want = (n <= cjson::depth_limit + 1) ? cjson::error::ok : cjson::error::depth_exceeded;
      sb::require_true(e == want);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("ct:: bakes and validates the same grammar");
    static_assert(cjson::ct::validate<cjson::ct::str{ R"({"a":[1,2,3],"b":"x"})" }>());
    static_assert(!cjson::ct::validate<cjson::ct::str{ "[1,,2]" }>());
    static_assert(!cjson::ct::validate<cjson::ct::str{ "01" }>());
    static_assert(!cjson::ct::validate<cjson::ct::str{ "\"\\uD800\"" }>());
    sb::require_true(true);
    sb::end_test_case();
  }
  return 1;
}
