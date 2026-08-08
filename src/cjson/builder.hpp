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
#include "dtoa.hpp"
#include "error.hpp"
#include "itoa.hpp"
#include "write.hpp"

#include <micron/string/strings.hpp>
#include <micron/type_traits.hpp>
#include <micron/types.hpp>

// %%%%%%%%%%%%%%%%%%%%%%%%%%
// builder

namespace cjson
{

class builder
{
  micron::string __out{};
  u64 __obj_bits[(depth_limit ? depth_limit : 1024) / 64 + 1]{};
  u32 __depth = 0;
  bool __key_open = false;      // a key was emitted, value pending
  error __err = error::ok;

  static constexpr u32 __maxd = depth_limit ? depth_limit : 1024;

  constexpr bool
  __top_obj() const noexcept
  {
    return __depth != 0 and ((__obj_bits[(__depth - 1) >> 6] >> ((__depth - 1) & 63)) & 1) != 0;
  }

  bool
  __value_ok() const noexcept
  {
    if ( __err != error::ok ) return false;
    if ( __depth == 0 ) return __out.size() == 0;
    if ( __top_obj() ) return __key_open;
    return true;
  }

  // escape into a stack chunk, append per chunk
  void
  __append_escaped(strv s)
  {
    u8 tmp[128];
    usize i = 0;
    while ( i < s.len ) {
      u8 *w = tmp;
      while ( i < s.len and w < tmp + 120 ) {
        const u8 c = u8(s.ptr[i]);
        if ( c == u8('"') or c == u8('\\') ) {
          *w++ = u8('\\');
          *w++ = c;
        } else if ( c >= 0x20 ) {
          *w++ = c;
        } else {
          *w++ = u8('\\');
          switch ( c ) {
          case 0x08:
            *w++ = u8('b');
            break;
          case 0x09:
            *w++ = u8('t');
            break;
          case 0x0a:
            *w++ = u8('n');
            break;
          case 0x0c:
            *w++ = u8('f');
            break;
          case 0x0d:
            *w++ = u8('r');
            break;
          default: {
            *w++ = u8('u');
            *w++ = u8('0');
            *w++ = u8('0');
            const u8 hi = c >> 4, lo = c & 0xf;
            *w++ = u8(hi < 10 ? '0' + hi : 'a' + hi - 10);
            *w++ = u8(lo < 10 ? '0' + lo : 'a' + lo - 10);
            break;
          }
          }
        }
        ++i;
      }
      __out.append(reinterpret_cast<const char *>(tmp), usize(w - tmp));
    }
  }

  void
  __post_value() noexcept
  {
    __key_open = false;
    __out.push_back(',');
  }

public:
  ~builder() = default;

  builder() = default;

  explicit builder(micron::string &&reuse) : __out(micron::move(reuse)) { __out.fast_clear(); }

  builder(const builder &) = delete;
  builder &operator=(const builder &) = delete;

  error
  err() const noexcept
  {
    return __err;
  }

  builder &
  obj()
  {
    if ( !__value_ok() or __depth >= __maxd ) {
      __err = __err == error::ok ? error::bad_syntax : __err;
      return *this;
    }
    __obj_bits[__depth >> 6] |= u64(1) << (__depth & 63);
    ++__depth;
    __key_open = false;
    __out.push_back('{');
    return *this;
  }

  builder &
  arr()
  {
    if ( !__value_ok() or __depth >= __maxd ) {
      __err = __err == error::ok ? error::bad_syntax : __err;
      return *this;
    }
    __obj_bits[__depth >> 6] &= ~(u64(1) << (__depth & 63));
    ++__depth;
    __key_open = false;
    __out.push_back('[');
    return *this;
  }

  builder &
  end()
  {
    if ( __err != error::ok ) return *this;
    if ( __depth == 0 or __key_open ) {
      __err = error::bad_syntax;
      return *this;
    }
    const bool obj = __top_obj();
    --__depth;
    if ( __out.size() and __out[__out.size() - 1] == ',' ) __out.pop_back();
    __out.push_back(obj ? '}' : ']');
    __out.push_back(',');
    __key_open = false;
    return *this;
  }

  builder &
  key(strv k)
  {
    if ( __err != error::ok ) return *this;
    if ( !__top_obj() or __key_open ) {
      __err = error::bad_syntax;
      return *this;
    }
    __out.push_back('"');
    __append_escaped(k);
    __out.push_back('"');
    __out.push_back(':');
    __key_open = true;
    return *this;
  }

  builder &
  key(const char *k)
  {
    return key(strv{ k, __doc::cstr_len(k) });
  }

  template<micron::is_string S>
  builder &
  key(const S &k)
  {
    return key(strv{ k.c_str(), k.size() });
  }

  builder &
  value(strv s)
  {
    if ( !__value_ok() ) {
      __err = __err == error::ok ? error::bad_syntax : __err;
      return *this;
    }
    __out.push_back('"');
    __append_escaped(s);
    __out.push_back('"');
    __post_value();
    return *this;
  }

  builder &
  value(const char *s)
  {
    return value(strv{ s, __doc::cstr_len(s) });
  }

  template<micron::is_string S>
  builder &
  value(const S &s)
  {
    return value(strv{ s.c_str(), s.size() });
  }

  builder &
  value(i64 v)
  {
    if ( !__value_ok() ) {
      __err = __err == error::ok ? error::bad_syntax : __err;
      return *this;
    }
    u8 tmp[24];
    u8 *e = __itoa::write_i64(tmp, v);
    __out.append(reinterpret_cast<const char *>(tmp), usize(e - tmp));
    __post_value();
    return *this;
  }

  builder &
  value(u64 v)
  {
    if ( !__value_ok() ) {
      __err = __err == error::ok ? error::bad_syntax : __err;
      return *this;
    }
    u8 tmp[24];
    u8 *e = __itoa::write_u64(tmp, v);
    __out.append(reinterpret_cast<const char *>(tmp), usize(e - tmp));
    __post_value();
    return *this;
  }

  builder &
  value(i32 v)
  {
    return value(i64(v));
  }

  builder &
  value(u32 v)
  {
    return value(u64(v));
  }

  builder &
  value(f64 v)
  {
    if ( !__value_ok() ) {
      __err = __err == error::ok ? error::bad_syntax : __err;
      return *this;
    }
    const u64 raw = __builtin_bit_cast(u64, v);
    u8 tmp[40];
    u8 *e = nullptr;
    if ( (raw & 0x7ff0000000000000ull) == 0x7ff0000000000000ull ) {
      tmp[0] = 'n';
      tmp[1] = 'u';
      tmp[2] = 'l';
      tmp[3] = 'l';
      e = tmp + 4;
    } else {
      e = __dtoa::write_f64(tmp, v);
    }
    __out.append(reinterpret_cast<const char *>(tmp), usize(e - tmp));
    __post_value();
    return *this;
  }

  builder &
  value(bool v)
  {
    if ( !__value_ok() ) {
      __err = __err == error::ok ? error::bad_syntax : __err;
      return *this;
    }
    __out.append(v ? "true" : "false", v ? 4 : 5);
    __post_value();
    return *this;
  }

  builder &
  null()
  {
    if ( !__value_ok() ) {
      __err = __err == error::ok ? error::bad_syntax : __err;
      return *this;
    }
    __out.append("null", 4);
    __post_value();
    return *this;
  }

  // preserialized fragment (ct-baked constants, nested documents); trusted verbatim
  builder &
  raw(strv json)
  {
    if ( !__value_ok() ) {
      __err = __err == error::ok ? error::bad_syntax : __err;
      return *this;
    }
    __out.append(json.ptr, json.len);
    __post_value();
    return *this;
  }

  template<typename V>
  builder &
  kv(strv k, V v)
  {
    key(k);
    return value(v);
  }

  template<typename V>
  builder &
  kv(const char *k, V v)
  {
    key(k);
    return value(v);
  }

  template<micron::is_string S, typename V>
  builder &
  kv(const S &k, const V &v)
  {
    key(strv{ k.c_str(), k.size() });
    return value(v);
  }

  strv
  out() noexcept
  {
    if ( __err != error::ok or __depth != 0 or __key_open or __out.size() == 0 ) return strv{};
    if ( __out[__out.size() - 1] == ',' ) __out.pop_back();
    return strv{ __out.c_str(), __out.size() };
  }

  micron::string
  take() noexcept
  {
    (void)out();
    micron::string s = micron::move(__out);
    __out = micron::string{};
    __depth = 0;
    __key_open = false;
    __err = error::ok;
    return s;
  }
};

};      // namespace cjson
