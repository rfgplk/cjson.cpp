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

#include <micron/types.hpp>

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// utf-8 validation (scalar)

namespace cjson::__utf8
{

// little-endian-value constants for a u32 holding bytes [b0 b1 b2 b3] of the sequence
inline constexpr u32 b2_mask = 0x0000c0e0u, b2_patt = 0x000080c0u, b2_requ = 0x0000001eu;
inline constexpr u32 b3_mask = 0x00c0c0f0u, b3_patt = 0x008080e0u, b3_requ = 0x0000200fu, b3_erro = 0x0000200du;
inline constexpr u32 b4_mask = 0xc0c0c0f8u, b4_patt = 0x808080f0u, b4_requ = 0x00003007u, b4_req1 = 0x00000004u, b4_req2 = 0x00003003u;

constexpr bool
is_seq2(u32 u) noexcept
{
  return (u & b2_mask) == b2_patt and (u & b2_requ) != 0;
}

constexpr bool
is_seq3(u32 u) noexcept
{
  const u32 tmp = u & b3_requ;
  return (u & b3_mask) == b3_patt and tmp != 0 and tmp != b3_erro;
}

constexpr bool
is_seq4(u32 u) noexcept
{
  const u32 tmp = u & b4_requ;
  return (u & b4_mask) == b4_patt and tmp != 0 and ((tmp & b4_req1) == 0 or (tmp & b4_req2) == 0);
}

constexpr u32
load4b(const u8 *p, usize i, usize len) noexcept
{
  if ( i + 4 <= len ) [[likely]]
    return __load32(p + i);
  u32 v = 0;
  for ( usize k = 0; i + k < len; ++k ) v |= u32(p[i + k]) << (8 * k);
  return v;
}

constexpr bool
validate_scalar(const u8 *p, usize len) noexcept
{
  usize i = 0;
  while ( i < len ) {
    // ascii fast path
    if ( p[i] < 0x80 ) {
      ++i;
      while ( i + 8 <= len and (__load64(p + i) & 0x8080808080808080ull) == 0 ) i += 8;
      while ( i < len and p[i] < 0x80 ) ++i;
      continue;
    }
    // non-ascii
    u32 u = load4b(p, i, len);
    const usize start = i;
    while ( is_seq3(u) ) {
      i += 3;
      u = load4b(p, i, len);
    }
    while ( is_seq2(u) ) {
      i += 2;
      u = load4b(p, i, len);
    }
    while ( is_seq4(u) ) {
      i += 4;
      u = load4b(p, i, len);
    }
    if ( i == start and (u & 0x80) != 0 ) return false;
  }
  return true;
}

};      // namespace cjson::__utf8
