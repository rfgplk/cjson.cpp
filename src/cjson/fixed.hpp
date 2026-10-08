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

#include "parse.hpp"
#include "write.hpp"
#include "utf8.hpp"

// ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
// Caller-owned parsing and transactional string emission; no growth or allocation.
namespace cjson
{
struct fixed_scratch {
  micron::raw_slice<u32> indexes{};
  wbytes pool{};
  micron::raw_slice<value> values{};
  micron::raw_slice<source_range> ranges{};      // optional, one per value including keys
};

// Any attempt invalidates earlier views of this scratch. Output is empty on failure.
// Input, scratch and output storage must be disjoint and remain alive while borrowed.
constexpr max_t
parse_into(doc &out, bytes in, fixed_scratch sc, opts o = {}, u32 max_depth = 64) noexcept
{
  out.release();
  if ( !in.ptr or !in.len ) return fail(error::empty_input);
  if ( in.len > 0xffffffffull - padding or max_depth > (depth_limit ? depth_limit : 1024) )
    return fail(error::out_of_range);
  if ( !sc.indexes.ptr or sc.indexes.len < __scan::index_slots(in.len) or !sc.pool.ptr
       or sc.pool.len < in.len + padding or !sc.values.ptr or !sc.values.len
       or (sc.ranges.ptr and sc.ranges.len < sc.values.len) ) return fail(error::oom);
  const max_t n = __scan::index_input_copy(in.ptr, in.len, sc.indexes.ptr, sc.pool.ptr, o);
  if ( n < 0 ) return n;
  if ( !n ) return fail(error::empty_input);
  __parse::arena a{sc.values.ptr, sc.values.len};
  usize consumed = 0;
  wbounds wb{};
  const auto build = [&]<bool Raw, bool Located>() constexpr {
    return __parse::build_t<Raw, true, true, Located>(sc.pool.ptr, in.len, sc.indexes.ptr, n, o, a,
                                                     consumed, wb, sc.ranges.ptr, max_depth);
  };
  const max_t result = o.numbers_as_raw
      ? (sc.ranges.ptr ? build.template operator()<true, true>() : build.template operator()<true, false>())
      : (sc.ranges.ptr ? build.template operator()<false, true>() : build.template operator()<false, false>());
  if ( result < 0 ) return result;
  out.__vals = sc.values.ptr; out.__nvals = usize(result); out.__vcap = 0;
  out.__pool = sc.pool.ptr; out.__pcap = sc.pool.len; out.__plen = in.len;
  out.__consumed = consumed; out.__wb = wb; out.__borrowed = true;
  return result;
}

constexpr max_t
quote_bound(bytes text) noexcept
{
  if ( text.len and !text.ptr ) return fail(error::bad_string);
  if ( text.len > (usize(~u64(0)) >> 1) / 6 - 2 ) return fail(error::out_of_range);
  if ( !__utf8::validate_scalar(text.ptr, text.len) ) return fail(error::bad_utf8);
  usize needed = 2;
  for ( usize i = 0; i < text.len; ++i ) {
    const u8 c = text.ptr[i];
    needed += c < 0x20 ? (c == 8 or c == 9 or c == 10 or c == 12 or c == 13 ? 2 : 6)
                      : (c == u8('"') or c == u8('\\') ? 2 : 1);
  }
  return max_t(needed);
}

// The entire input is checked before the first output write. Overlap is unsupported.
constexpr max_t
quote_into(bytes text, wbytes out) noexcept
{
  const max_t needed = quote_bound(text);
  if ( needed < 0 ) return needed;
  if ( !out.ptr or out.len < usize(needed) ) return fail(error::short_output);
  return max_t(__write::write_string_escaped(out.ptr, text.ptr, text.len) - out.ptr);
}
// Output is staging storage: publish only after finish() succeeds. Failure is sticky.
template<usize Depth = 64> class fixed_writer
{
  static_assert(Depth > 0 && Depth <= 1024);
  struct frame { bool object, key; usize count; };
  frame __stack[Depth]{};
  wbytes __out;
  usize __size = 0, __depth = 0;
  bool __root = false;
  error __error = error::ok;

  constexpr error __fail(error e) noexcept { if ( __error == error::ok ) __error = e; return __error; }
  constexpr bool __room(usize n) const noexcept { return n <= __out.len - __size; }
  constexpr error __prefix(usize bytes) noexcept
  {
    if ( __error != error::ok ) return __error;
    if ( __depth > Depth ) return __fail(error::depth_exceeded);
    bool comma = false;
    if ( !__depth ) {
      if ( __root ) return __fail(error::trailing_garbage);
    } else {
      auto &f = __stack[__depth - 1];
      if ( f.object and !f.key ) return __fail(error::bad_syntax);
      comma = !f.object and f.count;
    }
    if ( !__out.ptr or bytes > __out.len - __size or usize(comma) > __out.len - __size - bytes )
      return __fail(error::short_output);
    if ( comma ) __out.ptr[__size++] = ',';
    if ( !__depth ) __root = true;
    else { auto &f = __stack[__depth - 1]; ++f.count; f.key = false; }
    return error::ok;
  }
  constexpr error __container(bool object) noexcept
  {
    if ( __depth >= Depth ) return __fail(error::depth_exceeded);
    if ( __prefix(1) != error::ok ) return __error;
    __out.ptr[__size++] = object ? '{' : '[';
    __stack[__depth++] = {object, false, 0};
    return error::ok;
  }
  constexpr error __literal(const char *p, usize n) noexcept
  {
    if ( __prefix(n) != error::ok ) return __error;
    for ( usize i = 0; i < n; ++i ) __out.ptr[__size++] = u8(p[i]);
    return error::ok;
  }
public:
  constexpr explicit fixed_writer(wbytes out) noexcept : __out(out) { }
  fixed_writer(const fixed_writer &) = delete;
  fixed_writer &operator=(const fixed_writer &) = delete;
  constexpr error object() noexcept { return __container(true); }
  constexpr error array() noexcept { return __container(false); }
  constexpr error end() noexcept
  {
    if ( __error != error::ok ) return __error;
    if ( !__depth or __stack[__depth - 1].key ) return __fail(error::bad_syntax);
    if ( !__room(1) ) return __fail(error::short_output);
    __out.ptr[__size++] = __stack[--__depth].object ? '}' : ']';
    return error::ok;
  }
  constexpr error key(bytes text) noexcept
  {
    if ( __error != error::ok ) return __error;
    if ( !__depth or !__stack[__depth - 1].object or __stack[__depth - 1].key ) return __fail(error::bad_syntax);
    const max_t needed = quote_bound(text);
    if ( needed < 0 ) return __fail(as_error(needed));
    auto &f = __stack[__depth - 1];
    if ( !__room(usize(needed) + 1 + usize(f.count != 0)) ) return __fail(error::short_output);
    if ( f.count ) __out.ptr[__size++] = ',';
    __size += usize(quote_into(text, {__out.ptr + __size, __out.len - __size}));
    __out.ptr[__size++] = ':'; f.key = true;
    return error::ok;
  }
  constexpr error string(bytes text) noexcept
  {
    const max_t needed = quote_bound(text);
    if ( needed < 0 ) return __fail(as_error(needed));
    if ( __prefix(usize(needed)) != error::ok ) return __error;
    __size += usize(quote_into(text, {__out.ptr + __size, __out.len - __size}));
    return error::ok;
  }
  constexpr error number(u64 value) noexcept
  {
    u8 digits[20]; const usize n = usize(__itoa::write_u64(digits, value) - digits);
    if ( __prefix(n) != error::ok ) return __error;
    for ( usize i = 0; i < n; ++i ) __out.ptr[__size++] = digits[i];
    return error::ok;
  }
  constexpr error boolean(bool value) noexcept { return value ? __literal("true", 4) : __literal("false", 5); }
  constexpr error null() noexcept { return __literal("null", 4); }
  constexpr max_t finish() const noexcept
  {
    if ( __error != error::ok ) return fail(__error);
    return __root and !__depth ? max_t(__size) : fail(error::bad_syntax);
  }
};

};      // namespace cjson
