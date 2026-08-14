//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// rfc 8259 s8.1: "JSON text exchanged between systems that are not part of a closed
// ecosystem MUST be encoded using UTF-8." cjson validates utf-8 over the WHOLE input,
// not just inside strings, so this file drives well-formed and ill-formed sequences
// through the validator and pins both verdicts.
//
// The block-seam sweep is the highest-yield test here. On amd64 the utf-8 checker is
// FUSED into the avx2 classify loop, with prev1/2/3 coming from offset loads and block 0
// and the borrowed tail running through a 32-byte-prefixed scratch. A sequence that
// straddles a 64-byte boundary therefore takes a different code path from one that does
// not, so every case is re-run at ~140 offsets to land it before, across and after the
// seam. arm64/arm32 have no fused checker and fall to the scalar decoder, which is the
// oracle -- so this file is also a twin-identity test under `duck ... --arm`.
//
// Byte sequences are explicit u8 arrays, never string literals: C++ hex escapes are
// greedy, so "\xc0abc" is one enormous escape rather than the four bytes intended.

#include "rfc_cases.hpp"

#include "../tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

struct seq {
  const u8 *b;
  usize n;
  const char *why;
};

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// well-formed, including every boundary of rfc 3629's four ranges

// NOTE 0x00-0x1f are well-formed utf-8 but are NOT legal unescaped inside a string
// (s7 unescaped = %x20-21 / ...), so the one-byte range is probed at 0x20, not 0x01.
// rfc_strings.cpp owns the control-character rule; this file owns the encoding rule.
const u8 s_1lo[] = { 0x20 };
const u8 s_1hi[] = { 0x7f };
const u8 s_2lo[] = { 0xc2, 0x80 };                  // u+0080
const u8 s_2hi[] = { 0xdf, 0xbf };                  // u+07ff
const u8 s_3lo[] = { 0xe0, 0xa0, 0x80 };            // u+0800
const u8 s_3sur_lo[] = { 0xed, 0x9f, 0xbf };        // u+d7ff, just below the surrogates
const u8 s_3sur_hi[] = { 0xee, 0x80, 0x80 };        // u+e000, just above them
const u8 s_3hi[] = { 0xef, 0xbf, 0xbf };            // u+ffff
const u8 s_4lo[] = { 0xf0, 0x90, 0x80, 0x80 };      // u+10000
const u8 s_4hi[] = { 0xf4, 0x8f, 0xbf, 0xbf };      // u+10ffff
const u8 s_euro[] = { 0xe2, 0x82, 0xac };           // u+20ac
const u8 s_eacute[] = { 0xc3, 0xa9 };               // u+00e9

const seq k_good[] = {
  { s_1lo, 1, "u+0001, one byte" },
  { s_1hi, 1, "u+007f, top of the one-byte range" },
  { s_2lo, 2, "u+0080, bottom of the two-byte range" },
  { s_2hi, 2, "u+07ff, top of the two-byte range" },
  { s_3lo, 3, "u+0800, bottom of the three-byte range" },
  { s_3sur_lo, 3, "u+d7ff, immediately below the surrogate block" },
  { s_3sur_hi, 3, "u+e000, immediately above the surrogate block" },
  { s_3hi, 3, "u+ffff, top of the three-byte range" },
  { s_4lo, 4, "u+10000, bottom of the four-byte range" },
  { s_4hi, 4, "u+10ffff, the last code point" },
  { s_euro, 3, "u+20ac euro sign" },
  { s_eacute, 2, "u+00e9 e acute" },
};

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// ill-formed

const u8 b_over2a[] = { 0xc0, 0x80 };                  // overlong u+0000
const u8 b_over2b[] = { 0xc1, 0xbf };                  // overlong u+007f
const u8 b_over3[] = { 0xe0, 0x80, 0x80 };             // overlong
const u8 b_over3b[] = { 0xe0, 0x9f, 0xbf };            // overlong u+07ff
const u8 b_over4[] = { 0xf0, 0x80, 0x80, 0x80 };       // overlong
const u8 b_over4b[] = { 0xf0, 0x8f, 0xbf, 0xbf };      // overlong u+ffff
const u8 b_sur_lo[] = { 0xed, 0xa0, 0x80 };            // u+d800 encoded directly
const u8 b_sur_hi[] = { 0xed, 0xbf, 0xbf };            // u+dfff encoded directly
const u8 b_sur_pair[] = { 0xed, 0xa0, 0xbd, 0xed, 0xb8, 0x80 };
const u8 b_too_big[] = { 0xf4, 0x90, 0x80, 0x80 };      // u+110000
const u8 b_f5[] = { 0xf5, 0x80, 0x80, 0x80 };           // beyond the f4 lead
const u8 b_f7[] = { 0xf7, 0xbf, 0xbf, 0xbf };
const u8 b_5byte[] = { 0xf8, 0x88, 0x80, 0x80, 0x80 };
const u8 b_6byte[] = { 0xfc, 0x84, 0x80, 0x80, 0x80, 0x80 };
const u8 b_fe[] = { 0xfe };
const u8 b_ff[] = { 0xff };
const u8 b_cont1[] = { 0x80 };      // stray continuation
const u8 b_cont2[] = { 0xbf };
const u8 b_trunc2[] = { 0xc2 };                   // lead with no continuation
const u8 b_trunc3[] = { 0xe2, 0x82 };             // three-byte, one short
const u8 b_trunc4[] = { 0xf0, 0x9d, 0x84 };       // four-byte, one short
const u8 b_badcont[] = { 0xe2, 0x28, 0xa1 };      // continuation slot holds ascii

const seq k_bad[] = {
  { b_over2a, 2, "overlong two-byte encoding of u+0000" },
  { b_over2b, 2, "overlong two-byte encoding of u+007f" },
  { b_over3, 3, "overlong three-byte encoding" },
  { b_over3b, 3, "overlong three-byte encoding of u+07ff" },
  { b_over4, 4, "overlong four-byte encoding" },
  { b_over4b, 4, "overlong four-byte encoding of u+ffff" },
  { b_sur_lo, 3, "s8.2 u+d800 encoded directly in utf-8" },
  { b_sur_hi, 3, "s8.2 u+dfff encoded directly in utf-8" },
  { b_sur_pair, 6, "cesu-8 style surrogate pair" },
  { b_too_big, 4, "u+110000 is beyond u+10ffff" },
  { b_f5, 4, "0xf5 lead byte is out of range" },
  { b_f7, 4, "0xf7 lead byte is out of range" },
  { b_5byte, 5, "five-byte sequences are not utf-8" },
  { b_6byte, 6, "six-byte sequences are not utf-8" },
  { b_fe, 1, "0xfe never appears in utf-8" },
  { b_ff, 1, "0xff never appears in utf-8" },
  { b_cont1, 1, "stray continuation byte 0x80" },
  { b_cont2, 1, "stray continuation byte 0xbf" },
  { b_trunc2, 1, "two-byte lead with no continuation" },
  { b_trunc3, 2, "three-byte sequence truncated" },
  { b_trunc4, 3, "four-byte sequence truncated" },
  { b_badcont, 3, "continuation slot holding an ascii byte" },
};

// wrap a byte run in a string value:  ["<pad>xxx<bytes>"]
micron::vector<u8>
in_string(const u8 *b, usize n, u32 pad)
{
  micron::vector<u8> d;
  d.push_back(u8('['));
  d.push_back(u8('"'));
  for ( u32 i = 0; i < pad; ++i ) d.push_back(u8('x'));
  for ( usize i = 0; i < n; ++i ) d.push_back(b[i]);
  d.push_back(u8('"'));
  d.push_back(u8(']'));
  return d;
}

// as a member NAME rather than a value
micron::vector<u8>
in_key(const u8 *b, usize n)
{
  micron::vector<u8> d;
  d.push_back(u8('{'));
  d.push_back(u8('"'));
  for ( usize i = 0; i < n; ++i ) d.push_back(b[i]);
  d.push_back(u8('"'));
  d.push_back(u8(':'));
  d.push_back(u8('1'));
  d.push_back(u8('}'));
  return d;
}

// bare in the document body, outside any string
micron::vector<u8>
bare(const u8 *b, usize n)
{
  micron::vector<u8> d;
  d.push_back(u8('['));
  for ( usize i = 0; i < n; ++i ) d.push_back(b[i]);
  d.push_back(u8(']'));
  return d;
}

};      // namespace

int
main()
{
  {
    sb::test_case("s8.1: well-formed utf-8 is accepted at every range boundary");
    for ( const seq &s : k_good ) {
      auto d = in_string(s.b, s.n, 0);
      if ( cjson::validate(d.cbegin(), d.size()) != cjson::error::ok ) snowball::print("s8.1 FAILED (false reject): ", s.why);
      sb::require_true(cjson::validate(d.cbegin(), d.size()) == cjson::error::ok);
      sb::require_true(cjson::parse(cjson::bytes{ d.cbegin(), d.size() }).is_first());

      auto k = in_key(s.b, s.n);
      sb::require_true(cjson::validate(k.cbegin(), k.size()) == cjson::error::ok);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("s8.1: ill-formed utf-8 is rejected inside a string value");
    for ( const seq &s : k_bad ) {
      auto d = in_string(s.b, s.n, 0);
      if ( cjson::validate(d.cbegin(), d.size()) == cjson::error::ok ) snowball::print("s8.1 FAILED (accepted): ", s.why);
      sb::require_true(cjson::validate(d.cbegin(), d.size()) != cjson::error::ok);
      sb::require_true(cjson::parse(cjson::bytes{ d.cbegin(), d.size() }).is_second());
    }
    sb::end_test_case();
  }
  {
    // a key is just as much part of the text as a value; a checker that only sweeps
    // string VALUES would pass the previous block and fail this one
    sb::test_case("s8.1: ill-formed utf-8 is rejected inside a member name");
    for ( const seq &s : k_bad ) {
      auto d = in_key(s.b, s.n);
      if ( cjson::validate(d.cbegin(), d.size()) == cjson::error::ok ) snowball::print("s8.1 FAILED (accepted in key): ", s.why);
      sb::require_true(cjson::validate(d.cbegin(), d.size()) != cjson::error::ok);
    }
    sb::end_test_case();
  }
  {
    // utf-8 is validated over the whole input, so ill-formed bytes outside any string
    // must be refused too (they are also a syntax error, but they must never be ACCEPTED)
    sb::test_case("s8.1: ill-formed utf-8 outside a string is rejected");
    for ( const seq &s : k_bad ) {
      auto d = bare(s.b, s.n);
      sb::require_true(cjson::validate(d.cbegin(), d.size()) != cjson::error::ok);
    }
    sb::end_test_case();
  }
  {
    // THE seam sweep. 140 offsets carries every case across the 64-byte block boundary
    // and the 128-byte pair boundary, so the fused avx2 checker's prev1/2/3 offset loads,
    // its block-0 scratch prefix and its borrowed-tail scratch all get exercised with the
    // sequence split at every possible position.
    sb::test_case("s8.1: verdicts are identical at every offset across the block seams");
    for ( const seq &s : k_bad ) {
      for ( u32 pad = 0; pad < 140; ++pad ) {
        auto d = in_string(s.b, s.n, pad);
        if ( cjson::validate(d.cbegin(), d.size()) == cjson::error::ok ) {
          snowball::print("s8.1 seam FAILED (accepted): ", s.why);
          snowball::print("   at pad ", pad);
        }
        sb::require_true(cjson::validate(d.cbegin(), d.size()) != cjson::error::ok);
      }
    }
    for ( const seq &s : k_good ) {
      for ( u32 pad = 0; pad < 140; ++pad ) {
        auto d = in_string(s.b, s.n, pad);
        if ( cjson::validate(d.cbegin(), d.size()) != cjson::error::ok ) {
          snowball::print("s8.1 seam FAILED (false reject): ", s.why);
          snowball::print("   at pad ", pad);
        }
        sb::require_true(cjson::validate(d.cbegin(), d.size()) == cjson::error::ok);
      }
    }
    sb::end_test_case();
  }
  {
    // every entry point shares one stage-1 sweep, so they must land on the same verdict
    sb::test_case("s8.1: every entry point agrees on utf-8 validity");
    for ( const seq &s : k_bad ) {
      auto d = in_string(s.b, s.n, 3);
      const cjson::bytes v{ d.cbegin(), d.size() };
      sb::require_true(cjson::validate(v) != cjson::error::ok);
      sb::require_false(cjson::is_valid(v));
      sb::require_true(cjson::parse(v).is_second());

      cjson::scratch sc;
      sb::require_true(cjson::iterate(v, sc).is_second());

      micron::vector<u8> out;
      out.reserve(cjson::minify_bound(d.size()) + cjson::padding);
      sb::require_true(cjson::minify(v, cjson::wbytes{ out.begin(), cjson::minify_bound(d.size()) }) < 0);

      micron::vector<u8> mut = d.clone();
      sb::require_true(cjson::parse_insitu(cjson::wbytes{ mut.begin(), mut.size() }).is_second());
    }
    sb::end_test_case();
  }
  {
    // opts::skip_utf8 is an opt-in extension, permitted by s9. Default OFF; this pins
    // both that it is off by default and what turning it on actually does.
    sb::test_case("s9: skip_utf8 is opt-in and off by default");
    for ( const seq &s : k_bad ) {
      auto d = in_string(s.b, s.n, 0);
      const cjson::bytes v{ d.cbegin(), d.size() };
      sb::require_true(cjson::validate(v) != cjson::error::ok);
      sb::require_true(cjson::validate(v, cjson::opts{ .skip_utf8 = true }) == cjson::error::ok);
      sb::require_true(cjson::parse(v, cjson::opts{ .skip_utf8 = true }).is_first());
    }
    // and it does not turn off anything ELSE: the grammar and the control-character rule
    // still hold with it set
    sb::require_true(cjson::validate(reinterpret_cast<const u8 *>("[1,,2]"), 6, cjson::opts{ .skip_utf8 = true }) != cjson::error::ok);
    {
      const u8 ctrl[5] = { u8('['), u8('"'), u8(0x01), u8('"'), u8(']') };
      sb::require_true(cjson::validate(ctrl, 5, cjson::opts{ .skip_utf8 = true }) != cjson::error::ok);
    }
    sb::end_test_case();
  }
  {
    // the whole valid corpus must pass utf-8 validation; a false reject here is as bad
    // as a false accept above
    sb::test_case("s8.1: the valid corpus validates clean");
    const char *files[]
        = { "sample/64kb.json", "sample/128KB.json", "sample/256KB.json", "sample/512KB.json", "sample/1MB.json", "sample/twitter.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      sb::require_true(cjson::validate(tutil::view(data)) == cjson::error::ok);
    }
    sb::end_test_case();
  }
  return 1;
}
