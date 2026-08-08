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

#include "config.hpp"
#include "tables.hpp"

#include <micron/types.hpp>

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// integer writer

namespace cjson::__itoa
{

constexpr void
pair(u8 *p, u32 v) noexcept
{
  __store16(p, u32(digit_pairs.v[v * 2]) | (u32(digit_pairs.v[v * 2 + 1]) << 8));
}

// value < 100: writes 1-2 digits
constexpr u8 *
w2(u8 *buf, u32 v) noexcept
{
  const u32 lz = v < 10;
  __store16(buf, u32(digit_pairs.v[v * 2 + lz]) | (u32(digit_pairs.v[v * 2 + lz + 1]) << 8));
  return buf + 2 - lz;
}

// 1..8 digits with leading-zero suppression
constexpr u8 *
w1_8(u8 *buf, u32 v) noexcept
{
  if ( v < 100 ) return w2(buf, v);
  if ( v < 10000 ) {
    const u32 aa = (v * 5243) >> 19;      // v / 100
    const u32 lz = aa < 10;
    __store16(buf, u32(digit_pairs.v[aa * 2 + lz]) | (u32(digit_pairs.v[aa * 2 + lz + 1]) << 8));
    buf -= lz;
    pair(buf + 2, v - aa * 100);
    return buf + 4;
  }
  if ( v < 1000000 ) {
    const u32 aa = u32((u64(v) * 429497) >> 32);      // v / 10000
    const u32 bbcc = v - aa * 10000;
    const u32 bb = (bbcc * 5243) >> 19;
    const u32 lz = aa < 10;
    __store16(buf, u32(digit_pairs.v[aa * 2 + lz]) | (u32(digit_pairs.v[aa * 2 + lz + 1]) << 8));
    buf -= lz;
    pair(buf + 2, bb);
    pair(buf + 4, bbcc - bb * 100);
    return buf + 6;
  }
  {
    const u32 aabb = u32((u64(v) * 109951163) >> 40);      // v / 10000
    const u32 ccdd = v - aabb * 10000;
    const u32 aa = (aabb * 5243) >> 19;
    const u32 cc = (ccdd * 5243) >> 19;
    const u32 lz = aa < 10;
    __store16(buf, u32(digit_pairs.v[aa * 2 + lz]) | (u32(digit_pairs.v[aa * 2 + lz + 1]) << 8));
    buf -= lz;
    pair(buf + 2, aabb - aa * 100);
    pair(buf + 4, cc);
    pair(buf + 6, ccdd - cc * 100);
    return buf + 8;
  }
}

// exactly 8 digits, zero-padded
constexpr u8 *
w8(u8 *buf, u32 v) noexcept
{
  const u32 aabb = u32((u64(v) * 109951163) >> 40);
  const u32 ccdd = v - aabb * 10000;
  const u32 aa = (aabb * 5243) >> 19;
  const u32 cc = (ccdd * 5243) >> 19;
  pair(buf, aa);
  pair(buf + 2, aabb - aa * 100);
  pair(buf + 4, cc);
  pair(buf + 6, ccdd - cc * 100);
  return buf + 8;
}

constexpr u8 *
write_u64(u8 *buf, u64 v) noexcept
{
  if ( v < 100000000ull ) return w1_8(buf, u32(v));
  if ( v < 10000000000000000ull ) {
    const u64 hgh = v / 100000000ull;
    buf = w1_8(buf, u32(hgh));
    return w8(buf, u32(v - hgh * 100000000ull));
  }
  const u64 tmp = v / 100000000ull;
  const u64 low = v - tmp * 100000000ull;
  const u64 hgh = tmp / 100000000ull;
  buf = w1_8(buf, u32(hgh));
  buf = w8(buf, u32(tmp - hgh * 100000000ull));
  return w8(buf, u32(low));
}

constexpr u8 *
write_i64(u8 *buf, i64 v) noexcept
{
  const u64 pos = u64(v);
  const u64 neg = ~pos + 1;
  const bool sign = v < 0;
  *buf = u8('-');      // written unconditionally, kept only when negative
  return write_u64(buf + sign, sign ? neg : pos);
}

};      // namespace cjson::__itoa
