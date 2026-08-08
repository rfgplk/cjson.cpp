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

// rfc 3629 conformance for the scalar validator (the constexpr twin and the accept-set
// contract every simd body must match). vectors are hand-written — boundary characters,
// overlongs, surrogates, out-of-range and truncations

namespace
{

bool
ok(std::initializer_list<int> b)
{
  micron::vector<u8> v;
  for ( int x : b ) v.push_back(u8(x));
  return cjson::__utf8::validate_scalar(v.cbegin(), v.size());
}

// independent slow decoder: decode one codepoint at a time by the book
bool
ref_ok(const u8 *p, usize len)
{
  usize i = 0;
  while ( i < len ) {
    const u8 c = p[i];
    u32 cp = 0;
    u32 need = 0;
    if ( c < 0x80 ) {
      ++i;
      continue;
    } else if ( (c & 0xe0) == 0xc0 ) {
      cp = c & 0x1f;
      need = 1;
    } else if ( (c & 0xf0) == 0xe0 ) {
      cp = c & 0x0f;
      need = 2;
    } else if ( (c & 0xf8) == 0xf0 ) {
      cp = c & 0x07;
      need = 3;
    } else {
      return false;
    }
    if ( i + need >= len ) return false;
    for ( u32 k = 1; k <= need; k++ ) {
      if ( (p[i + k] & 0xc0) != 0x80 ) return false;
      cp = (cp << 6) | (p[i + k] & 0x3f);
    }
    if ( need == 1 and cp < 0x80 ) return false;
    if ( need == 2 and cp < 0x800 ) return false;
    if ( need == 3 and cp < 0x10000 ) return false;
    if ( cp >= 0xd800 and cp <= 0xdfff ) return false;
    if ( cp > 0x10ffff ) return false;
    i += need + 1;
  }
  return true;
}

};      // namespace

int
main()
{
  {
    sb::test_case("boundary codepoints of every sequence length are accepted");
    sb::require_true(ok({}));
    sb::require_true(ok({ 0x61 }));
    sb::require_true(ok({ 0x7f }));
    sb::require_true(ok({ 0xc2, 0x80 }));                  // u+0080
    sb::require_true(ok({ 0xdf, 0xbf }));                  // u+07ff
    sb::require_true(ok({ 0xe0, 0xa0, 0x80 }));            // u+0800
    sb::require_true(ok({ 0xed, 0x9f, 0xbf }));            // u+d7ff
    sb::require_true(ok({ 0xee, 0x80, 0x80 }));            // u+e000
    sb::require_true(ok({ 0xef, 0xbf, 0xbf }));            // u+ffff
    sb::require_true(ok({ 0xf0, 0x90, 0x80, 0x80 }));      // u+10000
    sb::require_true(ok({ 0xf4, 0x8f, 0xbf, 0xbf }));      // u+10ffff
    sb::require_true(ok({ 'a', 0xc3, 0xa9, 0xe4, 0xb8, 0xad, 0xf0, 0x9f, 0x98, 0x80, 'z' }));
    sb::end_test_case();
  }
  {
    sb::test_case("overlongs surrogates out-of-range and orphans are rejected");
    sb::require_false(ok({ 0xc0, 0x80 }));      // overlong 2-byte
    sb::require_false(ok({ 0xc1, 0xbf }));
    sb::require_false(ok({ 0xe0, 0x80, 0x80 }));      // overlong 3-byte
    sb::require_false(ok({ 0xe0, 0x9f, 0xbf }));
    sb::require_false(ok({ 0xf0, 0x80, 0x80, 0x80 }));      // overlong 4-byte
    sb::require_false(ok({ 0xf0, 0x8f, 0xbf, 0xbf }));
    sb::require_false(ok({ 0xed, 0xa0, 0x80 }));            // u+d800
    sb::require_false(ok({ 0xed, 0xbf, 0xbf }));            // u+dfff
    sb::require_false(ok({ 0xf4, 0x90, 0x80, 0x80 }));      // > u+10ffff
    sb::require_false(ok({ 0xf5, 0x80, 0x80, 0x80 }));
    sb::require_false(ok({ 0xff }));
    sb::require_false(ok({ 0x80 }));      // lone continuation
    sb::require_false(ok({ 0xbf }));
    sb::end_test_case();
  }
  {
    sb::test_case("truncated sequences and broken continuations are rejected");
    sb::require_false(ok({ 0xc2 }));
    sb::require_false(ok({ 0xe2, 0x82 }));
    sb::require_false(ok({ 0xf0, 0x9f, 0x92 }));
    sb::require_false(ok({ 0x61, 0xc2, 0x61 }));      // continuation replaced by ascii
    sb::require_false(ok({ 0xe4, 0xb8, 0x2d }));
    sb::require_false(ok({ 0xc2, 0xc2, 0x80 }));      // lead where continuation expected
    sb::end_test_case();
  }
  {
    sb::test_case("random byte soup matches an independent by-the-book decoder");
    tutil::rng rg;
    for ( u32 iter = 0; iter < 20000; iter++ ) {
      micron::vector<u8> v;
      const u32 n = 1 + rg.below(24);
      for ( u32 i = 0; i < n; i++ ) {
        // bias toward interesting lead bytes
        const u32 pick = rg.below(100);
        u8 c;
        if ( pick < 30 )
          c = u8(0x80 | rg.below(0x40));
        else if ( pick < 60 )
          c = u8(0xc0 | rg.below(0x40));
        else
          c = u8(rg.below(256));
        v.push_back(c);
      }
      const bool a = cjson::__utf8::validate_scalar(v.cbegin(), v.size());
      const bool b = ref_ok(v.cbegin(), v.size());
      if ( a != b ) {
        sb::print("mismatch at iter ", iter);
        sb::require_true(false);
      }
    }
    sb::end_test_case();
  }
  {
    sb::test_case("the sample corpus validates");
    auto tw = tutil::slurp("sample/twitter.json");
    sb::require_greater(tw.size(), static_cast<usize>(0));
    sb::require_true(cjson::__utf8::validate_scalar(tw.cbegin(), tw.size()));
    sb::end_test_case();
  }
  {
    sb::test_case("comptime and runtime agree on the same bytes");
    static constexpr u8 good[] = { 0xe4, 0xb8, 0xad, 0xe6, 0x96, 0x87 };
    static constexpr u8 bad[] = { 0xed, 0xa0, 0x80 };
    static_assert(cjson::__utf8::validate_scalar(good, sizeof(good)));
    static_assert(!cjson::__utf8::validate_scalar(bad, sizeof(bad)));
    sb::require_true(cjson::__utf8::validate_scalar(good, sizeof(good)));
    sb::require_false(cjson::__utf8::validate_scalar(bad, sizeof(bad)));
    sb::end_test_case();
  }
  return 1;
}
