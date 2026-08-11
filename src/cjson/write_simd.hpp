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

#include "write_simd_scalar.hpp"

#include <micron/bits/__arch.hpp>

#if defined(__micron_arch_x86_any)
#include "write_simd_amd64.hpp"
#elif defined(__micron_arch_arm64)
#include "write_simd_arm64.hpp"
#elif defined(__micron_arch_arm32)
#include "write_simd_arm32.hpp"
#endif

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// writer kernel dispatch

namespace cjson::__wscan
{

template<bool AsciiOnly>
[[gnu::always_inline]] constexpr u32
esc_mask32(const u8 *p) noexcept
{
#if defined(__micron_arch_x86_any) && defined(__micron_x86_avx2)
  if !consteval {
    return esc_mask32_amd64<AsciiOnly>(p);
  }
#elif defined(__micron_arch_arm64) && defined(__micron_arm_neon)
  if !consteval {
    return esc_mask32_arm64<AsciiOnly>(p);
  }
#elif defined(__micron_arch_arm32) && defined(__micron_arm_neon)
  if !consteval {
    return esc_mask32_arm32<AsciiOnly>(p);
  }
#endif
  return esc_mask32_scalar<AsciiOnly>(p);
}

[[gnu::always_inline]] constexpr void
copy32(u8 *dst, const u8 *src) noexcept
{
#if defined(__micron_arch_x86_any) && defined(__micron_x86_avx2)
  if !consteval {
    copy32_amd64(dst, src);
    return;
  }
#elif defined(__micron_arch_arm64) && defined(__micron_arm_neon)
  if !consteval {
    copy32_arm64(dst, src);
    return;
  }
#elif defined(__micron_arch_arm32) && defined(__micron_arm_neon)
  if !consteval {
    copy32_arm32(dst, src);
    return;
  }
#endif
  copy32_scalar(dst, src);
}

constexpr void
copy_plain(u8 *__restrict dst, const u8 *__restrict src, usize n) noexcept
{
  usize i = 0;
  for ( ; i + 32 <= n; i += 32 ) copy32(dst + i, src + i);
  if ( i < n ) __copy_run(dst + i, src + i, n - i);
}

};      // namespace cjson::__wscan
