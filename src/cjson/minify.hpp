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
#include "parse.hpp"

#include <micron/string/strings.hpp>
#include <micron/types.hpp>

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// minify
//
// textual whitespace strip

namespace cjson::__minify
{

constexpr max_t
sweep(const u8 *__restrict p, usize len, u32 *__restrict idx, u8 *__restrict out, usize cap, usize &w) noexcept
{
  namespace ss = cjson::__scan;
  if ( len > 0xffffffffull - 2 ) return fail(error::oom);
  ss::scan_state st{};
  ss::utf8_state uu{};
  usize n = 0;
  usize w2 = 0;
  usize i = 0;

  auto compact = [&](const u8 *src, u64 keep) constexpr noexcept -> bool {
    if ( keep == ~u64(0) ) {
      if ( w2 + 64 > cap ) [[unlikely]]
        return false;
      __copy64(out + w2, src);
      w2 += 64;
      return true;
    }
    while ( keep != 0 ) {
      const u32 s = u32(__builtin_ctzll(keep));
      const u64 run = keep >> s;
      const u32 rl = (~run == 0) ? (64 - s) : u32(__builtin_ctzll(~run));
      if ( w2 + rl > cap ) [[unlikely]]
        return false;
      __copy(out + w2, src + s, rl);
      w2 += rl;
      keep = (s + rl >= 64) ? 0 : keep & (~u64(0) << (s + rl));
    }
    return true;
  };

  for ( ; i + 64 <= len; i += 64 ) {
    const ss::block_masks m = ss::classify64<false>(p + i, uu);
    const ss::sbits sb = ss::structural_bits_ex(m, st);
    n += ss::flatten64(idx + n, u32(i), sb.structurals);
    if ( !compact(p + i, ~m.ws | sb.in_str) ) [[unlikely]]
      return fail(error::short_output);
  }
  if ( i < len ) {
    u8 tail[64];
    for ( u32 k = 0; k < 64; ++k ) tail[k] = 0x20;
    for ( usize k = 0; k + i < len; ++k ) tail[k] = p[i + k];
    const ss::block_masks m = ss::classify64<false>(tail, uu);
    const ss::sbits sb = ss::structural_bits_ex(m, st);
    n += ss::flatten64(idx + n, u32(i), sb.structurals);
    if ( !compact(tail, (~m.ws | sb.in_str) & (~u64(0) >> (64 - (len - i)))) ) [[unlikely]]
      return fail(error::short_output);
  }
  if ( st.prev_in_string != 0 ) [[unlikely]]
    return fail(error::bad_string);
  if ( st.ctrl_err != 0 ) [[unlikely]]
    return fail(error::bad_string);
  idx[n] = u32(len);
  idx[n + 1] = u32(len);
  w = w2;
  return max_t(n);
}

};      // namespace cjson::__minify

namespace cjson
{

constexpr usize
minify_bound(usize n) noexcept
{
  return n;
}

constexpr max_t
minify_into(const u8 *p, usize n, u8 *out, usize cap, opts o, scratch &sc) noexcept
{
  if ( n == 0 ) return fail(error::empty_input);
  if ( !sc.ensure(n) ) [[unlikely]]
    return fail(error::oom);
  usize w = 0;
  const max_t r = __minify::sweep(p, n, sc.idx, out, cap, w);
  if ( r < 0 ) [[unlikely]]
    return r;
  if ( !o.skip_utf8 and !__utf8::validate_scalar(p, n) ) [[unlikely]]
    return fail(error::bad_utf8);
  usize consumed = 0;
  if ( const max_t v = __parse::validate_indexes(p, n, sc.idx, r, o, consumed); v < 0 ) [[unlikely]]
    return v;
  return max_t(w);
}

constexpr max_t
minify_into(const u8 *p, usize n, u8 *out, usize cap, opts o = {}) noexcept
{
  scratch sc{};
  return minify_into(p, n, out, cap, o, sc);
}

constexpr max_t
minify(bytes in, wbytes out, opts o, scratch &sc) noexcept
{
  return minify_into(in.ptr, in.len, out.ptr, out.len, o, sc);
}

constexpr max_t
minify(bytes in, wbytes out, opts o = {}) noexcept
{
  return minify_into(in.ptr, in.len, out.ptr, out.len, o);
}

inline result<fjson>
minify(bytes in, opts o = {})
{
  fjson out(fjson::__uninit_t{}, in.len ? in.len : 1);
  const max_t r = minify_into(in.ptr, in.len, out.first(), in.len, o);
  if ( r < 0 ) return result<fjson>{ micron::tag<error>{}, as_error(r) };
  out.mark(usize(r));
  return result<fjson>{ micron::tag<fjson>{}, micron::move(out) };
}

inline result<micron::string>
minify_str(bytes in, opts o = {})
{
  micron::string s{};
  s.reserve(in.len + 1);
  const max_t r = minify_into(in.ptr, in.len, reinterpret_cast<u8 *>(s.data()), in.len, o);
  if ( r < 0 ) return result<micron::string>{ micron::tag<error>{}, as_error(r) };
  s.set_size(usize(r));
  return result<micron::string>{ micron::tag<micron::string>{}, micron::move(s) };
}

constexpr max_t
minify_into(const char *p, usize n, u8 *out, usize cap, opts o = {}) noexcept
{
  return minify_into(reinterpret_cast<const u8 *>(p), n, out, cap, o);
}

constexpr max_t
minify_into(const char *p, usize n, u8 *out, usize cap, opts o, scratch &sc) noexcept
{
  return minify_into(reinterpret_cast<const u8 *>(p), n, out, cap, o, sc);
}

// (ptr, len) forms
constexpr max_t
minify(const char *p, usize n, wbytes out, opts o = {}) noexcept
{
  return minify_into(p, n, out.ptr, out.len, o);
}

constexpr max_t
minify(const u8 *p, usize n, wbytes out, opts o = {}) noexcept
{
  return minify_into(p, n, out.ptr, out.len, o);
}

inline result<fjson>
minify(const char *p, usize n, opts o = {})
{
  return minify(bytes{ reinterpret_cast<const u8 *>(p), n }, o);
}

inline result<fjson>
minify(const u8 *p, usize n, opts o = {})
{
  return minify(bytes{ p, n }, o);
}

inline result<micron::string>
minify_str(const char *p, usize n, opts o = {})
{
  return minify_str(bytes{ reinterpret_cast<const u8 *>(p), n }, o);
}

inline result<micron::string>
minify_str(const u8 *p, usize n, opts o = {})
{
  return minify_str(bytes{ p, n }, o);
}

constexpr max_t
minify(strv in, wbytes out, opts o = {}) noexcept
{
  return minify_into(in.ptr, in.len, out.ptr, out.len, o);
}

constexpr max_t
minify(strv in, wbytes out, opts o, scratch &sc) noexcept
{
  return minify_into(in.ptr, in.len, out.ptr, out.len, o, sc);
}

inline result<fjson>
minify(strv in, opts o = {})
{
  return minify(bytes{ reinterpret_cast<const u8 *>(in.ptr), in.len }, o);
}

inline result<micron::string>
minify_str(strv in, opts o = {})
{
  return minify_str(bytes{ reinterpret_cast<const u8 *>(in.ptr), in.len }, o);
}

template<text_source C>
inline result<fjson>
minify(const C &in, opts o = {})
{
  return minify(as_bytes(in), o);
}

template<text_source C>
inline result<micron::string>
minify_str(const C &in, opts o = {})
{
  return minify_str(as_bytes(in), o);
}

template<text_source C>
inline max_t
minify(const C &in, wbytes out, opts o = {}) noexcept
{
  const bytes b = as_bytes(in);
  return minify_into(b.ptr, b.len, out.ptr, out.len, o);
}

template<text_source C>
inline max_t
minify(const C &in, wbytes out, opts o, scratch &sc) noexcept
{
  const bytes b = as_bytes(in);
  return minify_into(b.ptr, b.len, out.ptr, out.len, o, sc);
}

};      // namespace cjson
