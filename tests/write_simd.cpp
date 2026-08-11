//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// writer kernel identity: the machine esc_mask32/copy32 must agree with the scalar twin
// byte-for-byte, at every alignment, over every byte value. the twin is the oracle
// (CLAUDE.md rule 4) and is also what runs under constant evaluation, so a divergence
// here is a comptime-vs-runtime divergence in ct::write().

#include "../src/cjson/write_simd.hpp"
#include "../src/cjson/cjson.hpp"

#include "tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/types.hpp>

namespace
{

namespace ws = cjson::__wscan;

// the machine kernel, forced past the `if !consteval` gate
template<bool AsciiOnly>
u32
kernel(const u8 *p) noexcept
{
  return ws::esc_mask32<AsciiOnly>(p);
}

u64 rng_state = 0x243f6a8885a308d3ull;

u64
rng() noexcept
{
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}

};      // namespace

int
main()
{
  {
    sb::test_case("esc_mask32 matches the scalar twin for every byte at every position");
    // 32 buffers, each with one distinguished byte value at one position, over all 256
    // values x all 32 positions x 8 alignments
    alignas(64) u8 buf[128];
    bool ok = true;
    for ( u32 align = 0; align < 8 and ok; ++align ) {
      u8 *p = buf + align;
      for ( u32 v = 0; v < 256 and ok; ++v ) {
        for ( u32 pos = 0; pos < 32 and ok; ++pos ) {
          for ( u32 i = 0; i < 32; ++i ) p[i] = u8('a');
          p[pos] = u8(v);
          ok = ok and kernel<false>(p) == ws::esc_mask32_scalar<false>(p);
          ok = ok and kernel<true>(p) == ws::esc_mask32_scalar<true>(p);
        }
      }
    }
    sb::require_true(ok);
    sb::end_test_case();
  }
  {
    sb::test_case("esc_mask32 matches the twin on dense random blocks");
    alignas(64) u8 buf[128];
    bool ok = true;
    for ( u32 trial = 0; trial < 20000 and ok; ++trial ) {
      const u32 align = trial & 7;
      u8 *p = buf + align;
      for ( u32 i = 0; i < 32; ++i ) {
        const u64 r = rng();
        // bias hard toward the interesting bytes: controls, quote, backslash, high bytes
        switch ( r & 3 ) {
        case 0:
          p[i] = u8(r >> 8);
          break;
        case 1:
          p[i] = u8((r >> 8) & 0x1f);
          break;
        case 2:
          p[i] = (r & 4) ? u8('"') : u8('\\');
          break;
        default:
          p[i] = u8(0x20 + ((r >> 8) % 95));
          break;
        }
      }
      ok = ok and kernel<false>(p) == ws::esc_mask32_scalar<false>(p);
      ok = ok and kernel<true>(p) == ws::esc_mask32_scalar<true>(p);
    }
    sb::require_true(ok);
    sb::end_test_case();
  }
  {
    sb::test_case("esc_mask32 agrees with needs_escape() one bit at a time");
    alignas(64) u8 buf[64];
    bool ok = true;
    for ( u32 v = 0; v < 256; ++v ) {
      for ( u32 i = 0; i < 32; ++i ) buf[i] = u8(v);
      const u32 want = ws::needs_escape(u8(v), false) ? 0xffffffffu : 0u;
      const u32 want_a = ws::needs_escape(u8(v), true) ? 0xffffffffu : 0u;
      ok = ok and kernel<false>(buf) == want;
      ok = ok and kernel<true>(buf) == want_a;
    }
    sb::require_true(ok);
    // the exact escape set: control | '"' | '\\', and nothing else
    for ( u32 v = 0; v < 256; ++v ) {
      const bool want = (v < 0x20) or v == u32('"') or v == u32('\\');
      ok = ok and ws::needs_escape(u8(v), false) == want;
      ok = ok and ws::needs_escape(u8(v), true) == (want or v >= 0x80);
    }
    sb::require_true(ok);
    sb::end_test_case();
  }
  {
    sb::test_case("copy32 and copy_plain move bytes exactly");
    alignas(64) u8 src[512];
    alignas(64) u8 dst[512];
    for ( u32 i = 0; i < 512; ++i ) src[i] = u8(i * 7 + 1);
    bool ok = true;
    for ( u32 align = 0; align < 8 and ok; ++align ) {
      for ( u32 i = 0; i < 512; ++i ) dst[i] = 0;
      ws::copy32(dst + align, src + align);
      for ( u32 i = 0; i < 32; ++i ) ok = ok and dst[align + i] == src[align + i];
      ok = ok and dst[align + 32] == 0;      // no over-store
    }
    sb::require_true(ok);

    // copy_plain over every length that crosses the 32-byte block seam
    for ( usize n = 0; n <= 200 and ok; ++n ) {
      for ( u32 align = 0; align < 4 and ok; ++align ) {
        for ( u32 i = 0; i < 512; ++i ) dst[i] = 0xee;
        ws::copy_plain(dst + align, src, n);
        for ( usize i = 0; i < n; ++i ) ok = ok and dst[align + i] == src[i];
        ok = ok and dst[align + n] == 0xee;      // exact tail, nothing past the end
      }
    }
    sb::require_true(ok);
    sb::end_test_case();
  }
  {
    sb::test_case("kernels are identical under constant evaluation");
    // the scalar twin IS the constexpr path; prove it produces the documented masks
    constexpr auto ct = []() consteval -> bool {
      u8 b[32]{};
      for ( u32 i = 0; i < 32; ++i ) b[i] = u8('x');
      if ( ws::esc_mask32<false>(b) != 0 ) return false;
      b[0] = u8('"');
      b[5] = u8('\\');
      b[31] = 0x01;
      if ( ws::esc_mask32<false>(b) != (1u | (1u << 5) | (1u << 31)) ) return false;
      u8 d[32]{};
      ws::copy32(d, b);
      for ( u32 i = 0; i < 32; ++i )
        if ( d[i] != b[i] ) return false;
      u8 e[40]{};
      ws::copy_plain(e, b, 32);
      for ( u32 i = 0; i < 32; ++i )
        if ( e[i] != b[i] ) return false;
      return e[32] == 0;
    };
    static_assert(ct());
    sb::require_true(true);
    sb::end_test_case();
  }
  {
    sb::test_case("escaped strings round-trip through the block writer at every length");
    // lengths either side of the 32-byte seam, with the offender walked across it
    bool ok = true;
    for ( usize n = 1; n <= 96 and ok; ++n ) {
      for ( usize pos = 0; pos < n and ok; ++pos ) {
        micron::string j;
        j.reserve(n * 8 + 8);
        j += "\"";
        for ( usize i = 0; i < n; ++i ) j += (i == pos) ? "\\n" : "a";
        j += "\"";
        auto r = cjson::parse(j.c_str(), j.size());
        ok = ok and r.is_first();
        if ( !ok ) break;
        micron::string out = cjson::write_str(r.cast<cjson::doc>());
        ok = ok and out.size() == j.size();
        for ( usize i = 0; i < out.size() and ok; ++i ) ok = ok and out[i] == j[i];
      }
    }
    sb::require_true(ok);
    sb::end_test_case();
  }
  return 1;
}
