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
#include "error.hpp"
#include "number.hpp"
#include "scratch.hpp"
#include "stage1.hpp"
#include "string.hpp"
#include "value.hpp"

#include <micron/cmalloc.hpp>
#include <micron/type_traits.hpp>
#include <micron/types.hpp>

namespace cjson::__parse
{

constexpr u32
__hex_val(u8 c) noexcept
{
  if ( c >= u8('0') and c <= u8('9') ) return u32(c - u8('0'));
  const u8 l = c | 0x20;
  if ( l >= u8('a') and l <= u8('f') ) return u32(l - u8('a')) + 10;
  return 0xffffffffu;
}

constexpr u32
__hex4(const u8 *p) noexcept
{
  // any invalid nibble poisons the high bits
  return (__hex_val(p[0]) << 12) | (__hex_val(p[1]) << 8) | (__hex_val(p[2]) << 4) | __hex_val(p[3]);
}

constexpr u32
word4(char a, char b, char c, char d) noexcept
{
  return u32(u8(a)) | (u32(u8(b)) << 8) | (u32(u8(c)) << 16) | (u32(u8(d)) << 24);
}

inline constexpr u32 w_true = word4('t', 'r', 'u', 'e');
inline constexpr u32 w_null = word4('n', 'u', 'l', 'l');
inline constexpr u32 w_alse = word4('a', 'l', 's', 'e');

[[gnu::noinline, gnu::cold]] constexpr max_t
__validate_escaped(const u8 *p, usize len, const u32 *idx, max_t n, max_t &k) noexcept
{
  max_t j = k + 1;
  for ( ;; ) {
    if ( j >= n ) [[unlikely]]
      return fail(error::bad_string);
    const usize b = idx[j];
    if ( p[b] == u8('"') ) {
      k = j + 1;
      return 0;
    }
    // an escape-initiating backslash
    if ( b + 1 >= len ) [[unlikely]]
      return fail(error::bad_string);
    const u8 e = p[b + 1];
    if ( e == u8('"') or e == u8('\\') or e == u8('/') or e == u8('b') or e == u8('f') or e == u8('n') or e == u8('r') or e == u8('t') ) {
      ++j;
      continue;
    }
    if ( e != u8('u') ) [[unlikely]]
      return fail(error::bad_escape);
    if ( b + 6 > len ) [[unlikely]]
      return fail(error::bad_escape);
    const u32 hi = __hex4(p + b + 2);
    if ( hi > 0xffffu ) [[unlikely]]
      return fail(error::bad_escape);
    if ( hi >= 0xd800u and hi < 0xdc00u ) {
      if ( j + 1 >= n or idx[j + 1] != b + 6 ) return fail(error::bad_surrogate);
      if ( b + 12 > len or p[b + 7] != u8('u') ) return fail(error::bad_surrogate);
      const u32 lo = __hex4(p + b + 8);
      if ( lo > 0xffffu ) return fail(error::bad_escape);
      if ( lo < 0xdc00u or lo >= 0xe000u ) return fail(error::bad_surrogate);
      j += 2;
      continue;
    }
    if ( hi >= 0xdc00u and hi < 0xe000u ) [[unlikely]]
      return fail(error::bad_surrogate);
    ++j;
  }
}

[[gnu::always_inline]] constexpr max_t
validate_string(const u8 *p, usize len, const u32 *idx, max_t n, max_t &k) noexcept
{
  const max_t j = k + 1;
  if ( j < n and p[idx[j]] == u8('"') ) [[likely]] {
    k = j + 1;
    return 0;
  }
  return __validate_escaped(p, len, idx, n, k);
}

constexpr max_t
validate_number(const u8 *p, usize len, usize i) noexcept
{
  if ( p[i] == u8('-') ) ++i;
  if ( i >= len ) [[unlikely]]
    return fail(error::bad_number);
  if ( p[i] == u8('0') ) {
    ++i;
  } else if ( is_digit(p[i]) ) {
    ++i;
    while ( i < len and is_digit(p[i]) ) ++i;
  } else {
    return fail(error::bad_number);
  }
  if ( i < len and p[i] == u8('.') ) {
    ++i;
    if ( i >= len or !is_digit(p[i]) ) return fail(error::bad_number);
    while ( i < len and is_digit(p[i]) ) ++i;
  }
  if ( i < len and (char_class[p[i]] & c_exp) != 0 ) {
    ++i;
    if ( i < len and (p[i] == u8('+') or p[i] == u8('-')) ) ++i;
    if ( i >= len or !is_digit(p[i]) ) return fail(error::bad_number);
    while ( i < len and is_digit(p[i]) ) ++i;
  }
  if ( i < len and !is_num_end(p[i]) ) return fail(error::bad_number);
  return 0;
}

constexpr max_t
validate_literal(const u8 *p, usize len, usize i) noexcept
{
  const u8 c = p[i];
  if ( c == u8('t') ) {
    if ( i + 4 > len or __load32(p + i) != w_true ) return fail(error::bad_syntax);
    if ( i + 4 < len and !is_num_end(p[i + 4]) ) return fail(error::bad_syntax);
    return 0;
  }
  if ( c == u8('n') ) {
    if ( i + 4 > len or __load32(p + i) != w_null ) return fail(error::bad_syntax);
    if ( i + 4 < len and !is_num_end(p[i + 4]) ) return fail(error::bad_syntax);
    return 0;
  }
  if ( c == u8('f') ) {
    if ( i + 5 > len or __load32(p + i + 1) != w_alse ) return fail(error::bad_syntax);
    if ( i + 5 < len and !is_num_end(p[i + 5]) ) return fail(error::bad_syntax);
    return 0;
  }
  return fail(error::bad_syntax);
}

constexpr max_t
validate_indexes(const u8 *p, usize len, const u32 *idx, const max_t n, opts o, usize &consumed) noexcept
{
  if ( n == 0 ) return fail(error::empty_input);
  constexpr u32 vdepth = depth_limit ? depth_limit : 1024;
  u64 is_arr[(vdepth + 63) / 64]{};
  u32 depth = 0;
  max_t k = 0;

  enum class fs : u8 { value, after, key };

  fs s = fs::value;
  for ( ;; ) {
    switch ( s ) {
    case fs::value: {
      if ( k >= n ) [[unlikely]]
        return fail(error::bad_syntax);
      const u8 c = p[idx[k]];
      if ( c == u8('{') ) {
        if ( k + 1 < n and p[idx[k + 1]] == u8('}') ) {
          k += 2;
          s = fs::after;
          break;
        }
        if ( depth >= vdepth ) [[unlikely]]
          return fail(error::depth_exceeded);
        is_arr[depth >> 6] &= ~(u64(1) << (depth & 63));
        ++depth;
        ++k;
        s = fs::key;
        break;
      }
      if ( c == u8('[') ) {
        if ( k + 1 < n and p[idx[k + 1]] == u8(']') ) {
          k += 2;
          s = fs::after;
          break;
        }
        if ( depth >= vdepth ) [[unlikely]]
          return fail(error::depth_exceeded);
        is_arr[depth >> 6] |= u64(1) << (depth & 63);
        ++depth;
        ++k;
        s = fs::value;
        break;
      }
      if ( c == u8('"') ) {
        if ( const max_t r = validate_string(p, len, idx, n, k); r < 0 ) return r;
        s = fs::after;
        break;
      }
      if ( c == u8('-') or is_digit(c) ) {
        if ( const max_t r = validate_number(p, len, idx[k]); r < 0 ) return r;
        ++k;
        s = fs::after;
        break;
      }
      if ( const max_t r = validate_literal(p, len, idx[k]); r < 0 ) return r;
      ++k;
      s = fs::after;
      break;
    }
    case fs::after: {
      if ( depth == 0 ) {
        consumed = (k < n) ? idx[k] : len;
        if ( !o.stop_when_done and k != n ) return fail(error::trailing_garbage);
        return 0;
      }
      if ( k >= n ) [[unlikely]]
        return fail(error::bad_syntax);
      const u8 c = p[idx[k]];
      const bool top_arr = ((is_arr[(depth - 1) >> 6] >> ((depth - 1) & 63)) & 1) != 0;
      if ( c == u8(',') ) {
        ++k;
        s = top_arr ? fs::value : fs::key;
        break;
      }
      if ( c == u8(']') ) {
        if ( !top_arr ) [[unlikely]]
          return fail(error::bad_syntax);
        --depth;
        ++k;
        break;
      }
      if ( c == u8('}') ) {
        if ( top_arr ) [[unlikely]]
          return fail(error::bad_syntax);
        --depth;
        ++k;
        break;
      }
      return fail(error::bad_syntax);
    }
    case fs::key: {
      if ( k >= n or p[idx[k]] != u8('"') ) [[unlikely]]
        return fail(error::bad_syntax);
      if ( const max_t r = validate_string(p, len, idx, n, k); r < 0 ) return r;
      if ( k >= n or p[idx[k]] != u8(':') ) [[unlikely]]
        return fail(error::bad_syntax);
      ++k;
      s = fs::value;
      break;
    }
    }
  }
}

};      // namespace cjson::__parse

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// full rfc 8259 conformance;
// zero materialization;
// constexpr
namespace cjson
{

constexpr error
validate(const u8 *p, usize len, opts o, scratch &sc) noexcept
{
  if ( len == 0 ) return error::empty_input;
  if ( !sc.ensure(len) ) [[unlikely]]
    return error::oom;
  max_t r = __scan::index_input(p, len, sc.idx, o);
  if ( r >= 0 ) {
    usize consumed = 0;
    r = __parse::validate_indexes(p, len, sc.idx, r, o, consumed);
  }
  return r < 0 ? as_error(r) : error::ok;
}

constexpr error
validate(const u8 *p, usize len, opts o = {}) noexcept
{
  scratch sc{};
  return validate(p, len, o, sc);
}

constexpr error
validate(bytes in, opts o, scratch &sc) noexcept
{
  return validate(in.ptr, in.len, o, sc);
}

constexpr error
validate(bytes in, opts o = {}) noexcept
{
  return validate(in.ptr, in.len, o);
}

constexpr error
validate(const char *p, usize n, opts o = {}) noexcept
{
  if consteval {
    u8 *tmp = new u8[n ? n : 1];
    for ( usize i = 0; i < n; ++i ) tmp[i] = static_cast<u8>(p[i]);
    const error e = validate(tmp, n, o);
    delete[] tmp;
    return e;
  } else {
    return validate(reinterpret_cast<const u8 *>(p), n, o);
  }
}

constexpr error
validate(const char *p, usize n, opts o, scratch &sc) noexcept
{
  return validate(reinterpret_cast<const u8 *>(p), n, o, sc);
}

constexpr error
validate(strv in, opts o = {}) noexcept
{
  return validate(in.ptr, in.len, o);
}

constexpr error
validate(strv in, opts o, scratch &sc) noexcept
{
  return validate(in.ptr, in.len, o, sc);
}

template<text_source C>
inline error
validate(const C &in, opts o = {}) noexcept
{
  return validate(as_bytes(in), o);
}

template<text_source C>
inline error
validate(const C &in, opts o, scratch &sc) noexcept
{
  return validate(as_bytes(in), o, sc);
}

constexpr bool
is_valid(bytes in, opts o = {}) noexcept
{
  return validate(in, o) == error::ok;
}

constexpr bool
is_valid(const u8 *p, usize n, opts o = {}) noexcept
{
  return validate(p, n, o) == error::ok;
}

constexpr bool
is_valid(const char *p, usize n, opts o = {}) noexcept
{
  return validate(p, n, o) == error::ok;
}

constexpr bool
is_valid(strv in, opts o = {}) noexcept
{
  return validate(in.ptr, in.len, o) == error::ok;
}

template<text_source C>
inline bool
is_valid(const C &in, opts o = {}) noexcept
{
  return validate(as_bytes(in), o) == error::ok;
}

};      // namespace cjson

namespace cjson::__parse
{

struct arena {
  value *slab = nullptr;
  usize cap = 0;

  constexpr bool
  ensure(usize need, usize used) noexcept
  {
    if ( need <= cap ) [[likely]]
      return true;
    usize ncap = cap + cap / 2;
    if ( ncap < need ) ncap = need;
    if ( ncap < 16 ) ncap = 16;
    value *ns = nullptr;
    if consteval {
      ns = new value[ncap]{};
    } else {
      ns = static_cast<value *>(abc::malloc(ncap * sizeof(value)));
      if ( !ns ) return false;
    }
    if ( slab ) {
      micron::memcpy(ns, slab, used);
      if consteval {
        delete[] slab;
      } else {
        abc::free(slab);
      }
    }
    slab = ns;
    cap = ncap;
    return true;
  }

  constexpr void
  release() noexcept
  {
    if ( !slab ) return;
    if consteval {
      delete[] slab;
    } else {
      abc::free(slab);
    }
    slab = nullptr;
    cap = 0;
  }
};

[[gnu::always_inline]] constexpr max_t
read_string(u8 *pool, usize plen, const u32 *idx, max_t nidx, max_t &k, value &out) noexcept
{
  const usize open = idx[k];
  if ( k + 1 < nidx and pool[idx[k + 1]] == u8('"') ) [[likely]] {
    const usize close = idx[k + 1];
    pool[close] = 0;
    out.tag = make_tag(kind::string, s_noesc, close - open - 1);
    out.pay.ofs = open + 1;
    k += 2;
    return 0;
  }
  const max_t slen = __str::unescape(pool, plen, idx, nidx, k);
  if ( slen < 0 ) [[unlikely]]
    return slen;
  out.tag = make_tag(kind::string, s_plain, u64(slen));
  out.pay.ofs = open + 1;
  return 0;
}

constexpr max_t
read_literal(const u8 *p, usize len, usize i, value &out) noexcept
{
  const u8 c = p[i];
  if ( c == u8('t') ) {
    if ( i + 4 > len or __load32(p + i) != w_true or (i + 4 < len and !is_num_end(p[i + 4])) ) [[unlikely]]
      return fail(error::bad_syntax);
    out.tag = make_tag(kind::boolean, s_true, 0);
    out.pay.u = 0;
    return 0;
  }
  if ( c == u8('n') ) {
    if ( i + 4 > len or __load32(p + i) != w_null or (i + 4 < len and !is_num_end(p[i + 4])) ) [[unlikely]]
      return fail(error::bad_syntax);
    out.tag = make_tag(kind::null, 0, 0);
    out.pay.u = 0;
    return 0;
  }
  if ( c == u8('f') ) {
    if ( i + 5 > len or __load32(p + i + 1) != w_alse or (i + 5 < len and !is_num_end(p[i + 5])) ) [[unlikely]]
      return fail(error::bad_syntax);
    out.tag = make_tag(kind::boolean, s_false, 0);
    out.pay.u = 0;
    return 0;
  }
  return fail(error::bad_syntax);
}

template<bool RawNumbers, bool Bounds>
constexpr max_t
build_t(u8 *__restrict pool, usize len, const u32 *__restrict idx, const max_t nidx, opts o, arena &a, usize &consumed,
        wbounds &wb) noexcept
{
  constexpr usize none = ~usize(0);
  value *const vs = a.slab;
  usize vi = 0;
  usize ci = none;
  // counted unconditionally, never += (ci != none)
  u64 ctn_len = 0;
  u32 depth = 0;
  max_t k = 0;
  u64 wb0 = 32;
  u64 wp0 = 8;
  u64 wp1 = 0;
  u32 pdepth = 0;

  enum class fs : u8 { value, after, key };

  fs s = fs::value;
  for ( ;; ) {
    switch ( s ) {
    case fs::value: {
      if ( k >= nidx ) [[unlikely]]
        return fail(error::bad_syntax);
      const u8 c = pool[idx[k]];
      if ( c == u8('"') ) {
        if ( const max_t r = read_string(pool, len, idx, nidx, k, vs[vi]); r < 0 ) [[unlikely]]
          return r;
        if constexpr ( Bounds ) {
          const u64 sl = get_len(vs[vi]);
          wb0 += ((vs[vi].tag & s_mask) == s_noesc ? sl : sl * 6) + 4;
          wp0 += 3;
          wp1 += pdepth;
        }
        ++vi;
        ++ctn_len;
        s = fs::after;
        break;
      }
      if ( c == u8('{') or c == u8('[') ) {
        const bool obj = c == u8('{');
        if ( k + 1 < nidx and pool[idx[k + 1]] == (obj ? u8('}') : u8(']')) ) {
          vs[vi].tag = make_tag(obj ? kind::object : kind::array, 0, 0);
          vs[vi].pay.ofs = sizeof(value);
          if constexpr ( Bounds ) {
            wb0 += 12;
            wp0 += 3;
            wp1 += pdepth;
          }
          ++vi;
          k += 2;
          ++ctn_len;
          s = fs::after;
          break;
        }
        if constexpr ( depth_limit > 0 ) {
          if ( ++depth > depth_limit ) [[unlikely]]
            return fail(error::depth_exceeded);
        }
        if ( ci != none ) vs[ci].tag = ((ctn_len + 1) << tag_bit) | (vs[ci].tag & 0xff);      // stash parent count
        vs[vi].tag = make_tag(obj ? kind::object : kind::array, 0, 0);
        vs[vi].pay.ofs = ci == none ? 0 : (vi - ci) * sizeof(value);      // back-offset to parent
        if constexpr ( Bounds ) {
          wb0 += 12;
          ++pdepth;
          wp0 += 3;
          wp1 += pdepth;
        }
        ci = vi;
        ++vi;
        ctn_len = 0;
        ++k;
        s = obj ? fs::key : fs::value;
        break;
      }
      if ( c == u8('-') or is_digit(c) ) {
        if constexpr ( RawNumbers ) {
          // validate the grammar, store the raw token {ofs,len}
          if ( const max_t r = validate_number(pool, len, idx[k]); r < 0 ) [[unlikely]]
            return r;
          usize e = idx[k];
          while ( e < len and is_num_char(pool[e]) ) ++e;
          vs[vi].tag = make_tag(kind::raw, 0, e - idx[k]);
          vs[vi].pay.ofs = idx[k];
        } else if ( const max_t r = __num::read_number(pool, len, idx[k], vs[vi]); r < 0 ) [[unlikely]] {
          return r;
        }
        if constexpr ( Bounds ) {
          wb0 += RawNumbers ? 12 : 42;
          wp0 += 3;
          wp1 += pdepth;
        }
        ++vi;
        ++k;
        ++ctn_len;
        s = fs::after;
        break;
      }
      if ( const max_t r = read_literal(pool, len, idx[k], vs[vi]); r < 0 ) [[unlikely]]
        return r;
      if constexpr ( Bounds ) {
        wb0 += 12;
        wp0 += 3;
        wp1 += pdepth;
      }
      ++vi;
      ++k;
      ++ctn_len;
      s = fs::after;
      break;
    }
    case fs::after: {
      if ( ci == none ) {
        consumed = idx[k];      // sentinel yields len when everything is consumed
        if ( !o.stop_when_done and k != nidx ) [[unlikely]]
          return fail(error::trailing_garbage);
        if constexpr ( Bounds ) {
          wb.flat = wb0;
          wb.pretty_c = wp0;
          wb.pretty_k = wp1;
        }
        return max_t(vi);
      }
      if ( k >= nidx ) [[unlikely]]
        return fail(error::bad_syntax);
      const u8 c = pool[idx[k]];
      const kind ck = get_kind(vs[ci]);
      if ( c == u8(',') ) {
        ++k;
        s = ck == kind::object ? fs::key : fs::value;
        break;
      }
      if ( c == u8(']') or c == u8('}') ) {
        if ( (c == u8(']')) != (ck == kind::array) ) [[unlikely]]
          return fail(error::bad_syntax);
        const u64 back = vs[ci].pay.ofs;
        vs[ci].pay.ofs = (vi - ci) * sizeof(value);      // forward sibling offset
        if ( ck == kind::object )
          vs[ci].tag = (ctn_len << (tag_bit - 1)) | u64(kind::object);      // /2 and shift, one op
        else
          vs[ci].tag = (ctn_len << tag_bit) | u64(kind::array);
        ++k;
        if constexpr ( depth_limit > 0 ) --depth;
        if constexpr ( Bounds ) {
          --pdepth;
          wp0 += 1;
          wp1 += pdepth;
        }
        if ( back == 0 ) {
          ci = none;      // root container closed
        } else {
          ci = ci - back / sizeof(value);
          ctn_len = vs[ci].tag >> tag_bit;      // parent count, child already included
        }
        s = fs::after;
        break;
      }
      return fail(error::bad_syntax);
    }
    case fs::key: {
      if ( k >= nidx or pool[idx[k]] != u8('"') ) [[unlikely]]
        return fail(error::bad_syntax);
      if ( const max_t r = read_string(pool, len, idx, nidx, k, vs[vi]); r < 0 ) [[unlikely]]
        return r;
      if constexpr ( Bounds ) {
        const u64 sl = get_len(vs[vi]);
        wb0 += ((vs[vi].tag & s_mask) == s_noesc ? sl : sl * 6) + 4;
        wp0 += 3;
        wp1 += pdepth;
      }
      ++vi;
      ++ctn_len;
      if ( k >= nidx or pool[idx[k]] != u8(':') ) [[unlikely]]
        return fail(error::bad_syntax);
      ++k;
      s = fs::value;
      break;
    }
    }
  }
}

constexpr max_t
build(u8 *pool, usize len, const u32 *idx, const max_t nidx, opts o, arena &a, usize &consumed, wbounds &wb) noexcept
{
  if ( nidx == 0 ) return fail(error::empty_input);
  if ( !a.ensure(usize(nidx) + 2, 0) ) [[unlikely]]
    return fail(error::oom);
  if ( o.with_write_bound ) {
    return o.numbers_as_raw ? build_t<true, true>(pool, len, idx, nidx, o, a, consumed, wb)
                            : build_t<false, true>(pool, len, idx, nidx, o, a, consumed, wb);
  }
  return o.numbers_as_raw ? build_t<true, false>(pool, len, idx, nidx, o, a, consumed, wb)
                          : build_t<false, false>(pool, len, idx, nidx, o, a, consumed, wb);
}

constexpr max_t
build(u8 *pool, usize len, const u32 *idx, const max_t nidx, opts o, arena &a, usize &consumed) noexcept
{
  wbounds wb{};
  return build(pool, len, idx, nidx, o, a, consumed, wb);
}

};      // namespace cjson::__parse

namespace cjson
{

constexpr max_t
__parse_into(doc &d, bytes in, opts o, scratch &sc) noexcept
{
  if ( in.len == 0 ) return fail(error::empty_input);
  if ( !sc.ensure(in.len) ) [[unlikely]]
    return fail(error::oom);
  const usize pcap = in.len + padding;
  u8 *pool = nullptr;
  if consteval {
    pool = new u8[pcap];
  } else {
    pool = static_cast<u8 *>(abc::malloc(pcap));
    if ( !pool ) [[unlikely]]
      return fail(error::oom);
  }
  auto free_pool = [&]() constexpr {
    if consteval {
      delete[] pool;
    } else {
      abc::free(pool);
    }
  };

  // copy fused
  const max_t n = __scan::index_input_copy(in.ptr, in.len, sc.idx, pool, o);
  if ( n < 0 ) [[unlikely]] {
    free_pool();
    return n;
  }
  __parse::arena a{};
  usize consumed = 0;
  wbounds wb{};
  const max_t r = __parse::build(pool, in.len, sc.idx, n, o, a, consumed, wb);
  if ( r < 0 ) [[unlikely]] {
    a.release();
    free_pool();
    return r;
  }
  d.__vals = a.slab;
  d.__nvals = usize(r);
  d.__pool = pool;
  d.__pcap = pcap;
  d.__consumed = consumed;
  d.__wb = wb;
  d.__borrowed = false;
  return r;
}

constexpr max_t
__parse_reuse_into(doc &d, bytes in, opts o, scratch &sc) noexcept
{
  if ( in.len == 0 ) return fail(error::empty_input);
  if ( !sc.ensure(in.len) ) [[unlikely]]
    return fail(error::oom);
  const usize pcap = in.len + padding;
  if ( !sc.ensure_pool(pcap) ) [[unlikely]]
    return fail(error::oom);
  u8 *pool = sc.pool;
  const max_t n = __scan::index_input_copy(in.ptr, in.len, sc.idx, pool, o);
  if ( n < 0 ) [[unlikely]]
    return n;
  if ( !sc.ensure_vals(usize(n) + 2) ) [[unlikely]]
    return fail(error::oom);
  __parse::arena a{ sc.vals, sc.vals_cap };
  usize consumed = 0;
  wbounds wb{};
  const max_t r = __parse::build(pool, in.len, sc.idx, n, o, a, consumed, wb);
  sc.vals = a.slab;
  sc.vals_cap = a.cap;
  if ( r < 0 ) [[unlikely]]
    return r;
  d.__vals = a.slab;
  d.__nvals = usize(r);
  d.__pool = pool;
  d.__pcap = pcap;
  d.__consumed = consumed;
  d.__wb = wb;
  d.__borrowed = true;
  return r;
}

constexpr max_t
__parse_insitu_into(doc &d, wbytes in, opts o, scratch &sc) noexcept
{
  if ( in.len == 0 ) return fail(error::empty_input);
  if ( !sc.ensure(in.len) ) [[unlikely]]
    return fail(error::oom);
  const max_t n = __scan::index_input(in.ptr, in.len, sc.idx, o);
  if ( n < 0 ) [[unlikely]]
    return n;
  __parse::arena a{};
  usize consumed = 0;
  wbounds wb{};
  const max_t r = __parse::build(in.ptr, in.len, sc.idx, n, o, a, consumed, wb);
  if ( r < 0 ) [[unlikely]] {
    a.release();
    return r;
  }
  d.__vals = a.slab;
  d.__nvals = usize(r);
  d.__pool = nullptr;
  d.__alias = in.ptr;
  d.__pcap = 0;
  d.__consumed = consumed;
  d.__wb = wb;
  d.__borrowed = false;
  return r;
}

constexpr max_t
__parse_insitu_reuse_into(doc &d, wbytes in, opts o, scratch &sc) noexcept
{
  if ( in.len == 0 ) return fail(error::empty_input);
  if ( !sc.ensure(in.len) ) [[unlikely]]
    return fail(error::oom);
  const max_t n = __scan::index_input(in.ptr, in.len, sc.idx, o);
  if ( n < 0 ) [[unlikely]]
    return n;
  if ( !sc.ensure_vals(usize(n) + 2) ) [[unlikely]]
    return fail(error::oom);
  __parse::arena a{ sc.vals, sc.vals_cap };
  usize consumed = 0;
  wbounds wb{};
  const max_t r = __parse::build(in.ptr, in.len, sc.idx, n, o, a, consumed, wb);
  sc.vals = a.slab;
  sc.vals_cap = a.cap;
  if ( r < 0 ) [[unlikely]]
    return r;
  d.__vals = a.slab;
  d.__nvals = usize(r);
  d.__pool = nullptr;
  d.__alias = in.ptr;
  d.__pcap = 0;
  d.__consumed = consumed;
  d.__wb = wb;
  d.__borrowed = true;
  return r;
}

inline result<doc>
parse(bytes in, opts o, scratch &sc) noexcept
{
  doc d{};
  const max_t r = __parse_into(d, in, o, sc);
  if ( r < 0 ) return result<doc>{ micron::tag<error>{}, as_error(r) };
  return result<doc>{ micron::tag<doc>{}, micron::move(d) };
}

inline result<doc>
parse(bytes in, opts o = {}) noexcept
{
  scratch sc{};
  return parse(in, o, sc);
}

inline result<doc>
parse(const u8 *p, usize n, opts o = {}) noexcept
{
  return parse(bytes{ p, n }, o);
}

inline result<doc>
parse(const char *p, usize n, opts o = {}) noexcept
{
  return parse(bytes{ reinterpret_cast<const u8 *>(p), n }, o);
}

inline result<doc>
parse(const char *p, usize n, opts o, scratch &sc) noexcept
{
  return parse(bytes{ reinterpret_cast<const u8 *>(p), n }, o, sc);
}

inline result<doc>
parse(const u8 *p, usize n, opts o, scratch &sc) noexcept
{
  return parse(bytes{ p, n }, o, sc);
}

inline result<doc>
parse(strv in, opts o = {}) noexcept
{
  return parse(bytes{ reinterpret_cast<const u8 *>(in.ptr), in.len }, o);
}

inline result<doc>
parse(strv in, opts o, scratch &sc) noexcept
{
  return parse(bytes{ reinterpret_cast<const u8 *>(in.ptr), in.len }, o, sc);
}

template<text_source C>
inline result<doc>
parse(const C &in, opts o = {}) noexcept
{
  return parse(as_bytes(in), o);
}

template<text_source C>
inline result<doc>
parse(const C &in, opts o, scratch &sc) noexcept
{
  return parse(as_bytes(in), o, sc);
}

inline result<doc>
parse_insitu(wbytes in, opts o, scratch &sc) noexcept
{
  doc d{};
  const max_t r = __parse_insitu_into(d, in, o, sc);
  if ( r < 0 ) return result<doc>{ micron::tag<error>{}, as_error(r) };
  return result<doc>{ micron::tag<doc>{}, micron::move(d) };
}

inline result<doc>
parse_insitu(wbytes in, opts o = {}) noexcept
{
  scratch sc{};
  return parse_insitu(in, o, sc);
}

template<byte_source C>
inline result<doc>
parse_insitu(C &in, opts o = {}) noexcept
{
  return parse_insitu(as_wbytes(in), o);
}

template<byte_source C>
inline result<doc>
parse_insitu(C &in, opts o, scratch &sc) noexcept
{
  return parse_insitu(as_wbytes(in), o, sc);
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// reuse mode

inline result<doc>
parse_reuse(bytes in, opts o, scratch &sc) noexcept
{
  doc d{};
  const max_t r = __parse_reuse_into(d, in, o, sc);
  if ( r < 0 ) return result<doc>{ micron::tag<error>{}, as_error(r) };
  return result<doc>{ micron::tag<doc>{}, micron::move(d) };
}

inline result<doc>
parse_reuse(const u8 *p, usize n, opts o, scratch &sc) noexcept
{
  return parse_reuse(bytes{ p, n }, o, sc);
}

inline result<doc>
parse_reuse(const char *p, usize n, opts o, scratch &sc) noexcept
{
  return parse_reuse(bytes{ reinterpret_cast<const u8 *>(p), n }, o, sc);
}

inline result<doc>
parse_reuse(strv in, opts o, scratch &sc) noexcept
{
  return parse_reuse(bytes{ reinterpret_cast<const u8 *>(in.ptr), in.len }, o, sc);
}

template<text_source C>
inline result<doc>
parse_reuse(const C &in, opts o, scratch &sc) noexcept
{
  return parse_reuse(as_bytes(in), o, sc);
}

inline result<doc>
parse_insitu_reuse(wbytes in, opts o, scratch &sc) noexcept
{
  doc d{};
  const max_t r = __parse_insitu_reuse_into(d, in, o, sc);
  if ( r < 0 ) return result<doc>{ micron::tag<error>{}, as_error(r) };
  return result<doc>{ micron::tag<doc>{}, micron::move(d) };
}

template<byte_source C>
inline result<doc>
parse_insitu_reuse(C &in, opts o, scratch &sc) noexcept
{
  return parse_insitu_reuse(as_wbytes(in), o, sc);
}

};      // namespace cjson
