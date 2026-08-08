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
#include "stage1.hpp"
#include "value.hpp"

#include <micron/cmalloc.hpp>
#include <micron/types.hpp>

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// scratch
//
// reusable parser arena

namespace cjson
{

struct scratch {
  u32 *idx = nullptr;
  usize idx_cap = 0;      // in u32 slots
  u8 *pool = nullptr;
  usize pool_cap = 0;      // in bytes; a parse of len needs len + padding
  value *vals = nullptr;
  usize vals_cap = 0;      // in value slots

  constexpr ~scratch() { release(); }

  scratch() = default;
  scratch(const scratch &) = delete;
  scratch &operator=(const scratch &) = delete;

  constexpr scratch(scratch &&o) noexcept
      : idx(o.idx), idx_cap(o.idx_cap), pool(o.pool), pool_cap(o.pool_cap), vals(o.vals), vals_cap(o.vals_cap)
  {
    o.idx = nullptr;
    o.idx_cap = 0;
    o.pool = nullptr;
    o.pool_cap = 0;
    o.vals = nullptr;
    o.vals_cap = 0;
  }

  constexpr scratch &
  operator=(scratch &&o) noexcept
  {
    release();
    idx = o.idx;
    idx_cap = o.idx_cap;
    pool = o.pool;
    pool_cap = o.pool_cap;
    vals = o.vals;
    vals_cap = o.vals_cap;
    o.idx = nullptr;
    o.idx_cap = 0;
    o.pool = nullptr;
    o.pool_cap = 0;
    o.vals = nullptr;
    o.vals_cap = 0;
    return *this;
  }

  constexpr bool
  ensure(usize n) noexcept
  {
    const usize want = __scan::index_slots(n);
    if ( want <= idx_cap ) return true;
    __release_idx();
    if consteval {
      idx = new u32[want];
    } else {
      idx = static_cast<u32 *>(abc::malloc(want * sizeof(u32)));
      if ( !idx ) return false;
    }
    idx_cap = want;
    return true;
  }

  constexpr bool
  ensure_pool(usize need) noexcept
  {
    if ( need <= pool_cap ) [[likely]]
      return true;
    usize ncap = pool_cap + pool_cap / 2;
    if ( ncap < need ) ncap = need;
    if ( ncap < 256 ) ncap = 256;
    __release_pool();
    if consteval {
      pool = new u8[ncap];
    } else {
      pool = static_cast<u8 *>(abc::malloc(ncap));
      if ( !pool ) return false;
    }
    pool_cap = ncap;
    return true;
  }

  constexpr bool
  ensure_vals(usize need) noexcept
  {
    if ( need <= vals_cap ) [[likely]]
      return true;
    usize ncap = vals_cap + vals_cap / 2;
    if ( ncap < need ) ncap = need;
    if ( ncap < 16 ) ncap = 16;
    __release_vals();
    if consteval {
      vals = new value[ncap]{};
    } else {
      vals = static_cast<value *>(abc::malloc(ncap * sizeof(value)));
      if ( !vals ) return false;
    }
    vals_cap = ncap;
    return true;
  }

  constexpr void
  release() noexcept
  {
    __release_idx();
    __release_pool();
    __release_vals();
  }

  constexpr void
  __release_idx() noexcept
  {
    if ( !idx ) return;
    if consteval {
      delete[] idx;
    } else {
      abc::free(idx);
    }
    idx = nullptr;
    idx_cap = 0;
  }

  constexpr void
  __release_pool() noexcept
  {
    if ( !pool ) return;
    if consteval {
      delete[] pool;
    } else {
      abc::free(pool);
    }
    pool = nullptr;
    pool_cap = 0;
  }

  constexpr void
  __release_vals() noexcept
  {
    if ( !vals ) return;
    if consteval {
      delete[] vals;
    } else {
      abc::free(vals);
    }
    vals = nullptr;
    vals_cap = 0;
  }
};

};      // namespace cjson
