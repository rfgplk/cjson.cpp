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

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// cjson rodata tables

namespace cjson
{

template<usize N> struct tbl {
  u8 v[N]{};

  constexpr u8
  operator[](usize i) const noexcept
  {
    return v[i];
  }
};

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// character classes

inline constexpr u8 c_strsafe = 1 << 0;      // legal verbatim inside a double-quoted string: not " \ ctrl, ascii only
inline constexpr u8 c_space = 1 << 1;        // \t \n \r space
inline constexpr u8 c_digit = 1 << 2;        // 0-9
inline constexpr u8 c_nonzero = 1 << 3;      // 1-9
inline constexpr u8 c_exp = 1 << 4;          // e E
inline constexpr u8 c_dot = 1 << 5;          // .
inline constexpr u8 c_sign = 1 << 6;         // + -
inline constexpr u8 c_struct = 1 << 7;       // { } [ ] : ,

namespace __tables
{

consteval tbl<256>
gen_char_class() noexcept
{
  tbl<256> t{};
  for ( u32 c = 0x20; c < 0x80; ++c ) t.v[c] = c_strsafe;
  t.v[u8('"')] = 0;
  t.v[u8('\\')] = 0;
  t.v[u8('\t')] |= c_space;
  t.v[u8('\n')] |= c_space;
  t.v[u8('\r')] |= c_space;
  t.v[u8(' ')] |= c_space;
  for ( u32 c = '0'; c <= '9'; ++c ) t.v[c] |= c_digit;
  for ( u32 c = '1'; c <= '9'; ++c ) t.v[c] |= c_nonzero;
  t.v[u8('e')] |= c_exp;
  t.v[u8('E')] |= c_exp;
  t.v[u8('.')] |= c_dot;
  t.v[u8('+')] |= c_sign;
  t.v[u8('-')] |= c_sign;
  t.v[u8('{')] |= c_struct;
  t.v[u8('}')] |= c_struct;
  t.v[u8('[')] |= c_struct;
  t.v[u8(']')] |= c_struct;
  t.v[u8(':')] |= c_struct;
  t.v[u8(',')] |= c_struct;
  return t;
}

consteval tbl<256>
gen_num_end() noexcept
{
  tbl<256> t{};
  t.v[u8('\t')] = 1;
  t.v[u8('\n')] = 1;
  t.v[u8('\r')] = 1;
  t.v[u8(' ')] = 1;
  t.v[u8(',')] = 1;
  t.v[u8(':')] = 1;
  t.v[u8('[')] = 1;
  t.v[u8(']')] = 1;
  t.v[u8('{')] = 1;
  t.v[u8('}')] = 1;
  return t;
}

};      // namespace __tables

inline constexpr tbl<256> char_class = __tables::gen_char_class();
inline constexpr tbl<256> num_end = __tables::gen_num_end();

constexpr bool
is_space(u8 c) noexcept
{
  return (char_class[c] & c_space) != 0;
}

constexpr bool
is_digit(u8 c) noexcept
{
  // NOTE: don't do u32(c)-'0' <= 9; -funroll-loops explodes instruction count to +5.5 ins/value
  return (char_class[c] & c_digit) != 0;
}

constexpr bool
is_structural(u8 c) noexcept
{
  return (char_class[c] & c_struct) != 0;
}

constexpr bool
is_str_safe(u8 c) noexcept
{
  return (char_class[c] & c_strsafe) != 0;
}

// [.eE]
constexpr bool
is_fp_char(u8 c) noexcept
{
  return (char_class[c] & (c_dot | c_exp)) != 0;
}

// [0-9.eE+-]
constexpr bool
is_num_char(u8 c) noexcept
{
  return (char_class[c] & (c_digit | c_dot | c_exp | c_sign)) != 0;
}

// WARNING: 0x00 is not (and can't be) an end token; all fns downstream guard via i < len
constexpr bool
is_num_end(u8 c) noexcept
{
  return (char_class[c] & (c_space | c_struct)) != 0;
}

static_assert([] {
  for ( u32 c = 0; c < 256; ++c )
    if ( (num_end[u8(c)] != 0) != is_num_end(u8(c)) ) return false;
  return true;
}());

// '[' or '{' in two ops; '(c & 0xdf) == 0x5b' folds both openers
constexpr bool
is_open(u8 c) noexcept
{
  return (c & 0xdf) == 0x5b;
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// eisel-lemire pow5 table

// parse (eisel-lemire) needs [-342, 308]; write (schubfach) reaches +324 for subnormals
inline constexpr i32 pow5_min_q = -342;
inline constexpr i32 pow5_max_q = 324;

struct u128_parts {
  u64 hi = 0;
  u64 lo = 0;
};

namespace __tables
{

// little-endian u64-limb bignum, sized for 5^342 (~795 bits)
struct bn {
  u64 limb[16]{};
  u32 used = 1;
};

consteval void
bn_shl1(bn &a) noexcept
{
  u64 carry = 0;
  for ( u32 i = 0; i < a.used; ++i ) {
    const u64 nc = a.limb[i] >> 63;
    a.limb[i] = (a.limb[i] << 1) | carry;
    carry = nc;
  }
  if ( carry ) a.limb[a.used++] = carry;
}

consteval void
bn_add(bn &a, const bn &b) noexcept
{
  u64 carry = 0;
  const u32 n = a.used > b.used ? a.used : b.used;
  for ( u32 i = 0; i < n; ++i ) {
    const u64 x = i < a.used ? a.limb[i] : 0;
    const u64 y = i < b.used ? b.limb[i] : 0;
    const u64 s = x + y;
    const u64 c2 = s < x ? 1u : 0u;
    const u64 t = s + carry;
    carry = c2 + (t < s ? 1u : 0u);
    a.limb[i] = t;
  }
  a.used = n;
  if ( carry ) a.limb[a.used++] = carry;
}

consteval void
bn_mul5(bn &a) noexcept
{
  bn four = a;
  bn_shl1(four);
  bn_shl1(four);
  bn_add(four, a);
  a = four;
}

consteval u32
bn_bits(const bn &a) noexcept
{
  u32 b = (a.used - 1) * 64;
  u64 top = a.limb[a.used - 1];
  while ( top ) {
    ++b;
    top >>= 1;
  }
  return b;
}

// a >= b
consteval bool
bn_geq(const bn &a, const bn &b) noexcept
{
  if ( a.used != b.used ) return a.used > b.used;
  for ( i32 i = i32(a.used) - 1; i >= 0; --i )
    if ( a.limb[i] != b.limb[i] ) return a.limb[i] > b.limb[i];
  return true;
}

consteval void
bn_sub(bn &a, const bn &b) noexcept
{
  u64 borrow = 0;
  for ( u32 i = 0; i < a.used; ++i ) {
    const u64 bl = i < b.used ? b.limb[i] : 0;
    const u64 d = a.limb[i] - bl - borrow;
    borrow = (a.limb[i] < bl + borrow) or (bl == 0xffffffffffffffffull and borrow) ? 1 : 0;
    a.limb[i] = d;
  }
  while ( a.used > 1 and a.limb[a.used - 1] == 0 ) --a.used;
}

consteval bn
bn_pow5(u32 k) noexcept
{
  bn a{};
  a.limb[0] = 1;
  for ( u32 i = 0; i < k; ++i ) bn_mul5(a);
  return a;
}

// top 128 bits of a bignum, normalized to [2^127, 2^128), truncated
consteval u128_parts
pow5_from_bn(const bn &a) noexcept
{
  const u32 b = bn_bits(a);
  u64 hi = 0, lo = 0;
  if ( b <= 128 ) {
    hi = a.used > 1 ? a.limb[1] : 0;
    lo = a.limb[0];
    const u32 up = 128 - b;
    if ( up >= 64 ) {
      hi = lo << (up - 64);
      lo = 0;
    } else if ( up != 0 ) {
      hi = (hi << up) | (lo >> (64 - up));
      lo = lo << up;
    }
  } else {
    const u32 drop = b - 128;
    const u32 w = drop / 64, s = drop % 64;
    const u64 l0 = a.limb[w];
    const u64 l1 = w + 1 < a.used ? a.limb[w + 1] : 0;
    const u64 l2 = w + 2 < a.used ? a.limb[w + 2] : 0;
    if ( s == 0 ) {
      hi = l1;
      lo = l0;
    } else {
      lo = (l1 << (64 - s)) | (l0 >> s);
      hi = (l2 << (64 - s)) | (l1 >> s);
    }
  }
  return { hi, lo };
}

// normalized 128bit mantissa of 5^-k via shift-subtract long division of 2^(127 + bitlen(5^k)) by 5^k.
consteval u128_parts
pow5_neg_from(const bn &d, u32 k) noexcept
{
  const u32 s = 127 + bn_bits(d);
  bn rem{};
  rem.limb[0] = 1;
  u64 hi = 0, lo = 0;
  for ( u32 i = 0; i < s; ++i ) {
    bn_shl1(rem);
    u64 bit = 0;
    if ( bn_geq(rem, d) ) {
      bn_sub(rem, d);
      bit = 1;
    }
    hi = (hi << 1) | (lo >> 63);
    lo = (lo << 1) | bit;
  }
  if ( k <= 27 ) {
    // remainder is never zero (2^s / 5^k is never exact), so ceil = floor + 1
    ++lo;
    if ( lo == 0 ) ++hi;
  }
  return { hi, lo };
}

template<i32 First, u32 N> struct pow5_chunk {
  u128_parts v[N];
};

template<i32 First, u32 N>
consteval pow5_chunk<First, N>
gen_pow5_neg() noexcept
{
  pow5_chunk<First, N> c{};
  bn d = bn_pow5(u32(-First));
  for ( u32 i = 0; i < N; ++i ) {
    c.v[i] = pow5_neg_from(d, u32(-(First - i32(i))));
    if ( i + 1 < N ) bn_mul5(d);
  }
  return c;
}

consteval pow5_chunk<0, 325>
gen_pow5_pos() noexcept
{
  pow5_chunk<0, 325> c{};
  bn a{};
  a.limb[0] = 1;
  for ( u32 q = 0; q <= 324; ++q ) {
    c.v[q] = pow5_from_bn(a);
    bn_mul5(a);
  }
  return c;
}

};      // namespace __tables

inline constexpr auto pow5_pos = __tables::gen_pow5_pos();

inline constexpr auto pow5_n1 = __tables::gen_pow5_neg<-1, 43>();
inline constexpr auto pow5_n2 = __tables::gen_pow5_neg<-44, 43>();
inline constexpr auto pow5_n3 = __tables::gen_pow5_neg<-87, 43>();
inline constexpr auto pow5_n4 = __tables::gen_pow5_neg<-130, 43>();
inline constexpr auto pow5_n5 = __tables::gen_pow5_neg<-173, 43>();
inline constexpr auto pow5_n6 = __tables::gen_pow5_neg<-216, 43>();
inline constexpr auto pow5_n7 = __tables::gen_pow5_neg<-259, 43>();
inline constexpr auto pow5_n8 = __tables::gen_pow5_neg<-302, 41>();      // q in [-342, -302]

constexpr u128_parts
pow5(i32 q) noexcept
{
  if ( q >= 0 ) return pow5_pos.v[q];
  const u32 i = u32(-1 - q);
  switch ( i / 43 ) {
  case 0:
    return pow5_n1.v[i % 43];
  case 1:
    return pow5_n2.v[i % 43];
  case 2:
    return pow5_n3.v[i % 43];
  case 3:
    return pow5_n4.v[i % 43];
  case 4:
    return pow5_n5.v[i % 43];
  case 5:
    return pow5_n6.v[i % 43];
  case 6:
    return pow5_n7.v[i % 43];
  default:
    return pow5_n8.v[i % 43];
  }
}

// clinger's exactly-representable powers of ten: 1e0..1e22
namespace __tables
{

consteval auto
gen_pow10_exact() noexcept
{
  struct out {
    f64 v[23];
  } o{};

  f64 x = 1.0;
  for ( u32 i = 0; i <= 22; ++i ) {
    o.v[i] = x;      // every 10^i for i <= 22 is exact in binary64, as is each product
    x *= 10.0;
  }
  return o;
}

};      // namespace __tables

inline constexpr auto pow10_exact = __tables::gen_pow10_exact();

namespace __tables
{

consteval tbl<256>
gen_escape_map() noexcept
{
  tbl<256> t{};
  t.v[u8('"')] = u8('"');
  t.v[u8('\\')] = u8('\\');
  t.v[u8('/')] = u8('/');
  t.v[u8('b')] = 0x08;
  t.v[u8('f')] = 0x0c;
  t.v[u8('n')] = 0x0a;
  t.v[u8('r')] = 0x0d;
  t.v[u8('t')] = 0x09;
  return t;
}

struct hexq {
  u32 v[886];
};

consteval hexq
gen_digit_to_val32() noexcept
{
  hexq t{};
  for ( u32 i = 0; i < 886; ++i ) t.v[i] = 0xffffffffu;
  for ( u32 ofs = 0, shift = 0; ofs <= 630; ofs += 210, shift += 4 ) {
    for ( u32 c = '0'; c <= '9'; ++c ) t.v[ofs + c] = (c - '0') << shift;
    for ( u32 c = 'a'; c <= 'f'; ++c ) t.v[ofs + c] = (c - 'a' + 10) << shift;
    for ( u32 c = 'A'; c <= 'F'; ++c ) t.v[ofs + c] = (c - 'A' + 10) << shift;
  }
  return t;
}

};      // namespace __tables

inline constexpr tbl<256> escape_map = __tables::gen_escape_map();
inline constexpr auto digit_to_val32 = __tables::gen_digit_to_val32();

constexpr u32
hex4_to_u32(const u8 *src) noexcept
{
  return digit_to_val32.v[630 + src[0]] | digit_to_val32.v[420 + src[1]] | digit_to_val32.v[210 + src[2]] | digit_to_val32.v[0 + src[3]];
}

namespace __tables
{

struct pairs200 {
  u8 v[200];
};

consteval pairs200
gen_digit_pairs() noexcept
{
  pairs200 t{};
  for ( u32 i = 0; i < 100; ++i ) {
    t.v[i * 2] = u8('0' + i / 10);
    t.v[i * 2 + 1] = u8('0' + i % 10);
  }
  return t;
}

};      // namespace __tables

inline constexpr auto digit_pairs = __tables::gen_digit_pairs();

// correctness asserts
static_assert(char_class[0x00] == 0);
static_assert(char_class[u8('"')] == 0);
static_assert(char_class[u8('\\')] == 0);
static_assert(char_class[0x1f] == 0);
static_assert(char_class[0x80] == 0);
static_assert(char_class[0xff] == 0);
static_assert(char_class[u8(' ')] == (c_strsafe | c_space));
static_assert(char_class[u8('\t')] == c_space);
static_assert(char_class[u8('\n')] == c_space);
static_assert(char_class[u8('\r')] == c_space);
static_assert(char_class[u8('0')] == (c_strsafe | c_digit));
static_assert(char_class[u8('9')] == (c_strsafe | c_digit | c_nonzero));
static_assert(char_class[u8('e')] == (c_strsafe | c_exp));
static_assert(char_class[u8('E')] == (c_strsafe | c_exp));
static_assert(char_class[u8('.')] == (c_strsafe | c_dot));
static_assert(char_class[u8('-')] == (c_strsafe | c_sign));
static_assert(char_class[u8('+')] == (c_strsafe | c_sign));
static_assert(char_class[u8('{')] == (c_strsafe | c_struct));
static_assert(char_class[u8(',')] == (c_strsafe | c_struct));
static_assert(char_class[u8('a')] == c_strsafe);
static_assert(num_end[u8(' ')] == 1 && num_end[u8(',')] == 1 && num_end[u8('}')] == 1);
static_assert(num_end[0x00] == 0);
static_assert(num_end[u8('0')] == 0 && num_end[u8('e')] == 0 && num_end[u8('.')] == 0 && num_end[u8('"')] == 0);
static_assert(is_open(u8('[')) && is_open(u8('{')) && !is_open(u8(']')) && !is_open(u8('"')));

static_assert(pow5(-342).hi == 0xeef453d6923bd65aull && pow5(-342).lo == 0x113faa2906a13b3full);      // deep: floor
static_assert(pow5(308).hi == 0x8e679c2f5e44ff8full && pow5(308).lo == 0x570f09eaa7ea7648ull);
static_assert(pow5(0).hi == 0x8000000000000000ull && pow5(0).lo == 0);
static_assert(pow5(1).hi == 0xa000000000000000ull && pow5(1).lo == 0);
static_assert(pow5(-1).hi == 0xccccccccccccccccull && pow5(-1).lo == 0xcccccccccccccccdull);      // shallow: ceil
static_assert(pow5(-2).hi == 0xa3d70a3d70a3d70aull && pow5(-2).lo == 0x3d70a3d70a3d70a4ull);
static_assert(pow10_exact.v[0] == 1.0 && pow10_exact.v[22] == 1e22);

static_assert(escape_map[u8('n')] == 0x0a && escape_map[u8('t')] == 0x09 && escape_map[u8('"')] == u8('"'));
static_assert(escape_map[u8('q')] == 0 && escape_map[u8('u')] == 0 && escape_map[0] == 0);
static_assert([] {
  constexpr u8 s1[4] = { '0', '0', '4', '1' };
  constexpr u8 s2[4] = { 'F', 'f', 'F', 'f' };
  constexpr u8 s3[4] = { '0', '0', 'g', '0' };
  return hex4_to_u32(s1) == 0x41u && hex4_to_u32(s2) == 0xffffu && (hex4_to_u32(s3) >> 16) != 0;
}());

};      // namespace cjson
