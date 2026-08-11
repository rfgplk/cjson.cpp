//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#pragma once

#include "config.hpp"
#include "doc.hpp"
#include "ondemand.hpp"
#include "write.hpp"

namespace cjson
{

template<typename S>
concept __pun_sink = requires(S &s) {
  { s.put("x", usize(1)) } -> micron::convertible_to<max_t>;
};

template<__pun_sink S>
max_t
printk(S &s, const jnull &) noexcept
{
  return max_t(s.put("null", usize(4)));
}

template<__pun_sink S>
max_t
printk(S &s, const jraw &r) noexcept
{
  if ( r.text.len == 0 ) return 0;
  return max_t(s.put(r.text.ptr, r.text.len));
}

template<__pun_sink S>
max_t
printk(S &s, const vref &r)
{
  const val v = as_val(r);
  if ( !v ) return 0;
  const usize n = write_bound(v);
  if ( n != 0 and n <= 512 ) {
    u8 buf[512];
    const max_t w = write_into(v, wbytes{ buf, 512 });
    if ( w > 0 ) return max_t(s.put(reinterpret_cast<const char *>(buf), usize(w)));
    return 0;
  }
  micron::string t = write_str(v);
  return max_t(s.put(t.c_str(), t.size()));
}

};      // namespace cjson
