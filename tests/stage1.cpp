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

// stage-1 oracle: an obviously-correct byte-at-a-time reference indexer. the swar sweep
// (and later every simd body) must reproduce its index array bit for bit

namespace
{

// Sf=false: the oracle loops subscript these millions of times; the safety-checked
// operator[] would dominate suite wall time for zero coverage gain
template<typename T> using fastvec = micron::vector<T, micron::allocator_serial<>, false>;

struct ref_out {
  fastvec<u32> idx;
  bool unclosed = false;
  bool ctrl = false;
};

// NOTE: escape resolution is GLOBAL, mirroring the mask algebra (and simdjson): any
// unescaped backslash escapes its next char wherever it appears, and the only effect of
// being escaped is losing quote-activity. classification (ws/op/scalar) is context-free
ref_out
reference_indexes(const u8 *p, usize len)
{
  ref_out o;
  bool in_str = false;
  bool prev_scalar = false;
  bool escaped_next = false;
  usize i = 0;
  while ( i < len ) {
    const u8 c = p[i];
    const bool was_escaped = escaped_next;
    escaped_next = false;
    const bool escape_start = (c == u8('\\')) and !was_escaped;
    if ( escape_start ) escaped_next = true;
    const bool active_quote = (c == u8('"')) and !was_escaped;
    if ( in_str ) {
      if ( active_quote ) {
        o.idx.push_back(static_cast<u32>(i));      // close quote
        in_str = false;
        ++i;
        continue;
      }
      if ( escape_start ) o.idx.push_back(static_cast<u32>(i));      // escape-initiating backslash
      if ( c <= 0x1f ) o.ctrl = true;                                // raw control bytes are illegal in strings, escaped or not
      ++i;
      continue;
    }
    if ( active_quote ) {
      o.idx.push_back(static_cast<u32>(i));      // open quote
      in_str = true;
      prev_scalar = false;
      ++i;
      continue;
    }
    if ( cjson::is_space(c) ) {
      prev_scalar = false;
      ++i;
      continue;
    }
    // 0x0c and 0x1a mirror stage 1's accepted nibble-lut false positives (they
    // op-classify like ',' and ':'); the oracle pins that contract. stage 2 rejects
    // them wherever they land, so index arrays differ only on invalid inputs
    if ( cjson::is_structural(c) or c == 0x0c or c == 0x1a ) {
      o.idx.push_back(static_cast<u32>(i));
      prev_scalar = false;
      ++i;
      continue;
    }
    // scalar byte: stray backslashes and escape-deactivated quotes land here too
    if ( !prev_scalar ) o.idx.push_back(static_cast<u32>(i));
    prev_scalar = true;
    ++i;
  }
  o.unclosed = in_str;
  return o;
}

// run both and compare exactly; inputs the sweep rejects must be rejected by the oracle
// for the same reason (utf-8 is decoupled via skip_utf8 — it has its own suite)
bool
agree(const u8 *p, usize len)
{
  fastvec<u32> buf;
  buf.reserve(cjson::__scan::index_slots(len));
  const max_t r = cjson::__scan::index_input(p, len, buf.begin(), cjson::opts{ .skip_utf8 = true });
  const ref_out ref = reference_indexes(p, len);

  if ( r < 0 ) return ref.unclosed or ref.ctrl;
  if ( ref.unclosed or ref.ctrl ) return false;
  if ( static_cast<usize>(r) != ref.idx.size() ) return false;
  for ( usize i = 0; i < ref.idx.size(); i++ )
    if ( buf.begin()[i] != ref.idx[i] ) return false;
  // sentinels
  return buf.begin()[r] == len and buf.begin()[r + 1] == len;
}

bool
agree_str(const char *s)
{
  return agree(reinterpret_cast<const u8 *>(s), [&] {
    usize n = 0;
    while ( s[n] ) ++n;
    return n;
  }());
}

// the padded fast tail (classify-in-place + bit mask) must be indistinguishable from
// the space-filled scratch tail: same result, same indexes, same sentinels
bool
padded_matches_borrowed(const u8 *bytes, usize n, cjson::opts o)
{
  micron::vector<u8> padded;
  padded.reserve(n + cjson::padding + 8);
  for ( usize i = 0; i < n; i++ ) padded.push_back(bytes[i]);
  padded.push_back(0);
  for ( usize i = 1; i < cjson::padding; i++ ) padded.push_back(0x20);
  fastvec<u32> b1, b2;
  b1.reserve(cjson::__scan::index_slots(n));
  b2.reserve(cjson::__scan::index_slots(n));
  const max_t r1 = cjson::__scan::index_input(padded.cbegin(), n, b1.begin(), o, true);
  const max_t r2 = cjson::__scan::index_input(bytes, n, b2.begin(), o, false);
  if ( r1 != r2 ) return false;
  if ( r1 < 0 ) return true;
  for ( max_t k = 0; k <= r1 + 1; k++ )
    if ( b1.begin()[k] != b2.begin()[k] ) return false;
  return true;
}

// utf-8 seam: the fused simd checker must agree with the scalar decoder byte for
// byte — same accept/reject, same error priority (structural errors first), and
// identical index arrays on accept
bool
utf8_seam_agree(const u8 *p, usize len)
{
  fastvec<u32> b1, b2;
  b1.reserve(cjson::__scan::index_slots(len));
  b2.reserve(cjson::__scan::index_slots(len));
  const max_t rs = cjson::__scan::index_input(p, len, b1.begin(), cjson::opts{ .skip_utf8 = true });
  const max_t rf = cjson::__scan::index_input(p, len, b2.begin(), cjson::opts{});
  const bool scalar_ok = cjson::__utf8::validate_scalar(p, len);
  if ( rs < 0 ) return rf == rs;      // structural errors outrank utf-8 in both paths
  if ( !scalar_ok ) return rf < 0 and cjson::as_error(rf) == cjson::error::bad_utf8;
  if ( rf != rs ) return false;
  for ( max_t k = 0; k <= rs + 1; k++ )
    if ( b1.begin()[k] != b2.begin()[k] ) return false;
  return true;
}

};      // namespace

int
main()
{
  {
    sb::test_case("hand-picked shapes agree with the reference indexer");
    sb::require_true(agree_str("{}"));
    sb::require_true(agree_str("[]"));
    sb::require_true(agree_str("null"));
    sb::require_true(agree_str("  {\"a\":1, \"b\":[true,false,null], \"c\":\"x\"}  "));
    sb::require_true(agree_str("{\"esc\":\"a\\n b\\\\ c\\\" d\\u0041\"}"));
    sb::require_true(agree_str("[1,2.5e-3,-0.125,\"\",{},[]]"));
    sb::require_true(agree_str("\"\\\\\\\\\\\\\""));      // backslash runs ending in a quote
    sb::require_true(agree_str("\"unclosed"));
    sb::require_true(agree_str("\"a\"x"));      // scalar right after a close quote
    sb::require_true(agree_str("tru"));
    sb::require_true(agree_str("\\"));      // stray backslash outside any string
    sb::require_true(agree_str("nul\x01l"));
    sb::end_test_case();
  }
  {
    sb::test_case("quotes backslashes and scalars straddling block boundaries agree");
    // slide a torture motif far enough to cross every seam the block loop can have: the
    // 64B boundaries, the 128B pair seams (128, 256), the utf8-peel-shifted pair seams
    // (192, 320) and the exit from a pair loop into the 64B epilogue
    const char *motif = "{\"k\\\\\":\"v\\\"x\", \"n\":-12.5e+7, \"t\":true}";
    usize mlen = 0;
    while ( motif[mlen] ) ++mlen;
    for ( usize pad = 0; pad < 452; pad++ ) {
      micron::vector<u8> doc;
      for ( usize i = 0; i < pad; i++ ) doc.push_back(u8(' '));
      for ( usize i = 0; i < mlen; i++ ) doc.push_back(u8(motif[i]));
      sb::require_true(agree(doc.cbegin(), doc.size()));
    }
    // backslash runs of every length 1..70 crossing a boundary inside a string, anchored
    // so the run starts just before each seam (61 is the 64B one; the rest are pair seams)
    for ( usize lead : { usize(60), usize(124), usize(188), usize(252), usize(316) } ) {
      for ( usize run = 1; run <= 70; run++ ) {
        micron::vector<u8> doc;
        doc.push_back(u8('"'));
        for ( usize i = 0; i < lead; i++ ) doc.push_back(u8('a'));
        for ( usize i = 0; i < run; i++ ) doc.push_back(u8('\\'));
        doc.push_back(u8('n'));      // escape target for an odd tail
        doc.push_back(u8('"'));
        sb::require_true(agree(doc.cbegin(), doc.size()));
      }
    }
    sb::end_test_case();
  }
  {
    sb::test_case("the sample corpus agrees end to end");
    const char *files[] = { "sample/sample.json", "sample/flowers.json", "sample/64kb.json", "sample/twitter.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      sb::require_true(agree(data.cbegin(), data.size()));
    }
    sb::end_test_case();
  }
  {
    sb::test_case("random ascii soup never desynchronizes the sweep from the oracle");
    tutil::rng rg;
    const char alphabet[] = "{}[]:,\"\\ \t\n0123456789.eE+-truefalsnl\x01\x0c\x1axyz";
    // lengths reach past 320 so the sweep runs at least two full 128B pair iterations
    // under both peels (skip_utf8 starts at 0, the utf8 peel leaves i = 64)
    for ( u32 iter = 0; iter < 4000; iter++ ) {
      micron::vector<u8> doc;
      const u32 n = 1 + rg.below(600);
      for ( u32 i = 0; i < n; i++ ) doc.push_back(u8(alphabet[rg.below(sizeof(alphabet) - 1)]));
      sb::require_true(agree(doc.cbegin(), doc.size()));
    }
    sb::end_test_case();
  }
  {
    sb::test_case("padded and borrowed tail paths produce identical results");
    tutil::rng rg;
    const char alphabet[] = "{}[]:,\"\\ \t\n0123456789.eE+-truefalsnl xyz";
    // bias lengths onto the residues where the block loop hands off: {0,1,63,64,65,127}
    // mod 128, at both peel phases (skip_utf8 enters the pair loop at i=0, the utf8 peel
    // at i=64), and long enough for two full pair iterations before the seam
    static const u32 res[] = { 0, 1, 63, 64, 65, 127 };
    for ( u32 iter = 0; iter < 6000; iter++ ) {
      micron::vector<u8> doc;
      u32 n = 1 + rg.below(600);
      if ( n > 384 ) n = (n & ~127u) + ((iter / 6) & 1 ? 64u : 0u) + res[iter % 6];
      for ( u32 i = 0; i < n; i++ ) doc.push_back(u8(alphabet[rg.below(sizeof(alphabet) - 1)]));
      sb::require_true(padded_matches_borrowed(doc.cbegin(), doc.size(), cjson::opts{ .skip_utf8 = true }));
      sb::require_true(padded_matches_borrowed(doc.cbegin(), doc.size(), cjson::opts{}));
    }
    // inputs that end in the shapes the nul pad could disturb
    sb::require_true(padded_matches_borrowed(reinterpret_cast<const u8 *>("123"), 3, cjson::opts{}));
    sb::require_true(padded_matches_borrowed(reinterpret_cast<const u8 *>("[1,2] "), 6, cjson::opts{}));
    sb::require_true(padded_matches_borrowed(reinterpret_cast<const u8 *>("\"unclosed"), 9, cjson::opts{}));
    sb::require_true(padded_matches_borrowed(reinterpret_cast<const u8 *>("\"a\\"), 3, cjson::opts{}));
    sb::end_test_case();
  }
  {
    sb::test_case("fused utf-8 checker agrees with the scalar decoder");
    // crafted sequences straddling every offset around the 64-byte boundary: valid
    // 2/3/4-byte, truncations, lone continuations, surrogates, overlongs, too-large
    const u8 seqs[][5] = {
      { 0xc2, 0xa2, 0, 0, 2 },            // valid 2-byte
      { 0xe2, 0x82, 0xac, 0, 3 },         // valid 3-byte
      { 0xf0, 0x9f, 0x98, 0x80, 4 },      // valid 4-byte
      { 0xc2, 0, 0, 0, 1 },               // truncated 2-byte
      { 0xe2, 0x82, 0, 0, 2 },            // truncated 3-byte
      { 0xf0, 0x9f, 0x98, 0, 3 },         // truncated 4-byte
      { 0x80, 0, 0, 0, 1 },               // lone continuation
      { 0xed, 0xa0, 0x80, 0, 3 },         // encoded surrogate
      { 0xc0, 0xaf, 0, 0, 2 },            // overlong 2-byte
      { 0xe0, 0x80, 0xaf, 0, 3 },         // overlong 3-byte
      { 0xf0, 0x80, 0x80, 0x80, 4 },      // overlong 4-byte
      { 0xf4, 0x90, 0x80, 0x80, 4 },      // beyond u+10ffff
      { 0xf8, 0x88, 0x80, 0x80, 4 },      // 5-byte lead
    };
    // anchors straddle every seam the block loop has: the 64B boundaries and the 128B
    // pair seams at both peel phases. this is the only test that can catch a broken
    // utf8 `incomplete` latch or a dropped error accumulator across a pair seam
    static const usize anchors[] = { 56, 120, 184, 248, 312, 376 };
    for ( const auto &s : seqs ) {
      const u32 slen = s[4];
      for ( usize a : anchors )
        for ( usize at = a; at <= a + 12; at++ ) {
          micron::vector<u8> doc;
          for ( usize i = 0; i < at; i++ ) doc.push_back(u8('a'));
          for ( u32 i = 0; i < slen; i++ ) doc.push_back(s[i]);
          for ( usize i = 0; i < 8; i++ ) doc.push_back(u8('b'));
          sb::require_true(utf8_seam_agree(doc.cbegin(), doc.size()));
          // and truncated exactly at end-of-input
          micron::vector<u8> cut;
          for ( usize i = 0; i < at; i++ ) cut.push_back(u8('a'));
          for ( u32 i = 0; i < slen; i++ ) cut.push_back(s[i]);
          sb::require_true(utf8_seam_agree(cut.cbegin(), cut.size()));
        }
    }
    // multibyte soup: lead/continuation-biased bytes, many lengths
    tutil::rng rg;
    const u8 pool_bytes[] = { 'a',  'b',  ' ',  '"',  0xc2, 0xa2, 0xe2, 0x82, 0xac, 0xf0, 0x9f, 0x98,
                              0x80, 0xbf, 0xc0, 0xe0, 0xed, 0xf4, 0xf5, 0xff, 0x7f, 0x30, '{',  '}' };
    for ( u32 iter = 0; iter < 4000; iter++ ) {
      micron::vector<u8> doc;
      const u32 n = 1 + rg.below(300);
      for ( u32 i = 0; i < n; i++ ) doc.push_back(pool_bytes[rg.below(sizeof(pool_bytes))]);
      sb::require_true(utf8_seam_agree(doc.cbegin(), doc.size()));
    }
    sb::end_test_case();
  }
  {
    sb::test_case("a lone structural lands correctly at every offset of every seam length");
    // exhaustive, deterministic proof that no block's structurals are dropped, duplicated
    // or emitted out of order across the block-loop / epilogue / tail seams. one '{' at
    // every offset pins the emitted index exactly, and the lengths bracket each seam the
    // 64B loop, a 128B pair loop and both peels can produce
    for ( usize len : { usize(63), usize(64), usize(65), usize(127), usize(128), usize(129), usize(191), usize(192), usize(193), usize(255),
                        usize(256), usize(257), usize(319), usize(320), usize(321), usize(383), usize(384) } ) {
      for ( usize at = 0; at < len; at++ ) {
        micron::vector<u8> doc;
        for ( usize i = 0; i < len; i++ ) doc.push_back(u8(i == at ? '{' : ' '));
        sb::require_true(agree(doc.cbegin(), doc.size()));
        sb::require_true(padded_matches_borrowed(doc.cbegin(), doc.size(), cjson::opts{ .skip_utf8 = true }));
        sb::require_true(padded_matches_borrowed(doc.cbegin(), doc.size(), cjson::opts{}));
      }
    }
    sb::end_test_case();
  }
  return 1;
}
