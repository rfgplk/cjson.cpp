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

#if defined(__micron_arch_arm32) && defined(__micron_arm_neon)

#include "write_simd_scalar.hpp"

#include <micron/simd/aliases/neon.hpp>

#include <micron/types.hpp>

namespace cjson::__wscan
{

namespace __ne = micron::simd::neon;

using w128u8 = decltype(__ne::dup_u8(0));

[[gnu::always_inline]] static inline u32
__mm32_neon32(w128u8 m0, w128u8 m1) noexcept
{
  return u32(__ne::movemask_u8(m0)) | (u32(__ne::movemask_u8(m1)) << 16);
}

template<bool AsciiOnly>
[[gnu::always_inline]] static inline u32
esc_mask32_arm32(const u8 *p) noexcept
{
  const w128u8 in0 = __ne::load_u8(p);
  const w128u8 in1 = __ne::load_u8(p + 16);
  const w128u8 x1f = __ne::dup_u8(0x1f);
  const w128u8 quo = __ne::dup_u8('"');
  const w128u8 bs = __ne::dup_u8('\\');

  auto bad = [&](w128u8 in) { return __ne::or_(__ne::or_(__ne::le(in, x1f), __ne::eq(in, quo)), __ne::eq(in, bs)); };
  u32 m = __mm32_neon32(bad(in0), bad(in1));
  if constexpr ( AsciiOnly ) {
    const w128u8 x7f = __ne::dup_u8(0x7f);
    m |= __mm32_neon32(__ne::gt(in0, x7f), __ne::gt(in1, x7f));
  }
  return m;
}

[[gnu::always_inline]] static inline void
copy32_arm32(u8 *dst, const u8 *src) noexcept
{
  __ne::store_u8(dst, __ne::load_u8(src));
  __ne::store_u8(dst + 16, __ne::load_u8(src + 16));
}

};      // namespace cjson::__wscan

#endif
