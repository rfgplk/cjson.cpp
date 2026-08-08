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

#include "doc.hpp"
#include "dtoa.hpp"
#include "error.hpp"
#include "itoa.hpp"
#include "value.hpp"

#include <micron/cmalloc.hpp>
#include <micron/string/strings.hpp>
#include <micron/types.hpp>

namespace cjson
{

struct style {
  u8 indent = 0;                // 0 = minified; 2/4 pretty
  bool ascii_only = false;      // \uXXXX-escape all non-ascii (v1: reserved, not yet applied)
};

};      // namespace cjson

namespace cjson::__write
{

// escape-emitting string writer (decoded bytes in, json text out)
constexpr u8 *
write_string_escaped(u8 *w, const u8 *s, usize n) noexcept
{
  *w++ = u8('"');
  for ( usize i = 0; i < n; ++i ) {
    const u8 c = s[i];
    if ( c == u8('"') or c == u8('\\') ) {
      w[0] = u8('\\');
      w[1] = c;
      w += 2;
    } else if ( c >= 0x20 ) {
      *w++ = c;
    } else {
      // control char: named escape when one exists, else \u00xx
      switch ( c ) {
      case 0x08:
        w[0] = u8('\\');
        w[1] = u8('b');
        w += 2;
        break;
      case 0x09:
        w[0] = u8('\\');
        w[1] = u8('t');
        w += 2;
        break;
      case 0x0a:
        w[0] = u8('\\');
        w[1] = u8('n');
        w += 2;
        break;
      case 0x0c:
        w[0] = u8('\\');
        w[1] = u8('f');
        w += 2;
        break;
      case 0x0d:
        w[0] = u8('\\');
        w[1] = u8('r');
        w += 2;
        break;
      default: {
        w[0] = u8('\\');
        w[1] = u8('u');
        w[2] = u8('0');
        w[3] = u8('0');
        const u8 hi = c >> 4, lo = c & 0xf;
        w[4] = u8(hi < 10 ? '0' + hi : 'a' + hi - 10);
        w[5] = u8(lo < 10 ? '0' + lo : 'a' + lo - 10);
        w += 6;
        break;
      }
      }
    }
  }
  *w++ = u8('"');
  return w;
}

constexpr u8 *
write_scalar(u8 *w, const value &v, const u8 *pool) noexcept
{
  switch ( get_kind(v) ) {
  case kind::string: {
    const u8 *s = pool + v.pay.ofs;
    const u64 n = get_len(v);
    if ( (v.tag & s_mask) == s_noesc ) {
      *w++ = u8('"');
      // don't remove this; bmisses jump 1.5 -> 2.6%, ins -4.5 -> 2.6%, massive mispredict penalties
      if ( n <= 64 ) [[likely]]
        __copy_run(w, s, usize(n));
      else
        __copy(w, s, usize(n));
      w += n;
      *w++ = u8('"');
      return w;
    }
    return write_string_escaped(w, s, usize(n));
  }
  case kind::number: {
    const u64 st = v.tag & s_mask;
    if ( st == s_real ) {
      const u64 raw = __builtin_bit_cast(u64, v.pay.f);
      if ( (raw & 0x7ff0000000000000ull) == 0x7ff0000000000000ull ) {
        // non-finite -> null
        w[0] = u8('n');
        w[1] = u8('u');
        w[2] = u8('l');
        w[3] = u8('l');
        return w + 4;
      }
      return __dtoa::write_f64(w, v.pay.f);
    }
    if ( st == s_sint ) return __itoa::write_i64(w, v.pay.i);
    return __itoa::write_u64(w, v.pay.u);
  }
  case kind::boolean:
    if ( (v.tag & s_mask) == s_true ) {
      w[0] = u8('t');
      w[1] = u8('r');
      w[2] = u8('u');
      w[3] = u8('e');
      return w + 4;
    }
    w[0] = u8('f');
    w[1] = u8('a');
    w[2] = u8('l');
    w[3] = u8('s');
    w[4] = u8('e');
    return w + 5;
  case kind::null:
  default:
    w[0] = u8('n');
    w[1] = u8('u');
    w[2] = u8('l');
    w[3] = u8('l');
    return w + 4;
  }
}

constexpr usize
bound_slots(const value *vals, usize nvals) noexcept
{
  usize b = 32;      // root slack: the walk's transient closer+comma tail included
  for ( usize i = 0; i < nvals; ++i ) {
    const value &v = vals[i];
    switch ( get_kind(v) ) {
    case kind::string:
      b += (v.tag & s_mask) == s_noesc ? usize(get_len(v)) + 4 : usize(get_len(v)) * 6 + 4;
      break;
    case kind::number:
      b += 42;
      break;
    default:
      b += 12;      // literals and containers, incl. the transient trailing separator
      break;
    }
  }
  return b;
}

constexpr usize
pretty_extra(const value *root, u8 indent) noexcept
{
  if ( indent == 0 ) return 0;
  usize extra = 0;
  u32 depth = 0;
  const value *cur = root;
  const value *done = get_next(root);
  constexpr u32 wdepth = depth_limit ? depth_limit : 1024;
  const value *ends[wdepth];
  while ( cur != done or depth != 0 ) {
    if ( is_ctn(*cur) and get_len(*cur) != 0 ) {
      ends[depth++] = get_next(cur);
      extra += 1 + usize(depth) * indent + 2;      // opener line + ": " slack
      cur = cur + 1;
      continue;
    }
    extra += 1 + usize(depth) * indent + 2;
    cur = get_next(cur);
    while ( depth != 0 and cur == ends[depth - 1] ) {
      --depth;
      extra += 1 + usize(depth) * indent;      // closer line
    }
    if ( depth == 0 ) break;
  }
  return extra + 8;
}

struct frame {
  const value *end;
  bool obj;
  bool key_next;
};

inline constexpr u8 __spaces[64]
    = { 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
        0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
        0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 };

template<bool Pretty>
constexpr u8 *
emit_t(u8 *w, const value *root, const u8 *pool, u8 indent) noexcept
{
  constexpr u32 wdepth = depth_limit ? depth_limit : 1024;
  frame stack[wdepth];
  u32 sp = 0;
  const value *t_end = nullptr;
  bool t_obj = false;
  bool t_key = false;
  const value *cur = root;
  const value *done = get_next(root);

  auto newline_indent = [&](u8 *p, u32 depth) constexpr noexcept -> u8 * {
    *p++ = u8('\n');
    usize left = usize(depth) * indent;
    while ( left != 0 ) {
      const usize c = left < 64 ? left : 64;
      __copy(p, __spaces, c);
      p += c;
      left -= c;
    }
    return p;
  };

  for ( ;; ) {
    const bool is_key = sp != 0 and t_obj and t_key;
    if ( sp != 0 ) t_key = t_obj and !t_key;
    if constexpr ( Pretty ) {
      if ( sp != 0 and (is_key or !t_obj) ) w = newline_indent(w, sp);
    }

    if ( is_ctn(*cur) and get_len(*cur) != 0 ) {
      const bool obj = get_kind(*cur) == kind::object;
      *w++ = u8('[') | (u8(obj) << 5);
      if ( sp != 0 ) stack[sp - 1].key_next = t_key;      // sync the only mutable field
      stack[sp] = frame{ get_next(cur), obj, obj };
      ++sp;
      t_end = get_next(cur);
      t_obj = obj;
      t_key = obj;
      cur = cur + 1;
      continue;
    }
    if ( is_ctn(*cur) ) {
      // empty container
      const u8 open = u8('[') | (u8(get_kind(*cur) == kind::object) << 5);
      w[0] = open;
      w[1] = open + 2;      // ']' = '[' + 2, '}' = '{' + 2
      w[2] = u8(',');
      w += 3;
      cur = get_next(cur);
    } else {
      w = write_scalar(w, *cur, pool);
      *w++ = is_key ? u8(':') : u8(',');
      if constexpr ( Pretty ) {
        if ( is_key ) *w++ = u8(' ');
      }
      cur = cur + 1;
    }

    while ( sp != 0 and cur == t_end ) {
      --w;      // erase the trailing comma
      const bool obj = t_obj;
      --sp;
      if ( sp != 0 ) {
        t_end = stack[sp - 1].end;
        t_obj = stack[sp - 1].obj;
        t_key = stack[sp - 1].key_next;
      }
      if constexpr ( Pretty ) w = newline_indent(w, sp);
      w[0] = u8(']') | (u8(obj) << 5);
      w[1] = u8(',');
      w += 2;
    }
    if ( sp == 0 ) {
      if ( cur == done and w[-1] == u8(',') ) --w;
      return w;
    }
  }
}

constexpr u8 *
emit(u8 *w, const value *root, const u8 *pool, style st) noexcept
{
  return st.indent != 0 ? emit_t<true>(w, root, pool, st.indent) : emit_t<false>(w, root, pool, 0);
}

};      // namespace cjson::__write

namespace cjson
{

constexpr usize
write_bound(const doc &d, style st = {}) noexcept
{
  if ( !d.alive() ) return 0;
  const wbounds wb = d.__wbound_parts();
  if ( wb.flat != 0 ) [[likely]]
    return usize(wb.flat) + (st.indent != 0 ? usize(wb.pretty_c) + usize(st.indent) * usize(wb.pretty_k) : 0);
  return __write::bound_slots(d.root().__raw(), d.size()) + __write::pretty_extra(d.root().__raw(), st.indent);
}

constexpr max_t
write_into(const doc &d, wbytes out, style st = {}) noexcept
{
  if ( !d.alive() ) return fail(error::empty_input);
  if ( out.len < write_bound(d, st) ) return fail(error::short_output);
  u8 *end = __write::emit(out.ptr, d.root().__raw(), d.pool(), st);
  return max_t(end - out.ptr);
}

inline micron::string
write_str(const doc &d, style st = {})
{
  micron::string s{};
  if ( !d.alive() ) return s;
  const usize cap = write_bound(d, st);
  s.reserve(cap + 1);
  u8 *base = reinterpret_cast<u8 *>(s.data());
  u8 *end = __write::emit(base, d.root().__raw(), d.pool(), st);
  s.set_size(usize(end - base));
  return s;
}

inline fjson
write(const doc &d, style st = {})
{
  const usize cap = write_bound(d, st);
  fjson out(fjson::__uninit_t{}, cap ? cap : 1);
  if ( !d.alive() ) {
    out.mark(0);
    return out;
  }
  u8 *end = __write::emit(out.first(), d.root().__raw(), d.pool(), st);
  out.mark(usize(end - out.first()));
  return out;
}

};      // namespace cjson
