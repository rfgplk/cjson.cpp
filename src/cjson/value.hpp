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

#include <micron/types.hpp>

namespace cjson
{

enum class kind : u8 {
  none = 0,
  raw = 1,
  null = 2,
  boolean = 3,
  number = 4,
  string = 5,
  array = 6,
  object = 7,
};

// tag bit layout: [63:8] length | [7:5] reserved | [4:3] subtype | [2:0] kind
inline constexpr u64 k_mask = 0x07;
inline constexpr u64 s_mask = 0x18;
inline constexpr u32 tag_bit = 8;

// subtypes
inline constexpr u64 s_false = u64(0) << 3;      // boolean
inline constexpr u64 s_true = u64(1) << 3;
inline constexpr u64 s_uint = u64(0) << 3;      // number
inline constexpr u64 s_sint = u64(1) << 3;
inline constexpr u64 s_real = u64(2) << 3;
inline constexpr u64 s_plain = u64(0) << 3;      // string
inline constexpr u64 s_noesc = u64(1) << 3;      // string: no escapes

// NOTE: for efficiency stored like this, later marshalled to any in the porcelain layer
struct value {      // exactly 16 bytes, pod
  u64 tag;

  union payload {
    u64 u;
    i64 i;
    f64 f;
    u64 ofs;      // str/raw: text-pool offset | arr/obj: byte offset to next sibling
  } pay;
};

static_assert(sizeof(value) == 16);

constexpr kind
get_kind(const value &v) noexcept
{
  return static_cast<kind>(v.tag & k_mask);
}

constexpr u64
get_len(const value &v) noexcept
{
  return v.tag >> tag_bit;
}

constexpr bool
is_ctn(const value &v) noexcept
{
  return (v.tag & 6) == 6;      // array(6) and object(7) share bits 1-2
}

constexpr u64
make_tag(kind k, u64 subtype, u64 len) noexcept
{
  return (len << tag_bit) | subtype | static_cast<u64>(k);
}

constexpr const value *
get_next(const value *v) noexcept
{
  const u64 slots = is_ctn(*v) ? (v->pay.ofs / sizeof(value)) : 1;
  return v + slots;
}

constexpr bool
arr_is_flat(const value &v) noexcept
{
  return get_len(v) * sizeof(value) + sizeof(value) == v.pay.ofs;
}

};      // namespace cjson
