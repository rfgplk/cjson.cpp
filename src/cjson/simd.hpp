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

#include "simd_scalar.hpp"

#include <micron/bits/__arch.hpp>

#if defined(__micron_arch_x86_any)
#include "simd_amd64.hpp"
#elif defined(__micron_arch_arm64)
#include "simd_arm64.hpp"
#elif defined(__micron_arch_arm32)
#include "simd_arm32.hpp"
#endif

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// kernel dispatch

namespace cjson::__scan
{

#if defined(__micron_arch_x86_any) && defined(__micron_x86_avx2)
using utf8_state = utf8_state_amd64;
#else
struct utf8_state {
};
#endif

template<bool with_utf8>
[[gnu::always_inline]] constexpr block_masks
classify64(const u8 *p, [[maybe_unused]] utf8_state &u) noexcept
{
#if defined(__micron_arch_x86_any) && defined(__micron_x86_avx2)
  if !consteval {
    return classify_amd64<with_utf8>(p, u);
  }
#elif defined(__micron_arch_arm64) && defined(__micron_arm_neon)
  if !consteval {
    return classify_arm64(p);
  }
#elif defined(__micron_arch_arm32) && defined(__micron_arm_neon)
  if !consteval {
    return classify_arm32(p);
  }
#endif
  return classify_scalar(p);
}

constexpr u64
prefix_xor(u64 x) noexcept
{
#if defined(__micron_arch_x86_any) && defined(__micron_x86_avx2) && defined(__micron_x86_pclmul)
  if !consteval {
    return prefix_xor_amd64(x);
  }
#endif
  return prefix_xor_scalar(x);
}

[[gnu::always_inline]] constexpr u32
flatten64(u32 *tail, u32 base, u64 bits) noexcept
{
#if defined(__micron_arch_x86_any) && defined(__micron_x86_avx2)
  if !consteval {
    return flatten_amd64(tail, base, bits);
  }
#endif
  return flatten_scalar(tail, base, bits);
}

};      // namespace cjson::__scan
