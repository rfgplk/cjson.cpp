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

#include "simd_scalar.hpp"

#include <micron/simd/aliases/neon.hpp>

#include <micron/types.hpp>

namespace cjson::__scan
{

namespace __ne = micron::simd::neon;

using v128u8 = decltype(__ne::dup_u8(0));

[[gnu::always_inline]] static inline u64
__mm64_neon32(v128u8 m0, v128u8 m1, v128u8 m2, v128u8 m3) noexcept
{
  return u64(__ne::movemask_u8(m0)) | (u64(__ne::movemask_u8(m1)) << 16) | (u64(__ne::movemask_u8(m2)) << 32)
         | (u64(__ne::movemask_u8(m3)) << 48);
}

inline block_masks
classify_arm32(const u8 *p) noexcept
{
  const v128u8 in0 = __ne::load_u8(p);
  const v128u8 in1 = __ne::load_u8(p + 16);
  const v128u8 in2 = __ne::load_u8(p + 32);
  const v128u8 in3 = __ne::load_u8(p + 48);

  block_masks m{};

  const v128u8 sp = __ne::dup_u8(' '), tb = __ne::dup_u8('\t'), nl = __ne::dup_u8('\n'), cr = __ne::dup_u8('\r');
  auto ws_of
      = [&](v128u8 in) { return __ne::or_(__ne::or_(__ne::eq(in, sp), __ne::eq(in, tb)), __ne::or_(__ne::eq(in, nl), __ne::eq(in, cr))); };
  m.ws = __mm64_neon32(ws_of(in0), ws_of(in1), ws_of(in2), ws_of(in3));

  const v128u8 c0 = __ne::dup_u8('{'), c1 = __ne::dup_u8('}'), c2 = __ne::dup_u8('['), c3 = __ne::dup_u8(']'), c4 = __ne::dup_u8(':'),
               c5 = __ne::dup_u8(','), c6 = __ne::dup_u8(0x0c), c7 = __ne::dup_u8(0x1a);
  auto op_of = [&](v128u8 in) {
    const v128u8 a = __ne::or_(__ne::eq(in, c0), __ne::eq(in, c1));
    const v128u8 b = __ne::or_(__ne::eq(in, c2), __ne::eq(in, c3));
    const v128u8 c = __ne::or_(__ne::eq(in, c4), __ne::eq(in, c5));
    const v128u8 d = __ne::or_(__ne::eq(in, c6), __ne::eq(in, c7));
    return __ne::or_(__ne::or_(a, b), __ne::or_(c, d));
  };
  m.op = __mm64_neon32(op_of(in0), op_of(in1), op_of(in2), op_of(in3));

  const v128u8 q = __ne::dup_u8('"'), b = __ne::dup_u8('\\'), x1f = __ne::dup_u8(0x1f);
  m.quote = __mm64_neon32(__ne::eq(in0, q), __ne::eq(in1, q), __ne::eq(in2, q), __ne::eq(in3, q));
  m.bs = __mm64_neon32(__ne::eq(in0, b), __ne::eq(in1, b), __ne::eq(in2, b), __ne::eq(in3, b));
  auto ctrl_of = [&](v128u8 in) { return __ne::eq(__ne::min(in, x1f), in); };
  m.ctrl = __mm64_neon32(ctrl_of(in0), ctrl_of(in1), ctrl_of(in2), ctrl_of(in3));
  return m;
}

};      // namespace cjson::__scan

#endif      // __micron_arch_arm32 && __micron_arm_neon
