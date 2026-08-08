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

#include "error.hpp"
#include "simd.hpp"
#include "utf8.hpp"

#include <micron/types.hpp>

namespace cjson::__scan
{

// cross-block carries
struct scan_state {
  u64 prev_in_string = 0;       // all-ones while inside a string
  u64 next_is_escaped = 0;      // bit 0: first byte of next block is escaped
  u64 prev_scalar = 0;          // bit 0: last byte of prev block was a nonquote scalar
  u64 ctrl_err = 0;             // any unescaped control char inside a string
};

struct sbits {
  u64 structurals;
  u64 in_str;
};

// WARNING: not always_inline; minify.hpp calls this directly
// force-inlining increases cycles by +2..+8% cyc
constexpr sbits
structural_bits_ex(const block_masks &m, scan_state &st) noexcept
{
  const auto esc = resolve_escapes(m.bs, st.next_is_escaped);
  const u64 quote = m.quote & ~esc.escaped;
  const u64 in_str = prefix_xor(quote) ^ st.prev_in_string;
  st.prev_in_string = u64(i64(in_str) >> 63);
  const u64 string_tail = in_str ^ quote;      // interior + close quote

  const u64 scalar = ~(m.op | m.ws);
  const u64 nonquote_scalar = scalar & ~quote;
  const u64 follows = (nonquote_scalar << 1) | st.prev_scalar;
  st.prev_scalar = nonquote_scalar >> 63;
  const u64 potential = m.op | (scalar & ~follows);

  st.ctrl_err |= m.ctrl & (in_str & ~quote);

  return { (potential & ~string_tail) | quote | (m.bs & ~esc.escaped & in_str), in_str };
}

[[gnu::always_inline]] constexpr u64
structural_bits(const block_masks &m, scan_state &st) noexcept
{
  return structural_bits_ex(m, st).structurals;
}

constexpr usize
index_slots(usize n) noexcept
{
  return n + 8 + 2;
}

constexpr bool
utf8_valid([[maybe_unused]] utf8_state &u, const u8 *__restrict p, usize len) noexcept
{
#if defined(__micron_arch_x86_any) && defined(__micron_x86_avx2)
  if !consteval {
    return utf8_finish_amd64(u);
  }
#endif
  return __utf8::validate_scalar(p, len);
}

template<bool with_utf8, bool Copy>
constexpr max_t
__index_input(const u8 *__restrict p, usize len, u32 *__restrict idx, [[maybe_unused]] u8 *__restrict out, opts o, bool padded) noexcept
{
  if ( len > 0xffffffffull - 2 ) return fail(error::oom);
  (void)o;
  scan_state st{};
  utf8_state u{};
  usize n = 0;
  usize i = 0;
  u64 lag_bits = 0;
  u32 lag_base = 0;

  if constexpr ( with_utf8 ) {
    u8 head[96];
    for ( u32 k = 0; k < 96; ++k ) head[k] = 0x20;
    const usize hn = len < 64 ? len : 64;
    for ( usize k = 0; k < hn; ++k ) head[32 + k] = p[k];
    if constexpr ( Copy ) {
      for ( usize k = 0; k < hn; ++k ) out[k] = p[k];
    }
    const block_masks m = classify64<with_utf8>(head + 32, u);
    lag_bits = structural_bits(m, st);
    if ( hn < 64 ) lag_bits &= ~u64(0) >> (64 - hn);
    lag_base = 0;
    i = hn;
  }

  // NOTE: 2x unroll of lag one body; do _not_ do block interleaving, clear perf regression
  for ( ; i + 128 <= len; i += 128 ) {
    {
      const block_masks m = classify64<with_utf8>(p + i, u);
      if constexpr ( Copy ) __copy64(out + i, p + i);
      n += flatten64(idx + n, lag_base, lag_bits);
      lag_bits = structural_bits(m, st);
      lag_base = u32(i);
    }
    {
      const block_masks m = classify64<with_utf8>(p + i + 64, u);
      if constexpr ( Copy ) __copy64(out + i + 64, p + i + 64);
      n += flatten64(idx + n, lag_base, lag_bits);
      lag_bits = structural_bits(m, st);
      lag_base = u32(i + 64);
    }
  }
  for ( ; i + 64 <= len; i += 64 ) {
    const block_masks m = classify64<with_utf8>(p + i, u);
    if constexpr ( Copy ) __copy64(out + i, p + i);
    n += flatten64(idx + n, lag_base, lag_bits);
    lag_bits = structural_bits(m, st);
    lag_base = u32(i);
  }
  if ( i < len ) {
    if ( padded and !Copy ) {
      const block_masks m = classify64<with_utf8>(p + i, u);
      n += flatten64(idx + n, lag_base, lag_bits);
      lag_bits = structural_bits(m, st) & (~u64(0) >> (64 - (len - i)));
      lag_base = u32(i);
    } else {
      u8 tail[96];
      for ( u32 k = 0; k < 96; ++k ) tail[k] = 0x20;
      if constexpr ( with_utf8 ) {
        for ( u32 k = 0; k < 32; ++k ) tail[k] = p[i - 32 + k];
      }
      for ( usize k = 0; k + i < len; ++k ) tail[32 + k] = p[i + k];
      if constexpr ( Copy ) {
        for ( usize k = 0; k + i < len; ++k ) out[i + k] = p[i + k];
      }
      const block_masks m = classify64<with_utf8>(tail + 32, u);
      n += flatten64(idx + n, lag_base, lag_bits);
      lag_bits = structural_bits(m, st);
      lag_base = u32(i);
    }
  }
  if constexpr ( Copy ) {
    out[len] = 0;
    __fill_spaces(out + len + 1, padding - 1);
  }
  n += flatten64(idx + n, lag_base, lag_bits);
  if ( st.prev_in_string != 0 ) [[unlikely]]
    return fail(error::bad_string);
  if ( st.ctrl_err != 0 ) [[unlikely]]
    return fail(error::bad_string);
  if constexpr ( with_utf8 ) {
    if ( !utf8_valid(u, p, len) ) [[unlikely]]
      return fail(error::bad_utf8);
  }
  idx[n] = u32(len);
  idx[n + 1] = u32(len);
  return max_t(n);
}

constexpr max_t
index_input(const u8 *__restrict p, usize len, u32 *__restrict idx, opts o, bool padded = false) noexcept
{
  return o.skip_utf8 ? __index_input<false, false>(p, len, idx, nullptr, o, padded)
                     : __index_input<true, false>(p, len, idx, nullptr, o, padded);
}

constexpr max_t
index_input_copy(const u8 *__restrict p, usize len, u32 *__restrict idx, u8 *__restrict out, opts o) noexcept
{
  return o.skip_utf8 ? __index_input<false, true>(p, len, idx, out, o, false) : __index_input<true, true>(p, len, idx, out, o, false);
}

};      // namespace cjson::__scan
