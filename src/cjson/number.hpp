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
#include "value.hpp"

#include <micron/types.hpp>

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// number reader
//
//  ..integers   digit accumulation, u64/i64 encode, 20-digit checked edge
//  ..doubles    clinger exact fast path -> eisel-lemire 128-bit path (with the
//              mushtak-lemire second multiply) -> for truncated significands the
//              w/w+1 agreement trick -> exact stack-bigint boundary compare
// swar 8-digit gulps accelerate long fractions;
// grammar is validated as a side effect

namespace cjson::__num
{

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// swar: probe and parse 8 ascii digits at once

constexpr bool
is_eight_digits(u64 v) noexcept
{
  return ((v & 0xf0f0f0f0f0f0f0f0ull) | (((v + 0x0606060606060606ull) & 0xf0f0f0f0f0f0f0f0ull) >> 4)) == 0x3333333333333333ull;
}

constexpr u32
parse_eight_digits(u64 v) noexcept
{
  v = (v & 0x0f0f0f0f0f0f0f0full) * 2561 >> 8;
  v = (v & 0x00ff00ff00ff00ffull) * 6553601 >> 16;
  return u32((v & 0x0000ffff0000ffffull) * 42949672960001ull >> 32);
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// eisel-lemire: sig * 10^power -> correctly rounded f64 bits
// (for exact sig)

constexpr bool
compute_f64(i64 power, u64 sig, f64 &out) noexcept
{
  if ( sig == 0 or power < pow5_min_q ) {
    out = 0.0;
    return true;
  }
  if ( power > pow5_max_q ) return false;
  const i32 lz = __builtin_clzll(sig);
  const u64 w = sig << lz;
  const u128_parts p5 = pow5(i32(power));
  u128 first = u128(w) * p5.hi;
  u64 fhi = u64(first >> 64);
  u64 flo = u64(first);
  if ( (fhi & 0x1ff) == 0x1ff ) {
    const u128 second = u128(w) * p5.lo;
    const u64 shi = u64(second >> 64);
    flo += shi;
    if ( flo < shi ) ++fhi;
  }
  const u64 upperbit = fhi >> 63;
  u64 mantissa = fhi >> (upperbit + 9);
  const i32 exponent = i32(((152170 + 65536) * power) >> 16) + 1024 + 63;
  i32 real_exponent = exponent - (lz + i32(1 ^ upperbit));
  if ( real_exponent <= 0 ) [[unlikely]] {
    // subnormal (or a hard zero)
    if ( -real_exponent + 1 >= 64 ) {
      out = 0.0;
      return true;
    }
    mantissa >>= -real_exponent + 1;
    mantissa += mantissa & 1;
    mantissa >>= 1;
    real_exponent = (mantissa < (u64(1) << 52)) ? 0 : 1;
    out = __builtin_bit_cast(f64, (u64(real_exponent) << 52) | (mantissa & 0x000fffffffffffffull));
    return true;
  }
  // round-ties-to-even guard for exact halves (5^q fits the product window)
  if ( flo <= 1 and power >= -4 and power <= 23 and (mantissa & 3) == 1 ) [[unlikely]] {
    if ( (mantissa << (upperbit + 64 - 53 - 2)) == fhi ) mantissa &= ~u64(1);
  }
  mantissa += mantissa & 1;
  mantissa >>= 1;
  if ( mantissa >= (u64(1) << 53) ) {
    mantissa = u64(1) << 52;
    ++real_exponent;
  }
  mantissa &= ~(u64(1) << 52);
  if ( real_exponent > 2046 ) [[unlikely]]
    return false;
  out = __builtin_bit_cast(f64, (u64(real_exponent) << 52) | mantissa);
  return true;
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// 4096-bit stack bigint comparing the full decimal against the upper
// halfway boundary of the eisel-lemire candidate

struct bigint {
  u32 used = 1;
  u64 limb[64]{};
};

constexpr void
big_mul_small(bigint &a, u64 m) noexcept
{
  u64 carry = 0;
  for ( u32 i = 0; i < a.used; ++i ) {
    const u128 t = u128(a.limb[i]) * m + carry;
    a.limb[i] = u64(t);
    carry = u64(t >> 64);
  }
  if ( carry ) a.limb[a.used++] = carry;
}

constexpr void
big_add_small(bigint &a, u64 v) noexcept
{
  u64 carry = v;
  for ( u32 i = 0; i < a.used and carry; ++i ) {
    const u64 s = a.limb[i] + carry;
    carry = s < carry ? 1 : 0;
    a.limb[i] = s;
  }
  if ( carry ) a.limb[a.used++] = carry;
}

inline constexpr u64 pow10_u64_max = 10000000000000000000ull;      // 10^19

constexpr void
big_mul_pow10(bigint &a, u32 e) noexcept
{
  while ( e >= 19 ) {
    big_mul_small(a, pow10_u64_max);
    e -= 19;
  }
  if ( e ) {
    u64 m = 1;
    for ( u32 i = 0; i < e; ++i ) m *= 10;
    big_mul_small(a, m);
  }
}

constexpr void
big_mul_pow2(bigint &a, u32 e) noexcept
{
  const u32 words = e / 64;
  const u32 bits = e % 64;
  if ( words ) {
    for ( i32 i = i32(a.used) - 1; i >= 0; --i ) a.limb[u32(i) + words] = a.limb[i];
    for ( u32 i = 0; i < words; ++i ) a.limb[i] = 0;
    a.used += words;
  }
  if ( bits ) {
    u64 carry = 0;
    for ( u32 i = 0; i < a.used; ++i ) {
      const u64 nc = a.limb[i] >> (64 - bits);
      a.limb[i] = (a.limb[i] << bits) | carry;
      carry = nc;
    }
    if ( carry ) a.limb[a.used++] = carry;
  }
}

constexpr i32
big_cmp(const bigint &a, const bigint &b) noexcept
{
  if ( a.used != b.used ) return a.used < b.used ? -1 : 1;
  for ( i32 i = i32(a.used) - 1; i >= 0; --i )
    if ( a.limb[i] != b.limb[i] ) return a.limb[i] < b.limb[i] ? -1 : 1;
  return 0;
}

inline constexpr u32 max_big_digits = 768;      // beyond this a sticky digit preserves ordering

// noinline&&cold; bigints are 520 bytes each, would put a 1248 byte frame on every call
[[gnu::noinline, gnu::cold]] constexpr bool
__exact_boundary(const u8 *p, usize len, usize start, i64 exp_lit, f64 cand, f64 &out) noexcept
{
  usize s = start;
  if ( s < len and p[s] == u8('-') ) ++s;
  const usize int_start = s;
  while ( s < len and u32(p[s]) - u32('0') <= 9 ) ++s;
  const usize int_end = s;
  usize frac_start = s, frac_end = s;
  if ( s < len and p[s] == u8('.') ) {
    ++s;
    frac_start = s;
    while ( s < len and u32(p[s]) - u32('0') <= 9 ) ++s;
    frac_end = s;
  }
  bigint full{};
  u32 digits = 0;
  i64 tail_exp = exp_lit;
  bool sticky = false;
  u64 chunk = 0;
  u32 chunk_n = 0;
  bool leading = true;
  for ( u32 part = 0; part < 2; ++part ) {
    const usize a = part == 0 ? int_start : frac_start;
    const usize b = part == 0 ? int_end : frac_end;
    for ( usize s = a; s < b; ++s ) {
      const u64 dg = u64(p[s] - u8('0'));
      if ( part == 1 ) --tail_exp;
      if ( leading and dg == 0 ) continue;
      leading = false;
      if ( digits >= max_big_digits ) {
        sticky = sticky or dg != 0;
        ++tail_exp;
        continue;
      }
      chunk = chunk * 10 + dg;
      if ( ++chunk_n == 19 ) {
        big_mul_pow10(full, 19);
        big_add_small(full, chunk);
        chunk = 0;
        chunk_n = 0;
      }
      ++digits;
    }
  }
  if ( chunk_n ) {
    big_mul_pow10(full, chunk_n);
    big_add_small(full, chunk);
  }
  if ( sticky ) {
    big_mul_pow10(full, 1);
    big_add_small(full, 1);
    --tail_exp;
  }
  // candidate upper boundary
  const u64 raw = __builtin_bit_cast(u64, cand);
  const u64 efield = (raw >> 52) & 0x7ff;
  const u64 mant = efield ? ((raw & 0x000fffffffffffffull) | (u64(1) << 52)) : (raw & 0x000fffffffffffffull);
  const i64 e2 = i64(efield ? efield : 1) - 1075;
  bigint comp{};
  comp.limb[0] = mant * 2 + 1;
  const i64 e_up = e2 - 1;
  if ( tail_exp >= 0 )
    big_mul_pow10(full, u32(tail_exp));
  else
    big_mul_pow10(comp, u32(-tail_exp));
  if ( e_up >= 0 )
    big_mul_pow2(comp, u32(e_up));
  else
    big_mul_pow2(full, u32(-e_up));
  const i32 cmp = big_cmp(full, comp);
  u64 out_raw = raw;
  if ( cmp > 0 )
    out_raw += 1;
  else if ( cmp == 0 )
    out_raw += out_raw & 1;
  if ( ((out_raw >> 52) & 0x7ff) == 0x7ff ) return false;      // rounded up into infinity
  out = __builtin_bit_cast(f64, out_raw);
  return true;
}

constexpr max_t
read_number(const u8 *p, usize len, usize start, value &out) noexcept
{
  usize i = start;
  const bool neg = p[i] == u8('-');
  if ( neg ) ++i;
  if ( i >= len or !is_digit(p[i]) ) [[unlikely]]
    return fail(error::bad_number);

  // 0 | [1-9][0-9]*
  u64 sig = 0;
  u32 sig_digits = 0;
  i64 dropped_int = 0;      // int digits beyond the first 19 significant
  bool truncated = false;
  if ( p[i] == u8('0') ) {
    ++i;
    if ( i < len and is_digit(p[i]) ) [[unlikely]]
      return fail(error::bad_number);      // leading zero
  } else {
    u64 acc = u64(p[i] - u8('0'));
    usize j = i + 1;
    while ( j + 8 <= len and (j - i) + 8 <= 19 ) {
      const u64 v = __load64(p + j);
      if ( !is_eight_digits(v) ) break;
      acc = acc * 100000000ull + parse_eight_digits(v);
      j += 8;
    }
    while ( j < len ) {
      const u32 d = u32(p[j]) - u32('0');
      if ( d > 9 ) break;
      acc = acc * 10 + d;
      ++j;
    }
    const usize nd = j - i;
    if ( nd <= 19 ) [[likely]] {
      sig = acc;
      sig_digits = u32(nd);
    } else {
      for ( usize t = i; t < i + 19; ++t ) sig = sig * 10 + u64(p[t] - u8('0'));
      sig_digits = 19;
      dropped_int = i64(nd - 19);
      for ( usize t = i + 19; t < j; ++t ) {
        if ( p[t] != u8('0') ) {
          truncated = true;
          break;
        }
      }
    }
    i = j;
  }

  // fraction
  i64 exp_adjust = dropped_int;
  bool is_float = false;
  if ( i < len and p[i] == u8('.') ) {
    is_float = true;
    ++i;
    if ( i >= len or !is_digit(p[i]) ) [[unlikely]]
      return fail(error::bad_number);
    if ( sig == 0 ) {
      while ( i < len and p[i] == u8('0') ) {
        --exp_adjust;      // leading fractional zeros
        ++i;
      }
      if ( i < len and u32(p[i]) - u32('0') <= 9 ) {
        sig = u64(p[i] - u8('0'));
        sig_digits = 1;
        --exp_adjust;
        ++i;
      }
    }
    while ( sig != 0 and i + 8 <= len and sig_digits + 8 <= 19 ) {
      const u64 v = __load64(p + i);
      if ( !is_eight_digits(v) ) break;
      sig = sig * 100000000ull + parse_eight_digits(v);
      sig_digits += 8;
      exp_adjust -= 8;
      i += 8;
    }
    while ( i < len ) {
      const u32 d = u32(p[i]) - u32('0');
      if ( d > 9 ) break;
      if ( sig_digits < 19 ) {
        sig = sig * 10 + u64(d);
        ++sig_digits;
        --exp_adjust;
      } else {
        truncated = truncated or d != 0;
      }
      ++i;
    }
  }

  i64 exp_lit = 0;
  if ( i < len and (p[i] | 0x20) == u8('e') ) {
    is_float = true;
    ++i;
    bool eneg = false;
    if ( i < len and (p[i] == u8('+') or p[i] == u8('-')) ) {
      eneg = p[i] == u8('-');
      ++i;
    }
    if ( i >= len or !is_digit(p[i]) ) [[unlikely]]
      return fail(error::bad_number);
    while ( i < len ) {
      const u32 d = u32(p[i]) - u32('0');
      if ( d > 9 ) break;
      if ( exp_lit < 100000000 ) exp_lit = exp_lit * 10 + i64(d);
      ++i;
    }
    if ( eneg ) exp_lit = -exp_lit;
  }
  if ( i < len and !is_num_end(p[i]) ) [[unlikely]]
    return fail(error::bad_number);

  if ( !is_float ) {
    if ( dropped_int == 0 ) {
      if ( neg ) {
        if ( sig <= (u64(1) << 63) ) {
          out.tag = make_tag(kind::number, s_sint, 0);
          out.pay.i = i64(~sig + 1);      // modular u64->i64: exact for int64_min too
          return max_t(i - start);
        }
      } else {
        out.tag = make_tag(kind::number, s_uint, 0);
        out.pay.u = sig;
        return max_t(i - start);
      }
    } else if ( dropped_int == 1 and !neg ) {
      const u64 d20 = u64(p[i - 1] - u8('0'));
      if ( sig < 1844674407370955161ull or (sig == 1844674407370955161ull and d20 <= 5) ) {
        out.tag = make_tag(kind::number, s_uint, 0);
        out.pay.u = sig * 10 + d20;
        return max_t(i - start);
      }
    }
    // integer wider than 64 bits: represented as a double, exactly rounded
  }

  const i64 exp10 = exp_lit + exp_adjust;
  f64 d = 0;

  if ( sig == 0 or exp10 < -342 - 19 ) {
    d = 0.0;
  } else if ( exp10 > 308 ) {
    return fail(error::bad_number);      // guaranteed beyond dbl_max
  } else if ( !truncated and sig < (u64(1) << 53) and exp10 >= -22 and exp10 <= 22 ) {
    // clinger: one exact ieee op
    d = f64(sig);
    d = exp10 < 0 ? d / pow10_exact.v[-exp10] : d * pow10_exact.v[exp10];
  } else if ( !truncated ) {
    if ( !compute_f64(exp10, sig, d) ) return fail(error::bad_number);
  } else {
    // truncated significand: if w and w+1 agree the tail cannot matter
    f64 d1 = 0, d2 = 0;
    const bool o1 = compute_f64(exp10, sig, d1);
    const bool o2 = compute_f64(exp10, sig + 1, d2);
    if ( o1 and o2 and __builtin_bit_cast(u64, d1) == __builtin_bit_cast(u64, d2) ) {
      d = d1;
    } else if ( !o1 ) {
      return fail(error::bad_number);      // even the lower bound is infinite
    } else if ( !__exact_boundary(p, len, start, exp_lit, d1, d) ) [[unlikely]] {
      return fail(error::bad_number);      // rounded up into infinity
    }
  }

  out.tag = make_tag(kind::number, s_real, 0);
  out.pay.f = neg ? -d : d;
  return max_t(i - start);
}

};      // namespace cjson::__num
