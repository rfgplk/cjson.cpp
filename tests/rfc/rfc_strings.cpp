//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// rfc 8259 s7 (the string ABNF), s8.2 (unpaired surrogates) and s8.3 (string comparison).
//
//   string    = quotation-mark *char quotation-mark
//   char      = unescaped / escape ( %x22 / %x5C / %x2F / %x62 / %x66 / %x6E / %x72 /
//                                    %x74 / %x75 4HEXDIG )
//   unescaped = %x20-21 / %x23-5B / %x5D-10FFFF
//
// The unescaped production is the sharp edge and is swept exhaustively: every one of the
// 32 control characters U+0000..U+001F must be refused raw, and U+007F DEL must be
// ACCEPTED raw -- it sits inside %x5D-10FFFF and is not escape-required. Getting that
// backwards in either direction is the classic bug.

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
using rfc::verdict;

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// s7: the escape set is exactly eight two-character forms, plus \uXXXX

const rfc::kase k_escapes[] = {
  K("\"\\\"\"", verdict::accept, "s7 escape %x22 quotation mark"),
  K("\"\\\\\"", verdict::accept, "s7 escape %x5C reverse solidus"),
  K("\"\\/\"", verdict::accept, "s7 escape %x2F solidus"),
  K("\"\\b\"", verdict::accept, "s7 escape %x62 backspace"),
  K("\"\\f\"", verdict::accept, "s7 escape %x66 form feed"),
  K("\"\\n\"", verdict::accept, "s7 escape %x6E line feed"),
  K("\"\\r\"", verdict::accept, "s7 escape %x72 carriage return"),
  K("\"\\t\"", verdict::accept, "s7 escape %x74 tab"),
  K("\"\\u0041\"", verdict::accept, "s7 escape %x75 4HEXDIG"),
  K("\"/\"", verdict::accept, "s7 solidus need not be escaped"),
  K("\"\\u005C\"", verdict::accept, "s7 the spec's own \\u005C example"),
  K("\"\\b\\f\\n\\r\\t\\\\\\\"\\/\"", verdict::accept, "s7 all eight escapes in one string"),

  K("\"\\x41\"", verdict::reject, "s7 \\x is not in the char production"),
  K("\"\\'\"", verdict::reject, "s7 \\' is not in the char production"),
  K("\"\\a\"", verdict::reject, "s7 \\a is not in the char production"),
  K("\"\\v\"", verdict::reject, "s7 \\v is not in the char production"),
  K("\"\\0\"", verdict::reject, "s7 \\0 is not in the char production"),
  K("\"\\e\"", verdict::reject, "s7 \\e is not in the char production"),
  K("\"\\ \"", verdict::reject, "s7 an escaped space is not in the char production"),
  K("\"\\U0041\"", verdict::reject, "s7 the u in \\uXXXX is lowercase"),
  K("\"\\\"", verdict::reject, "s7 a trailing escape swallows the closing quote"),
  K("\"\\", verdict::reject, "s7 escape at end of input"),
};

// s7: \u requires exactly four hex digits, and "The hexadecimal letters A through F can
// be uppercase or lowercase."
const rfc::kase k_hex[] = {
  K("\"\\u0041\"", verdict::accept, "s7 4HEXDIG, digits"),
  K("\"\\uABCD\"", verdict::accept, "s7 4HEXDIG, uppercase letters"),
  K("\"\\uabcd\"", verdict::accept, "s7 4HEXDIG, lowercase letters"),
  K("\"\\uAbCd\"", verdict::accept, "s7 4HEXDIG, mixed case"),
  K("\"\\uFFFF\"", verdict::accept, "s7 u+ffff is a legal escape"),
  K("\"\\u0000\"", verdict::accept, "s7 u+0000 may be escaped"),
  K("\"\\u001F\"", verdict::accept, "s7 control characters may be escaped"),
  K("\"\\u007F\"", verdict::accept, "s7 del may be escaped"),

  K("\"\\u\"", verdict::reject, "s7 \\u needs 4HEXDIG"),
  K("\"\\u1\"", verdict::reject, "s7 \\u needs 4, not 1"),
  K("\"\\u12\"", verdict::reject, "s7 \\u needs 4, not 2"),
  K("\"\\u123\"", verdict::reject, "s7 \\u needs 4, not 3"),
  K("\"\\u123g\"", verdict::reject, "s7 g is not a hex digit"),
  K("\"\\uG123\"", verdict::reject, "s7 G is not a hex digit"),
  K("\"\\u 123\"", verdict::reject, "s7 a space is not a hex digit"),
  K("\"\\u-123\"", verdict::reject, "s7 a minus is not a hex digit"),
  K("\"\\u12 4\"", verdict::reject, "s7 a space is not a hex digit"),
  K("\"\\u", verdict::reject, "s7 \\u at end of input"),
};

// s7 / s8.2: an extended character is "represented as a 12-character sequence, encoding
// the UTF-16 surrogate pair". A half on its own is the "\uDEAD" case s8.2 names.
const rfc::kase k_surrogates[] = {
  K("\"\\uD834\\uDD1E\"", verdict::accept, "s7 the spec's own G clef u+1d11e example"),
  K("\"\\ud834\\udd1e\"", verdict::accept, "s7 the same pair, lowercase hex"),
  K("\"\\uD800\\uDC00\"", verdict::accept, "s7 the lowest surrogate pair, u+10000"),
  K("\"\\uDBFF\\uDFFF\"", verdict::accept, "s7 the highest surrogate pair, u+10ffff"),
  K("\"\\uD83D\\uDE00\"", verdict::accept, "s7 an ordinary emoji pair"),

  K("\"\\uDEAD\"", verdict::reject, "s8.2 the spec's own unpaired-surrogate example"),
  K("\"\\uD800\"", verdict::reject, "s8.2 lone high surrogate"),
  K("\"\\uDBFF\"", verdict::reject, "s8.2 lone high surrogate, top of range"),
  K("\"\\uDC00\"", verdict::reject, "s8.2 lone low surrogate"),
  K("\"\\uDFFF\"", verdict::reject, "s8.2 lone low surrogate, top of range"),
  K("\"\\uDC00\\uD800\"", verdict::reject, "s8.2 a reversed pair is not a pair"),
  K("\"\\uD800\\uD800\"", verdict::reject, "s8.2 two high halves"),
  K("\"\\uD800x\"", verdict::reject, "s8.2 high half followed by an ordinary character"),
  K("\"\\uD800\\n\"", verdict::reject, "s8.2 high half followed by a two-character escape"),
  K("\"\\uD800\\u0041\"", verdict::reject, "s8.2 high half followed by a bmp escape"),
  K("\"\\uD800 \\uDC00\"", verdict::reject, "s8.2 the low half must immediately follow"),
  K("\"\\uD800", verdict::reject, "s8.2 high half at end of input"),
  K("[\"\\uD800\",\"\\uDC00\"]", verdict::reject, "s8.2 halves split across two strings"),
};

// s7: strings are delimited by quotation marks
const rfc::kase k_delims[] = {
  K("\"\"", verdict::accept, "s7 the empty string"),
  K("\"abc\"", verdict::accept, "s7 ordinary content"),
  K("\"", verdict::reject, "s7 unterminated string"),
  K("\"abc", verdict::reject, "s7 unterminated string"),
  K("'abc'", verdict::reject, "s7 apostrophes do not delimit a string"),
  K("[\"a\",\"b]", verdict::reject, "s7 unterminated string inside an array"),
  K("{\"a\":\"b}", verdict::reject, "s7 unterminated string as a member value"),
};

};      // namespace

int
main()
{
  {
    sb::test_case("s7: the escape set is exactly the eight forms plus \\uXXXX");
    rfc::expect_all(k_escapes);
    sb::end_test_case();
  }
  {
    sb::test_case("s7: \\u takes exactly four hex digits, either case");
    rfc::expect_all(k_hex);
    sb::end_test_case();
  }
  {
    sb::test_case("s7/s8.2: surrogate halves are paired or rejected, never replaced");
    rfc::expect_all(k_surrogates);
    sb::end_test_case();
  }
  {
    sb::test_case("s7: quotation marks delimit a string");
    rfc::expect_all(k_delims);
    sb::end_test_case();
  }
  {
    // unescaped = %x20-21 / %x23-5B / %x5D-10FFFF. Sweep the whole excluded low range
    // one byte at a time rather than spot-checking it.
    sb::test_case("s7: all 32 control characters U+0000..U+001F are refused raw");
    for ( u32 c = 0x00; c <= 0x1f; ++c ) {
      const u8 doc[3] = { u8('"'), u8(c), u8('"') };
      if ( cjson::validate(doc, 3) == cjson::error::ok ) snowball::print("s7 FAILED: raw control char accepted, byte ", c);
      sb::require_true(cjson::validate(doc, 3) != cjson::error::ok);
      sb::require_true(cjson::parse(doc, 3).is_second());

      // the same byte behind a backslash is not a line continuation either
      const u8 esc[4] = { u8('"'), u8('\\'), u8(c), u8('"') };
      sb::require_true(cjson::validate(esc, 4) != cjson::error::ok);

      // and inside a member name, not just a value
      const u8 key[8] = { u8('{'), u8('"'), u8(c), u8('"'), u8(':'), u8('1'), u8('}'), u8(' ') };
      sb::require_true(cjson::validate(key, 7) != cjson::error::ok);
    }
    sb::end_test_case();
  }
  {
    // 0x7f is INSIDE %x5D-10FFFF: it is not escape-required, and refusing it would be
    // just as wrong as accepting 0x1f
    sb::test_case("s7: U+007F DEL is ordinary unescaped content");
    const u8 doc[3] = { u8('"'), u8(0x7f), u8('"') };
    sb::require_true(cjson::validate(doc, 3) == cjson::error::ok);
    auto r = cjson::parse(doc, 3);
    sb::require_true(r.is_first());
    auto s = r.cast<cjson::doc>().root().str_or();
    sb::require(s.len, static_cast<usize>(1));
    sb::require_true(u8(s.ptr[0]) == 0x7f);

    // as should every other printable ascii byte in %x20-21 and %x23-5B
    for ( u32 c = 0x20; c <= 0x7f; ++c ) {
      if ( c == u32('"') or c == u32('\\') ) continue;      // these two MUST be escaped
      const u8 d[3] = { u8('"'), u8(c), u8('"') };
      if ( cjson::validate(d, 3) != cjson::error::ok ) snowball::print("s7 FAILED: printable byte refused, byte ", c);
      sb::require_true(cjson::validate(d, 3) == cjson::error::ok);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("s7: the two escape-required characters are refused raw");
    const u8 q[3] = { u8('"'), u8('"'), u8('"') };
    sb::require_true(cjson::validate(q, 3) != cjson::error::ok);
    const u8 b[3] = { u8('"'), u8('\\'), u8('"') };
    sb::require_true(cjson::validate(b, 3) != cjson::error::ok);
    sb::end_test_case();
  }
  {
    // s7 permits \u0000, so a decoded string may contain a nul. The length is the
    // contract; any c_str()-shaped reader truncates and that is a documented hazard.
    sb::test_case("s7: \\u0000 decodes to a real nul and the length carries it");
    auto r = PJ("\"a\\u0000b\"");
    sb::require_true(r.is_first());
    auto s = r.cast<cjson::doc>().root().str_or();
    sb::require(s.len, static_cast<usize>(3));
    sb::require_true(s.ptr[0] == 'a');
    sb::require_true(u8(s.ptr[1]) == 0x00);
    sb::require_true(s.ptr[2] == 'b');
    sb::end_test_case();
  }
  {
    // s7: the G clef is the spec's worked example; check the bytes, not just acceptance
    sb::test_case("s7: a surrogate pair decodes to the right utf-8 bytes");
    auto r = PJ("\"\\uD834\\uDD1E\"");
    sb::require_true(r.is_first());
    auto s = r.cast<cjson::doc>().root().str_or();
    sb::require(s.len, static_cast<usize>(4));
    sb::require_true(u8(s.ptr[0]) == 0xf0 and u8(s.ptr[1]) == 0x9d);
    sb::require_true(u8(s.ptr[2]) == 0x84 and u8(s.ptr[3]) == 0x9e);

    // u+10ffff, the top of the range
    auto r2 = PJ("\"\\uDBFF\\uDFFF\"");
    sb::require_true(r2.is_first());
    auto s2 = r2.cast<cjson::doc>().root().str_or();
    sb::require(s2.len, static_cast<usize>(4));
    sb::require_true(u8(s2.ptr[0]) == 0xf4 and u8(s2.ptr[1]) == 0x8f);
    sb::require_true(u8(s2.ptr[2]) == 0xbf and u8(s2.ptr[3]) == 0xbf);
    sb::end_test_case();
  }
  {
    // s8.3: "implementations that compare strings with escaped characters unconverted may
    // incorrectly find that "a\\b" and "a\u005Cb" are not equal." That is the spec's own
    // example, so it is the test.
    sb::test_case("s8.3: escaped and unescaped spellings of a name compare equal");
    auto r1 = PJ(R"({"a\\b":1})");
    auto r2 = PJ(R"({"a\u005Cb":2})");
    sb::require_true(r1.is_first() and r2.is_first());

    // the lookup key is a, reverse solidus, b
    const char key[] = { 'a', '\\', 'b' };
    const cjson::strv k{ key, 3 };
    sb::require(r1.cast<cjson::doc>().root()[k].i64_or(-1), static_cast<i64>(1));
    sb::require(r2.cast<cjson::doc>().root()[k].i64_or(-1), static_cast<i64>(2));

    // and the decoded names are byte-identical
    auto n1 = r1.cast<cjson::doc>().root().members().begin();
    auto n2 = r2.cast<cjson::doc>().root().members().begin();
    sb::require((*n1).key.len, (*n2).key.len);
    sb::require((*n1).key.len, static_cast<usize>(3));
    for ( usize i = 0; i < 3; ++i ) sb::require_true((*n1).key.ptr[i] == (*n2).key.ptr[i]);
    sb::end_test_case();
  }
  {
    // the same equality, for a value rather than a name
    sb::test_case("s8.3: escaped and unescaped spellings of a value decode identically");

    struct pair {
      const char *a;
      usize an;
      const char *b;
      usize bn;
    };

    // lengths are DEDUCED, never hand-counted: an off-by-one silently truncates the very
    // case under test into a different one (it did, on the first draft of this table)
    auto P = []<usize NA, usize NB>(const char (&a)[NA], const char (&b)[NB]) { return pair{ a, NA - 1, b, NB - 1 }; };
    const pair ps[] = {
      P(R"("a\\b")", R"("a\u005Cb")"), P(R"("\/")", R"("\u002F")"), P(R"("/")", R"("\u002F")"),  P(R"("\n")", R"("\u000A")"),
      P(R"("\t")", R"("\u0009")"),     P(R"("\r")", R"("\u000D")"), P(R"("\b")", R"("\u0008")"), P(R"("\f")", R"("\u000C")"),
      P(R"("A")", R"("\u0041")"),      P(R"("\"")", R"("\u0022")"),
    };
    for ( const pair &p : ps ) {
      auto ra = cjson::parse(reinterpret_cast<const u8 *>(p.a), p.an);
      auto rb = cjson::parse(reinterpret_cast<const u8 *>(p.b), p.bn);
      sb::require_true(ra.is_first() and rb.is_first());
      auto sa = ra.cast<cjson::doc>().root().str_or();
      auto sb_ = rb.cast<cjson::doc>().root().str_or();
      sb::require(sa.len, sb_.len);
      for ( usize i = 0; i < sa.len; ++i ) sb::require_true(sa.ptr[i] == sb_.ptr[i]);
    }
    sb::end_test_case();
  }
  {
    // escapes must survive the 64-byte block seam: the escape resolver carries state
    // across blocks, and an escape run landing on a boundary is where that shows
    sb::test_case("s7: escapes decode identically at every offset across a block seam");
    for ( u32 pad = 0; pad < 140; ++pad ) {
      micron::vector<u8> doc;
      doc.push_back(u8('"'));
      for ( u32 i = 0; i < pad; ++i ) doc.push_back(u8('x'));
      const char tail[] = "\\u0041\\n\\\\\\\"";      // A, LF, backslash, quote
      for ( usize i = 0; tail[i]; ++i ) doc.push_back(u8(tail[i]));
      doc.push_back(u8('"'));

      auto r = cjson::parse(cjson::bytes{ doc.cbegin(), doc.size() });
      if ( !r.is_first() ) snowball::print("s7 seam FAILED at pad=", pad);
      sb::require_true(r.is_first());
      auto s = r.cast<cjson::doc>().root().str_or();
      sb::require(s.len, static_cast<usize>(pad + 4));
      sb::require_true(s.ptr[pad + 0] == 'A');
      sb::require_true(s.ptr[pad + 1] == '\n');
      sb::require_true(s.ptr[pad + 2] == '\\');
      sb::require_true(s.ptr[pad + 3] == '"');
    }
    sb::end_test_case();
  }
  return 1;
}
