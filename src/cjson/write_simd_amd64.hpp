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

#include "write_simd_scalar.hpp"

#include <micron/simd/aliases/avx.hpp>
#include <micron/simd/aliases/avx2.hpp>

#include <micron/types.hpp>

namespace cjson::__wscan
{

namespace __avx2 = micron::simd::avx2;
namespace __avx = micron::simd::avx;

template<bool AsciiOnly>
[[gnu::always_inline, gnu::target("avx2")]] static inline u32
esc_mask32_amd64(const u8 *p) noexcept
{
  const __m256i in = __avx2::loadu_i256(reinterpret_cast<const __m256i *>(p));
  // c <= 0x1f  <=>  min_u8(c, 0x1f) == c
  const __m256i ctrl = __avx2::cmpeq_i8(__avx2::min_u8(in, __avx2::set1_i8(0x1f)), in);
  const __m256i quo = __avx2::cmpeq_i8(in, __avx2::set1_i8('"'));
  const __m256i bs = __avx2::cmpeq_i8(in, __avx2::set1_i8('\\'));
  const __m256i bad = __avx2::or_i256(__avx2::or_i256(ctrl, quo), bs);
  u32 m = u32(__avx2::movemask_i8(bad));
  if constexpr ( AsciiOnly ) {
    m |= u32(__avx2::movemask_i8(in));
  }
  return m;
}

[[gnu::always_inline, gnu::target("avx2")]] static inline void
copy32_amd64(u8 *dst, const u8 *src) noexcept
{
  __avx::storeu_i256(reinterpret_cast<__m256i_u *>(dst), __avx2::loadu_i256(reinterpret_cast<const __m256i *>(src)));
}

};      // namespace cjson::__wscan

#endif
