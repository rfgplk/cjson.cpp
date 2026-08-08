// Copyright (c) 2025 David Lucius Severus
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
// LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#pragma once

#include <micron/bits/__arch.hpp>

#if defined(__micron_arch_x86_any) && defined(__micron_x86_avx2)

#include "simd_scalar.hpp"

#include <micron/simd/aliases/avx2.hpp>
#include <micron/simd/aliases/sse.hpp>
#if defined(__micron_x86_pclmul)
#include <micron/simd/aliases/aes.hpp>
#endif

#include <micron/types.hpp>

namespace cjson::__scan
{

namespace __avx = micron::simd::avx2;

[[gnu::always_inline, gnu::target("avx2")]] static inline u64
__mask2(__m256i lo, __m256i hi) noexcept
{
  return u64(u32(__avx::movemask_i8(lo))) | (u64(u32(__avx::movemask_i8(hi))) << 32);
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// fused utf-8 checker

struct utf8_state_amd64 {
  __m256i err{};
  __m256i incomplete{};
};

[[gnu::always_inline, gnu::target("avx2")]] static inline __m256i
__nib_hi(__m256i v) noexcept
{
  return __avx::and_i256(__avx::shr_i16(v, 4), __avx::set1_i8(0x0f));
}

// lookup4 error classes per byte pair
[[gnu::always_inline, gnu::target("avx2")]] static inline __m256i
__utf8_chunk_err(__m256i input, __m256i prev1, __m256i prev2, __m256i prev3) noexcept
{
  // TOO_SHORT 01
  // TOO_LONG 02
  // OVERLONG_3 04
  // TOO_LARGE 08
  // SURROGATE 10
  // OVERLONG_2 20
  // TOO_LARGE_1000/OVERLONG_4 40
  // TWO_CONTS 80
  // CARRY 83
  const __m256i b1h_lut = _mm256_setr_epi8(2, 2, 2, 2, 2, 2, 2, 2, char(0x80), char(0x80), char(0x80), char(0x80), 0x21, 0x01, 0x15, 0x49,
                                           2, 2, 2, 2, 2, 2, 2, 2, char(0x80), char(0x80), char(0x80), char(0x80), 0x21, 0x01, 0x15, 0x49);
  const __m256i b1l_lut = _mm256_setr_epi8(char(0xe7), char(0xa3), char(0x83), char(0x83), char(0x8b), char(0xcb), char(0xcb), char(0xcb),
                                           char(0xcb), char(0xcb), char(0xcb), char(0xcb), char(0xcb), char(0xdb), char(0xcb), char(0xcb),
                                           char(0xe7), char(0xa3), char(0x83), char(0x83), char(0x8b), char(0xcb), char(0xcb), char(0xcb),
                                           char(0xcb), char(0xcb), char(0xcb), char(0xcb), char(0xcb), char(0xdb), char(0xcb), char(0xcb));
  const __m256i b2h_lut = _mm256_setr_epi8(1, 1, 1, 1, 1, 1, 1, 1, char(0xe6), char(0xae), char(0xba), char(0xba), 1, 1, 1, 1, 1, 1, 1, 1,
                                           1, 1, 1, 1, char(0xe6), char(0xae), char(0xba), char(0xba), 1, 1, 1, 1);
  const __m256i sc = __avx::and_i256(__avx::and_i256(__avx::shuffle_v_i8_256(b1h_lut, __nib_hi(prev1)),
                                                     __avx::shuffle_v_i8_256(b1l_lut, __avx::and_i256(prev1, __avx::set1_i8(0x0f)))),
                                     __avx::shuffle_v_i8_256(b2h_lut, __nib_hi(input)));
  // prev2 >= 0xe0 or prev3 >= 0xf0: this byte must be a 3rd/4th continuation
  const __m256i must23 = __avx::or_i256(__avx::sub_sat_u8(prev2, __avx::set1_i8(0x60)), __avx::sub_sat_u8(prev3, __avx::set1_i8(0x70)));
  const __m256i must23_80 = __avx::and_i256(must23, __avx::set1_i8(char(0x80)));
  return __avx::xor_i256(must23_80, sc);
}

[[gnu::target("avx2")]] static inline bool
utf8_finish_amd64(utf8_state_amd64 &u) noexcept
{
  const __m256i e = __avx::or_i256(u.err, u.incomplete);
  return _mm256_testz_si256(e, e) != 0;
}

template<bool with_utf8>
[[gnu::always_inline, gnu::target("avx2")]] static inline block_masks
classify_amd64(const u8 *p, utf8_state_amd64 &u) noexcept
{
  const __m256i ws_lut = _mm256_setr_epi8(' ', 100, 100, 100, 17, 100, 113, 2, 100, '\t', '\n', 112, 100, '\r', 100, 100, ' ', 100, 100,
                                          100, 17, 100, 113, 2, 100, '\t', '\n', 112, 100, '\r', 100, 100);
  // folds [ ] onto { } and , : match directly. bytes >= 0x80
  const __m256i op_lut
      = _mm256_setr_epi8(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ':', '{', ',', '}', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, ':', '{', ',', '}', 0, 0);
  const __m256i x20 = __avx::set1_i8(0x20);
  const __m256i x1f = __avx::set1_i8(0x1f);
  const __m256i q = __avx::set1_i8('"');
  const __m256i b = __avx::set1_i8('\\');

  const __m256i in0 = __avx::loadu_i256(reinterpret_cast<const __m256i *>(p));
  const __m256i in1 = __avx::loadu_i256(reinterpret_cast<const __m256i *>(p + 32));

  const __m256i ws0 = __avx::cmpeq_i8(__avx::shuffle_v_i8_256(ws_lut, in0), in0);
  const __m256i ws1 = __avx::cmpeq_i8(__avx::shuffle_v_i8_256(ws_lut, in1), in1);
  const __m256i op0 = __avx::cmpeq_i8(__avx::shuffle_v_i8_256(op_lut, in0), __avx::or_i256(in0, x20));
  const __m256i op1 = __avx::cmpeq_i8(__avx::shuffle_v_i8_256(op_lut, in1), __avx::or_i256(in1, x20));
  const __m256i ct0 = __avx::cmpeq_i8(__avx::min_u8(in0, x1f), in0);      // c <= 0x1f
  const __m256i ct1 = __avx::cmpeq_i8(__avx::min_u8(in1, x1f), in1);

  if constexpr ( with_utf8 ) {
    // ascii fast path
    if ( __avx::movemask_i8(__avx::or_i256(in0, in1)) == 0 ) [[likely]] {
      u.err = __avx::or_i256(u.err, u.incomplete);
    } else {
      u.err = __avx::or_i256(u.err, __utf8_chunk_err(in0, __avx::loadu_i256(reinterpret_cast<const __m256i *>(p - 1)),
                                                     __avx::loadu_i256(reinterpret_cast<const __m256i *>(p - 2)),
                                                     __avx::loadu_i256(reinterpret_cast<const __m256i *>(p - 3))));
      u.err = __avx::or_i256(u.err, __utf8_chunk_err(in1, __avx::loadu_i256(reinterpret_cast<const __m256i *>(p + 31)),
                                                     __avx::loadu_i256(reinterpret_cast<const __m256i *>(p + 30)),
                                                     __avx::loadu_i256(reinterpret_cast<const __m256i *>(p + 29))));
      const __m256i eof_max
          = _mm256_setr_epi8(char(0xff), char(0xff), char(0xff), char(0xff), char(0xff), char(0xff), char(0xff), char(0xff), char(0xff),
                             char(0xff), char(0xff), char(0xff), char(0xff), char(0xff), char(0xff), char(0xff), char(0xff), char(0xff),
                             char(0xff), char(0xff), char(0xff), char(0xff), char(0xff), char(0xff), char(0xff), char(0xff), char(0xff),
                             char(0xff), char(0xff), char(0xef), char(0xdf), char(0xbf));
      u.incomplete = __avx::sub_sat_u8(in1, eof_max);
    }
  }

  return block_masks{
    __mask2(ws0, ws1),
    __mask2(op0, op1),
    __mask2(__avx::cmpeq_i8(in0, q), __avx::cmpeq_i8(in1, q)),
    __mask2(__avx::cmpeq_i8(in0, b), __avx::cmpeq_i8(in1, b)),
    __mask2(ct0, ct1),
  };
}

#if defined(__micron_x86_pclmul)
namespace __clm = micron::simd::aes;

[[gnu::always_inline, gnu::target("pclmul,sse2")]] static inline u64
prefix_xor_amd64(u64 x) noexcept
{
  const __m128i ones = micron::simd::sse::splat_i8(char(0xff));
  const __m128i v = _mm_set_epi64x(0, i64(x));
  return u64(_mm_cvtsi128_si64(__clm::clmul_64<0>(v, ones)));
}
#endif

[[gnu::always_inline]] __attribute__((no_sanitize("undefined"))) static inline u32
flatten_amd64(u32 *__restrict tail, u32 base, u64 bits) noexcept
{
  if ( bits == 0 ) return 0;
  const u32 cnt = u32(__builtin_popcountll(bits));
  tail[0] = base + u32(__builtin_ctzll(bits));
  bits &= bits - 1;
  tail[1] = base + u32(__builtin_ctzll(bits));
  bits &= bits - 1;
  tail[2] = base + u32(__builtin_ctzll(bits));
  bits &= bits - 1;
  tail[3] = base + u32(__builtin_ctzll(bits));
  bits &= bits - 1;
  if ( cnt > 4 ) [[unlikely]] {
    tail[4] = base + u32(__builtin_ctzll(bits));
    bits &= bits - 1;
    tail[5] = base + u32(__builtin_ctzll(bits));
    bits &= bits - 1;
    tail[6] = base + u32(__builtin_ctzll(bits));
    bits &= bits - 1;
    tail[7] = base + u32(__builtin_ctzll(bits));
    bits &= bits - 1;
    if ( cnt > 8 ) [[unlikely]] {
      tail[8] = base + u32(__builtin_ctzll(bits));
      bits &= bits - 1;
      tail[9] = base + u32(__builtin_ctzll(bits));
      bits &= bits - 1;
      tail[10] = base + u32(__builtin_ctzll(bits));
      bits &= bits - 1;
      tail[11] = base + u32(__builtin_ctzll(bits));
      bits &= bits - 1;
      if ( cnt > 12 ) [[unlikely]] {
        tail[12] = base + u32(__builtin_ctzll(bits));
        bits &= bits - 1;
        tail[13] = base + u32(__builtin_ctzll(bits));
        bits &= bits - 1;
        tail[14] = base + u32(__builtin_ctzll(bits));
        bits &= bits - 1;
        tail[15] = base + u32(__builtin_ctzll(bits));
        bits &= bits - 1;
        if ( cnt > 16 ) [[unlikely]] {
          u32 i = 16;
          do {
            tail[i] = base + u32(__builtin_ctzll(bits));
            bits &= bits - 1;
            ++i;
          } while ( i < cnt );
        }
      }
    }
  }
  return cnt;
}

};      // namespace cjson::__scan

#endif      // __micron_arch_x86_any && __micron_x86_avx2
