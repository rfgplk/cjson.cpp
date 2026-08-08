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

#if defined(__micron_arch_arm64) && defined(__micron_arm_neon)

#include "simd_scalar.hpp"

#include <micron/simd/aliases/neon.hpp>

#include <micron/types.hpp>

namespace cjson::__scan
{

namespace __ne = micron::simd::neon;

using v128u8 = decltype(__ne::dup_u8(0));

[[gnu::always_inline]] static inline u64
__mm64_neon(v128u8 m0, v128u8 m1, v128u8 m2, v128u8 m3) noexcept
{
  return u64(__ne::movemask_u8(m0)) | (u64(__ne::movemask_u8(m1)) << 16) | (u64(__ne::movemask_u8(m2)) << 32)
         | (u64(__ne::movemask_u8(m3)) << 48);
}

inline block_masks
classify_arm64(const u8 *p) noexcept
{
  const v128u8 in0 = __ne::load_u8(p);
  const v128u8 in1 = __ne::load_u8(p + 16);
  const v128u8 in2 = __ne::load_u8(p + 32);
  const v128u8 in3 = __ne::load_u8(p + 48);

  alignas(16) static constexpr u8 t_hi_arr[16] = { 0x09, 0x02, 0x11, 0x02, 0x00, 0x04, 0x00, 0x04, 0, 0, 0, 0, 0, 0, 0, 0 };
  alignas(16) static constexpr u8 t_lo_arr[16] = { 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0x08, 0x0a, 0x04, 0x01, 0x0c, 0x00, 0x00 };
  const v128u8 t_hi = __ne::load_u8(t_hi_arr);
  const v128u8 t_lo = __ne::load_u8(t_lo_arr);
  const v128u8 x0f = __ne::dup_u8(0x0f);
  const v128u8 m_op = __ne::dup_u8(0x07);
  const v128u8 m_ws = __ne::dup_u8(0x18);

  auto cls = [&](v128u8 in) { return __ne::and_(__ne::tbl1_u8(t_hi, __ne::shr_imm_u8<4>(in)), __ne::tbl1_u8(t_lo, __ne::and_(in, x0f))); };
  const v128u8 f0 = cls(in0), f1 = cls(in1), f2 = cls(in2), f3 = cls(in3);

  block_masks m{};
  // vtstq: nonzero-AND test to a lane mask (no micron alias)
  m.ws = __mm64_neon(vtstq_u8(f0, m_ws), vtstq_u8(f1, m_ws), vtstq_u8(f2, m_ws), vtstq_u8(f3, m_ws));
  m.op = __mm64_neon(vtstq_u8(f0, m_op), vtstq_u8(f1, m_op), vtstq_u8(f2, m_op), vtstq_u8(f3, m_op));

  const v128u8 q = __ne::dup_u8('"'), b = __ne::dup_u8('\\'), x1f = __ne::dup_u8(0x1f);
  m.quote = __mm64_neon(__ne::eq(in0, q), __ne::eq(in1, q), __ne::eq(in2, q), __ne::eq(in3, q));
  m.bs = __mm64_neon(__ne::eq(in0, b), __ne::eq(in1, b), __ne::eq(in2, b), __ne::eq(in3, b));
  m.ctrl = __mm64_neon(__ne::le(in0, x1f), __ne::le(in1, x1f), __ne::le(in2, x1f), __ne::le(in3, x1f));
  return m;
}

};      // namespace cjson::__scan

#endif      // __micron_arch_arm64 && __micron_arm_neon
