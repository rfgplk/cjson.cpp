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

#include <micron/sum.hpp>
#include <micron/types.hpp>

namespace cjson
{

enum class error : i32 {
  ok = 0,
  bad_syntax,            // structural violation: unexpected token, unbalanced containers, stray comma/colon
  bad_number,            // malformed number literal: leading zero, bare sign, "1.", "1e", hex
  bad_string,            // unterminated string, unescaped control char inside a string
  bad_escape,            // unknown \x escape or malformed \uXXXX hex
  bad_utf8,              // invalid utf-8 byte sequence: overlong, surrogate encoding, > u+10ffff, truncation
  bad_surrogate,         // \uXXXX surrogate half without its pair, or reversed pairing
  depth_exceeded,        // nesting beyond depth_limit
  trailing_garbage,      // non-whitespace bytes after the root (absent opts::stop_when_done)
  empty_input,           // no root value found
  short_output,          // caller-supplied output buffer too small (minify/write _into)
  wrong_type,            // typed extraction against a mismatched kind
  no_such_field,         // object key / json-pointer / array index miss
  out_of_range,          // number does not fit the requested integer type
  oom,                   // arena/scratch/output allocation failed
  io_error,              // kernel-side failure on the flash sink (ring setup, short write, fsync)
};

template<typename T> using result = micron::option<T, error>;

constexpr max_t
fail(error e) noexcept
{
  return -static_cast<max_t>(static_cast<i32>(e));
}

constexpr error
as_error(max_t r) noexcept
{
  return r < 0 ? static_cast<error>(static_cast<i32>(-r)) : error::ok;
}

constexpr const char *
error_name(error e) noexcept
{
  switch ( e ) {
  case error::ok:
    return "ok";
  case error::bad_syntax:
    return "bad_syntax";
  case error::bad_number:
    return "bad_number";
  case error::bad_string:
    return "bad_string";
  case error::bad_escape:
    return "bad_escape";
  case error::bad_utf8:
    return "bad_utf8";
  case error::bad_surrogate:
    return "bad_surrogate";
  case error::depth_exceeded:
    return "depth_exceeded";
  case error::trailing_garbage:
    return "trailing_garbage";
  case error::empty_input:
    return "empty_input";
  case error::short_output:
    return "short_output";
  case error::wrong_type:
    return "wrong_type";
  case error::no_such_field:
    return "no_such_field";
  case error::out_of_range:
    return "out_of_range";
  case error::oom:
    return "oom";
  case error::io_error:
    return "io_error";
  }
  return "unknown";
}

struct fault {
  error code = error::ok;
  usize at = 0;
};

};      // namespace cjson
