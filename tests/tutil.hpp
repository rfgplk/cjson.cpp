//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// shared cjson test utilities: whole-file slurp, byte views, deterministic rng
#pragma once

#include <micron/linux/io.hpp>
#include <micron/linux/io/ext.hpp>
#include <micron/slice.hpp>
#include <micron/std.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

#include "../src/cjson/config.hpp"

namespace tutil
{

inline micron::vector<u8>
slurp(const char *path)
{
  micron::vector<u8> out;
  micron::posix::fd_t fd = micron::posix::open_read(path);
  if ( !fd.open() ) return out;
  micron::stat_t st{};
  if ( micron::fstat(fd, st) < 0 or st.st_size <= 0 ) {
    micron::posix::close_fd(fd);
    return out;
  }
  out.reserve(static_cast<usize>(st.st_size) + 1);
  u8 buf[4096];
  for ( ;; ) {
    const max_t n = micron::posix::read(fd, buf, sizeof(buf));
    if ( n <= 0 ) break;
    for ( max_t i = 0; i < n; i++ ) out.push_back(buf[i]);
  }
  micron::posix::close_fd(fd);
  return out;
}

inline cjson::bytes
view(const micron::vector<u8> &v)
{
  return cjson::bytes{ v.cbegin(), v.size() };
}

inline cjson::bytes
view(const char *s)
{
  usize n = 0;
  while ( s[n] ) ++n;
  return cjson::bytes{ reinterpret_cast<const u8 *>(s), n };
}

// fixed-seed xorshift — never time-based
struct rng {
  u64 s = 0xa0761d6478bd642full;

  constexpr u64
  next() noexcept
  {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }

  constexpr u32
  below(u32 lim) noexcept
  {
    return lim == 0 ? 0 : static_cast<u32>(next() % lim);
  }
};

};      // namespace tutil
