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

#include <micron/concepts.hpp>
#include <micron/memory/cmemory.hpp>
#include <micron/slice.hpp>
#include <micron/string/strings.hpp>
#include <micron/sum.hpp>
#include <micron/type_traits.hpp>
#include <micron/types.hpp>

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// cjson config

#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
#define CJSON_LITTLE_ENDIAN 1
#else
#define CJSON_LITTLE_ENDIAN 0
#endif

namespace cjson
{
class value;
// hehe
using pun = micron::any<bool, u64, i64, f64, micron::string, const cjson::value *>;

using bytes = micron::raw_slice<const u8>;       // borrowed input view; never mutated or padded
using wbytes = micron::raw_slice<u8>;            // buffer (insitu / _into outputs)
using strv = micron::raw_slice<const char>;      // strings out of getters ({.ptr,.len})
using fjson = micron::slice<u8>;                 // owned byte output

inline constexpr usize padding = 64;

// nesting depth cap; 0 folds the counter away entirely
#ifdef CJSON_DEPTH_LIMIT
inline constexpr u32 depth_limit = CJSON_DEPTH_LIMIT;
#else
inline constexpr u32 depth_limit = 1024;
#endif

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// read options
struct opts {
  bool numbers_as_raw : 1 = false;        // store numbers as raw {ofs,len} tokens, no conversion
  bool skip_utf8 : 1 = false;             // skip utf-8 validation of the input
  bool stop_when_done : 1 = false;        // accept trailing bytes after the first root
  bool relaxed : 1 = false;               // RESERVED TODO: comments + trailing commas via the scalar kernel
  bool with_write_bound : 1 = false;      // accumulate O(1) writer bounds during stage 2 (~6 ins/value;
                                          // without it write_bound falls back to the O(nvals) walks)
};

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// container seam
template<typename C>
concept byte_source = micron::is_iterable_container<micron::remove_cvref_t<C>>
                      && micron::is_trivially_copyable_v<typename micron::remove_cvref_t<C>::value_type>;

// NOTE: for sizeof(value_type) > 1 the byte view is this machine's byte order
template<byte_source C>
inline bytes
as_bytes(const C &c) noexcept
{
  return bytes{ reinterpret_cast<const u8 *>(c.data()), c.size() * sizeof(typename micron::remove_cvref_t<C>::value_type) };
}

template<byte_source C>
inline wbytes
as_wbytes(C &c) noexcept
{
  return wbytes{ reinterpret_cast<u8 *>(c.data()), c.size() * sizeof(typename micron::remove_cvref_t<C>::value_type) };
}

template<typename C>
concept text_source = byte_source<C> || micron::is_string<micron::remove_cvref_t<C>>;

template<typename C>
  requires(micron::is_string<micron::remove_cvref_t<C>> && !byte_source<C>)
inline bytes
as_bytes(const C &c) noexcept
{
  return bytes{ reinterpret_cast<const u8 *>(c.data()), c.size() * sizeof(typename micron::remove_cvref_t<C>::value_type) };
}

// {ptr,len} normalisation
constexpr strv
as_strv(const char *s) noexcept
{
  usize n = 0;
  if ( s )
    while ( s[n] ) ++n;
  return strv{ s, n };
}

constexpr strv
as_strv(strv s) noexcept
{
  return s;
}

template<micron::has_cstr S>
constexpr strv
as_strv(const S &s) noexcept
{
  return strv{ s.c_str(), s.size() };
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// punning loads/stores
// NOTE: loads and stores here are bit named (__load16 reads 2 bytes, __store16 writes 2 bytes)
// czlib names them as bytes (__store16 is a 16 byte op)

constexpr u32
__load16(const u8 *p) noexcept
{
#if CJSON_LITTLE_ENDIAN
  if !consteval {
    u16 v;
    __builtin_memcpy(&v, p, 2);
    return v;
  }
#endif
  return u32(p[0]) | (u32(p[1]) << 8);
}

constexpr u32
__load32(const u8 *p) noexcept
{
#if CJSON_LITTLE_ENDIAN
  if !consteval {
    u32 v;
    __builtin_memcpy(&v, p, 4);
    return v;
  }
#endif
  return u32(p[0]) | (u32(p[1]) << 8) | (u32(p[2]) << 16) | (u32(p[3]) << 24);
}

constexpr u64
__load64(const u8 *p) noexcept
{
#if CJSON_LITTLE_ENDIAN
  if !consteval {
    u64 v;
    __builtin_memcpy(&v, p, 8);
    return v;
  }
#endif
  return u64(p[0]) | (u64(p[1]) << 8) | (u64(p[2]) << 16) | (u64(p[3]) << 24) | (u64(p[4]) << 32) | (u64(p[5]) << 40) | (u64(p[6]) << 48)
         | (u64(p[7]) << 56);
}

constexpr void
__store16(u8 *p, u32 v) noexcept
{
#if CJSON_LITTLE_ENDIAN
  if !consteval {
    u16 t = u16(v);
    __builtin_memcpy(p, &t, 2);
    return;
  }
#endif
  p[0] = u8(v);
  p[1] = u8(v >> 8);
}

constexpr void
__store32(u8 *p, u32 v) noexcept
{
#if CJSON_LITTLE_ENDIAN
  if !consteval {
    __builtin_memcpy(p, &v, 4);
    return;
  }
#endif
  for ( i32 i = 0; i < 4; ++i ) p[i] = u8(v >> (8 * i));
}

constexpr void
__store64(u8 *p, u64 v) noexcept
{
#if CJSON_LITTLE_ENDIAN
  if !consteval {
    __builtin_memcpy(p, &v, 8);
    return;
  }
#endif
  for ( i32 i = 0; i < 8; ++i ) p[i] = u8(v >> (8 * i));
}

constexpr void
__copy(u8 *dst, const u8 *src, usize n) noexcept
{
  micron::memcpy(dst, src, n);
}

constexpr void
__copy16(u8 *dst, const u8 *src) noexcept
{
  if !consteval {
    __builtin_memcpy(dst, src, 16);
    return;
  }
  for ( i32 i = 0; i < 16; ++i ) dst[i] = src[i];
}

// one whole 64-byte block; constant size on purpose: __copy() routes to micron::memcpy,
// which stays out of line
constexpr void
__copy64(u8 *dst, const u8 *src) noexcept
{
  if !consteval {
    __builtin_memcpy(dst, src, 64);
    return;
  }
  for ( i32 i = 0; i < 64; ++i ) dst[i] = src[i];
}

// 0x20 fill
constexpr void
__fill_spaces(u8 *dst, usize n) noexcept
{
  usize i = 0;
  for ( ; i + 8 <= n; i += 8 ) __store64(dst + i, 0x2020202020202020ull);
  for ( ; i < n; ++i ) dst[i] = u8(0x20);
}

// inlinable copy for short runs
constexpr void
__copy_run(u8 *dst, const u8 *src, usize n) noexcept
{
  while ( n >= 16 ) {
    __copy16(dst, src);
    dst += 16;
    src += 16;
    n -= 16;
  }
  if ( n & 8 ) {
    __store64(dst, __load64(src));
    dst += 8;
    src += 8;
  }
  if ( n & 4 ) {
    __store32(dst, __load32(src));
    dst += 4;
    src += 4;
  }
  if ( n & 2 ) {
    __store16(dst, __load16(src));
    dst += 2;
    src += 2;
  }
  if ( n & 1 ) *dst = *src;
}

constexpr void
__move16(u8 *dst, const u8 *src) noexcept
{
  const u64 lo = __load64(src);
  const u64 hi = __load64(src + 8);
  __store64(dst, lo);
  __store64(dst + 8, hi);
}

};      // namespace cjson
