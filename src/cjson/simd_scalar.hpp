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

// scalar path, these are used in constexpr contextx

namespace cjson::__scan
{

// one 64-byte block classified into bitmasks (bit i == byte i)
struct block_masks {
  u64 ws = 0;         // \t \n \r space
  u64 op = 0;         // { } [ ] : ,
  u64 quote = 0;      // " (raw, escape-unaware)
  u64 bs = 0;         // backslash
  u64 ctrl = 0;       // bytes <= 0x1f (incl. nul)
};

constexpr block_masks
classify_scalar(const u8 *p) noexcept
{
  block_masks m{};
  for ( u32 i = 0; i < 64; ++i ) {
    const u8 c = p[i];
    const u64 bit = u64(1) << i;
    const u8 cls = char_class[c];
    if ( cls & c_space ) m.ws |= bit;
    if ( (cls & c_struct) or c == 0x0c or c == 0x1a ) m.op |= bit;
    if ( c == u8('"') ) m.quote |= bit;
    if ( c == u8('\\') ) m.bs |= bit;
    if ( c <= 0x1f ) m.ctrl |= bit;
  }
  return m;
}

inline constexpr u64 odd_bits = 0xaaaaaaaaaaaaaaaaull;

struct escaped_and_escape {
  u64 escaped;
  u64 escape;
};

constexpr u64
__next_escape_and_terminal(u64 potential) noexcept
{
  const u64 maybe_escaped = potential << 1;
  const u64 maybe_escaped_and_odd = maybe_escaped | odd_bits;
  const u64 even_series_and_odd = maybe_escaped_and_odd - potential;
  return even_series_and_odd ^ odd_bits;
}

constexpr escaped_and_escape
resolve_escapes(u64 bs, u64 &next_is_escaped) noexcept
{
  if ( bs == 0 ) {
    const u64 escaped = next_is_escaped;
    next_is_escaped = 0;
    return { escaped, 0 };
  }
  const u64 potential = bs & ~next_is_escaped;
  const u64 eat = __next_escape_and_terminal(potential);
  const u64 escaped = eat ^ (bs | next_is_escaped);
  const u64 escape = eat & bs;
  next_is_escaped = escape >> 63;
  return { escaped, escape };
}

// kogge-stone doubling
constexpr u64
prefix_xor_scalar(u64 x) noexcept
{
  x ^= x << 1;
  x ^= x << 2;
  x ^= x << 4;
  x ^= x << 8;
  x ^= x << 16;
  x ^= x << 32;
  return x;
}

constexpr u32
flatten_scalar(u32 *tail, u32 base, u64 bits) noexcept
{
  u32 n = 0;
  while ( bits ) {
    tail[n++] = base + u32(__builtin_ctzll(bits));
    bits &= bits - 1;
  }
  return n;
}

};      // namespace cjson::__scan
