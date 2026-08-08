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
#include "error.hpp"
#include "tables.hpp"

#include <micron/types.hpp>

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// string unescape

namespace cjson::__str
{

constexpr u32
encode_utf8(u32 cp, u8 *dst) noexcept
{
  if ( cp < 0x80 ) {
    dst[0] = u8(cp);
    return 1;
  }
  if ( cp < 0x800 ) {
    dst[0] = u8((cp >> 6) + 192);
    dst[1] = u8((cp & 63) + 128);
    return 2;
  }
  if ( cp < 0x10000 ) {
    dst[0] = u8((cp >> 12) + 224);
    dst[1] = u8(((cp >> 6) & 63) + 128);
    dst[2] = u8((cp & 63) + 128);
    return 3;
  }
  dst[0] = u8((cp >> 18) + 240);
  dst[1] = u8(((cp >> 12) & 63) + 128);
  dst[2] = u8(((cp >> 6) & 63) + 128);
  dst[3] = u8((cp & 63) + 128);
  return 4;
}

// forward block copy, overlap-tolerant for dst < src
constexpr void
copy_fwd(u8 *dst, const u8 *src, usize n) noexcept
{
  while ( n >= 16 ) {
    __move16(dst, src);
    dst += 16;
    src += 16;
    n -= 16;
  }
  for ( usize i = 0; i < n; ++i ) dst[i] = src[i];
}

constexpr max_t
unescape(u8 *pool, usize len, const u32 *idx, max_t n, max_t &k) noexcept
{
  const usize open = idx[k];
  usize dst = open + 1;
  usize src = open + 1;
  max_t j = k + 1;
  for ( ;; ) {
    if ( j >= n ) [[unlikely]]
      return fail(error::bad_string);
    const usize stop = idx[j];
    // clean span up to the next escape or the close quote
    if ( stop > src ) {
      copy_fwd(pool + dst, pool + src, stop - src);
      dst += stop - src;
      src = stop;
    }
    if ( pool[stop] == u8('"') ) {
      k = j + 1;
      const max_t out_len = max_t(dst - (open + 1));
      pool[open + 1 + usize(out_len)] = 0;      // c-string compatibility; always in-bounds (dst <= close)
      return out_len;
    }
    // escape at src
    if ( src + 1 >= len ) [[unlikely]]
      return fail(error::bad_string);
    const u8 e = pool[src + 1];
    const u8 simple = escape_map[e];
    if ( simple != 0 ) {
      pool[dst++] = simple;
      src += 2;
      ++j;
      continue;
    }
    if ( e != u8('u') ) [[unlikely]]
      return fail(error::bad_escape);
    if ( src + 6 > len ) [[unlikely]]
      return fail(error::bad_escape);
    u32 cp = hex4_to_u32(pool + src + 2);
    if ( cp > 0xffffu ) [[unlikely]]
      return fail(error::bad_escape);
    if ( cp >= 0xd800u and cp < 0xdc00u ) {
      // high surrogate: the low half must be the immediately following escape index
      if ( j + 1 >= n or idx[j + 1] != src + 6 ) [[unlikely]]
        return fail(error::bad_surrogate);
      if ( src + 12 > len or pool[src + 7] != u8('u') ) [[unlikely]]
        return fail(error::bad_surrogate);
      const u32 lo = hex4_to_u32(pool + src + 8);
      if ( lo > 0xffffu ) [[unlikely]]
        return fail(error::bad_escape);
      if ( lo < 0xdc00u or lo >= 0xe000u ) [[unlikely]]
        return fail(error::bad_surrogate);
      cp = (((cp - 0xd800u) << 10) | (lo - 0xdc00u)) + 0x10000u;
      dst += encode_utf8(cp, pool + dst);
      src += 12;
      j += 2;
      continue;
    }
    if ( cp >= 0xdc00u and cp < 0xe000u ) [[unlikely]]
      return fail(error::bad_surrogate);
    dst += encode_utf8(cp, pool + dst);
    src += 6;
    ++j;
  }
}

};      // namespace cjson::__str
