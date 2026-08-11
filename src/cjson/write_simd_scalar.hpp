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

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// writer scalar scan kernels

namespace cjson::__wscan
{

// a byte cannot be emitted verbatim inside a json string iff
//   c < 0x20  (control)  |  c == '"'  |  c == '\\'
constexpr bool
needs_escape(u8 c, bool ascii_only) noexcept
{
  return c < 0x20 or c == u8('"') or c == u8('\\') or (ascii_only and c >= 0x80);
}

// bit i set <=> p[i] needs escaping
template<bool AsciiOnly>
constexpr u32
esc_mask32_scalar(const u8 *p) noexcept
{
  u32 m = 0;
  for ( u32 i = 0; i < 32; ++i ) m |= u32(needs_escape(p[i], AsciiOnly)) << i;
  return m;
}

template<bool AsciiOnly>
constexpr u32
esc_mask_tail_scalar(const u8 *p, u32 n) noexcept
{
  u32 m = 0;
  for ( u32 i = 0; i < n; ++i ) m |= u32(needs_escape(p[i], AsciiOnly)) << i;
  return m;
}

constexpr void
copy32_scalar(u8 *dst, const u8 *src) noexcept
{
  for ( u32 i = 0; i < 32; ++i ) dst[i] = src[i];
}

};      // namespace cjson::__wscan
