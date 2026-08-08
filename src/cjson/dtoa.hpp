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
#include "itoa.hpp"
#include "tables.hpp"

#include <micron/types.hpp>

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// schubfach (giulietti 2022)
//
// shortest decimal that roundtrips via three round to odd 196-bit products
// formatting follows ecmascript

namespace cjson::__dtoa
{

// top 64 bits of cp * (g.hi:g.lo) with a sticky low bit, rounded to odd
constexpr u64
round_to_odd(u128_parts g, u64 cp) noexcept
{
  const u128 x = u128(cp) * g.lo;
  const u128 y = u128(cp) * g.hi + u64(x >> 64);
  const u64 y_hi = u64(y >> 64);
  const u64 y_lo = u64(y);
  return y_hi | u64(y_lo > 1);
}

struct dec {
  u64 sig;
  i32 exp;
};

// shortest decimal of sig_bin * 2^exp_bin (finite, nonzero)
constexpr dec
bin_to_dec(u64 sig_raw, i32 exp_raw, u64 sig_bin, i32 exp_bin) noexcept
{
  const bool irregular = sig_raw == 0 and exp_raw > 1;
  const bool even = (sig_bin & 1) == 0;
  const u64 cbl = 4 * sig_bin - 2 + u64(irregular);
  const u64 cb = 4 * sig_bin;
  const u64 cbr = 4 * sig_bin + 2;
  const i32 k = i32((i64(exp_bin) * 315653 - (irregular ? 131237 : 0)) >> 20);
  const i32 h = exp_bin + i32((i64(-k) * 217707) >> 16) + 1;      // h in [1,4]
  u128_parts p10 = pow5(-k);
  // schubfach needs a strictly-greater cache;
  // our negative entries (q in [-27,-1], i.e. k in [1,27]) are already ceild
  if ( k < 1 or k > 27 ) {
    ++p10.lo;
    if ( p10.lo == 0 ) ++p10.hi;
  }
  const u64 vbl = round_to_odd(p10, cbl << h);
  const u64 vb = round_to_odd(p10, cb << h);
  const u64 vbr = round_to_odd(p10, cbr << h);
  const u64 lower = vbl + u64(!even);
  const u64 upper = vbr - u64(!even);
  const u64 s = vb / 4;
  if ( s >= 10 ) {
    const u64 sp = s / 10;
    const bool up_in = lower <= 40 * sp;
    const bool wp_in = upper >= 40 * sp + 40;
    if ( up_in != wp_in ) return { sp * 10 + (wp_in ? 10 : 0), k };
  }
  const bool u_in = lower <= 4 * s;
  const bool w_in = upper >= 4 * s + 4;
  const u64 mid = 4 * s + 2;
  const bool round_up = vb > mid or (vb == mid and (s & 1));
  return { s + ((u_in != w_in) ? u64(w_in) : u64(round_up)), k };
}

constexpr u8 *
write_f64(u8 *buf, f64 val) noexcept
{
  const u64 raw = __builtin_bit_cast(u64, val);
  const bool sign = (raw >> 63) != 0;
  *buf = u8('-');
  buf += sign;
  if ( (raw << 1) == 0 ) {
    buf[0] = u8('0');
    buf[1] = u8('.');
    buf[2] = u8('0');
    return buf + 3;
  }
  const u64 efield = (raw >> 52) & 0x7ff;
  const u64 mfield = raw & 0x000fffffffffffffull;
  u64 sig_bin = 0;
  i32 exp_bin = 0;
  if ( efield ) {
    sig_bin = mfield | (u64(1) << 52);
    exp_bin = i32(efield) - 1075;
  } else {
    sig_bin = mfield;
    exp_bin = -1074;
  }
  // exact small integer: no schubfach, just digits + ".0"
  if ( exp_bin >= -52 and exp_bin <= 0 and i32(u32(__builtin_ctzll(sig_bin))) >= -exp_bin ) {
    buf = __itoa::write_u64(buf, sig_bin >> u32(-exp_bin));
    buf[0] = u8('.');
    buf[1] = u8('0');
    return buf + 2;
  }
  const dec d = bin_to_dec(mfield, i32(efield), sig_bin, exp_bin);
  u8 tmp[24]{};
  u8 *dig_end = __itoa::write_u64(tmp, d.sig);
  const i32 n_full = i32(dig_end - tmp);
  while ( dig_end[-1] == u8('0') ) --dig_end;
  const i32 n = i32(dig_end - tmp);
  const i32 e10 = n_full - 1 + d.exp;      // value = t.tttt * 10^e10

  if ( e10 >= 0 and e10 <= 20 ) {
    if ( n <= e10 + 1 ) {
      // ddd[000].0
      for ( i32 i = 0; i < n; ++i ) *buf++ = tmp[i];
      for ( i32 i = n; i <= e10; ++i ) *buf++ = u8('0');
      buf[0] = u8('.');
      buf[1] = u8('0');
      return buf + 2;
    }
    // dd.ddd
    for ( i32 i = 0; i <= e10; ++i ) *buf++ = tmp[i];
    *buf++ = u8('.');
    for ( i32 i = e10 + 1; i < n; ++i ) *buf++ = tmp[i];
    return buf;
  }
  if ( e10 < 0 and e10 >= -6 ) {
    // 0.000ddd
    *buf++ = u8('0');
    *buf++ = u8('.');
    for ( i32 i = -1; i > e10; --i ) *buf++ = u8('0');
    for ( i32 i = 0; i < n; ++i ) *buf++ = tmp[i];
    return buf;
  }
  // d[.ddd]e[-]EEE
  *buf++ = tmp[0];
  if ( n > 1 ) {
    *buf++ = u8('.');
    for ( i32 i = 1; i < n; ++i ) *buf++ = tmp[i];
  }
  *buf++ = u8('e');
  u32 mag = 0;
  if ( e10 < 0 ) {
    *buf++ = u8('-');
    mag = u32(-e10);
  } else {
    mag = u32(e10);
  }
  return __itoa::w1_8(buf, mag);
}

};      // namespace cjson::__dtoa
