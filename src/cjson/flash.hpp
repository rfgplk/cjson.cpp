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

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// to disk through io_uring

#include "doc.hpp"
#include "error.hpp"
#include "value.hpp"
#include "write.hpp"

#include <micron/io/flash.hpp>
#include <micron/types.hpp>

namespace cjson::flash
{

namespace mf = micron::io::flash;

inline constexpr u32 max_chunks = 16;

struct sink_opts {
  u32 chunks = 4;                   // buffers in flight; clamped to [2, max_chunks]
  usize chunk = 256 * 1024;         // per-buffer size (page-aligned by the engine)
  bool fsync_on_close = false;      // fdatasync in drain()
  u32 queue_depth = 64;             // sq entries
};

class sink
{
  mf::engine __eng;
  mf::pool_buf __bufs[max_chunks];
  u32 __state[max_chunks]{};      // 0 = free, 1 = in flight
  u32 __nbufs = 0;
  u32 __cur = 0;
  usize __chunk = 0;
  usize __fill = 0;       // bytes staged in __bufs[__cur]
  u64 __off = 0;          // next file offset
  u64 __written = 0;      // bytes handed to the kernel and confirmed
  u32 __inflight = 0;
  i32 __fd = -1;
  bool __sync = false;
  error __err = error::ok;

  // overflow slab
  wbuf __big;

  void
  __fail(error e) noexcept
  {
    if ( __err == error::ok ) __err = e;
  }

  bool
  __reap_one() noexcept
  {
    if ( __inflight == 0 ) return true;
    bool ok = true;
    const i32 r = mf::drain(__eng, 1, [&](u64 ud, i32 res) {
      const u32 slot = u32(ud & 0xffff);
      const u32 want = u32(ud >> 16);
      if ( slot < __nbufs ) __state[slot] = 0;
      if ( res < 0 or u32(res) != want )
        ok = false;
      else
        __written += u64(res);
    });
    --__inflight;
    if ( r < 0 or !ok ) {
      __fail(error::io_error);
      return false;
    }
    return true;
  }

  bool
  __flush_cur() noexcept
  {
    if ( __fill == 0 ) return true;
    const u64 ud = u64(__cur) | (u64(__fill) << 16);
    if ( mf::queue_write(__eng, __fd, __bufs[__cur].data, u32(__fill), __off, ud) < 0 ) [[unlikely]] {
      __fail(error::io_error);
      return false;
    }
    if ( mf::submit(__eng) < 0 ) [[unlikely]] {
      __fail(error::io_error);
      return false;
    }
    __state[__cur] = 1;
    ++__inflight;
    __off += u64(__fill);
    __fill = 0;

    for ( u32 i = 1; i <= __nbufs; ++i ) {
      const u32 c = (__cur + i) % __nbufs;
      if ( __state[c] == 0 ) {
        __cur = c;
        return true;
      }
    }
    if ( !__reap_one() ) return false;
    for ( u32 c = 0; c < __nbufs; ++c )
      if ( __state[c] == 0 ) {
        __cur = c;
        return true;
      }
    __fail(error::io_error);
    return false;
  }

public:
  sink(const sink &) = delete;
  sink &operator=(const sink &) = delete;
  sink(sink &&) = delete;
  sink &operator=(sink &&) = delete;

  explicit sink(i32 fd, sink_opts o = {}) noexcept
  {
    if ( o.chunks < 2 ) o.chunks = 2;
    if ( o.chunks > max_chunks ) o.chunks = max_chunks;
    mf::engine_opts eo{};
    eo.entries = o.queue_depth;
    eo.fixed_bufs = o.chunks;
    eo.fixed_buf_size = o.chunk;
    if ( __eng.init(eo) < 0 or !__eng.live() ) {
      __fail(error::io_error);
      return;
    }
    for ( u32 i = 0; i < o.chunks; ++i ) {
      auto b = mf::acquire_buf(__eng);
      if ( !b.is_first() ) break;
      __bufs[i] = micron::move(b.template cast<mf::pool_buf>());
      ++__nbufs;
    }
    if ( __nbufs < 2 ) {
      __fail(error::io_error);
      return;
    }
    __chunk = __bufs[0].cap;
    __fd = fd;
    __sync = o.fsync_on_close;
  }

  explicit sink(mf::file &f, sink_opts o = {}) noexcept : sink(f.raw_fd(), o) { }

  ~sink() { (void)drain(); }

  [[nodiscard]] error
  err() const noexcept
  {
    return __err;
  }

  [[nodiscard]] bool
  ok() const noexcept
  {
    return __err == error::ok;
  }

  [[nodiscard]] usize
  chunk_size() const noexcept
  {
    return __chunk;
  }

  [[nodiscard]] u64
  written() const noexcept
  {
    return __written;
  }

  [[nodiscard]] u8 *
  reserve(usize n) noexcept
  {
    if ( __err != error::ok ) return nullptr;
    if ( n > __chunk ) return nullptr;
    if ( __fill + n > __chunk and !__flush_cur() ) return nullptr;
    return reinterpret_cast<u8 *>(__bufs[__cur].data) + __fill;
  }

  void
  commit(usize n) noexcept
  {
    __fill += n;
  }

  bool
  put(const u8 *p, usize n) noexcept
  {
    while ( n != 0 ) {
      if ( __err != error::ok ) return false;
      if ( __fill == __chunk and !__flush_cur() ) return false;
      const usize take = (__chunk - __fill) < n ? (__chunk - __fill) : n;
      __copy(reinterpret_cast<u8 *>(__bufs[__cur].data) + __fill, p, take);
      __fill += take;
      p += take;
      n -= take;
    }
    return true;
  }

  bool
  put1(u8 c) noexcept
  {
    if ( __err != error::ok ) return false;
    if ( __fill == __chunk and !__flush_cur() ) return false;
    reinterpret_cast<u8 *>(__bufs[__cur].data)[__fill++] = c;
    return true;
  }

  bool
  __put_oversized(val v, style st, usize bound) noexcept
  {
    if ( !__big.reserve(bound) ) {
      __fail(error::oom);
      return false;
    }
    u8 *base = __big.data();
    u8 *e = __write::emit(base, v.__raw(), v.__owner()->pool(), st);
    return put(base, usize(e - base));
  }

  max_t
  drain() noexcept
  {
    if ( __fd < 0 ) return fail(__err == error::ok ? error::io_error : __err);
    if ( __fill != 0 ) (void)__flush_cur();
    while ( __inflight != 0 )
      if ( !__reap_one() ) break;
    if ( __sync and __err == error::ok ) {
      if ( mf::fdatasync(__fd, __eng) < 0 ) __fail(error::io_error);
    }
    if ( __err != error::ok ) return fail(__err);
    return max_t(__written);
  }
};

namespace __fl
{

inline bool
one(sink &s, val v, style st, usize extra) noexcept
{
  const usize b = write_bound(v, st);
  if ( b + extra <= s.chunk_size() ) {
    u8 *w = s.reserve(b + extra);
    if ( !w ) return s.__put_oversized(v, st, b);
    u8 *e = __write::emit(w, v.__raw(), v.__owner()->pool(), st);
    s.commit(usize(e - w));
    return true;
  }
  return s.__put_oversized(v, st, b);
}

};      // namespace __fl

inline max_t
write_to(const doc &d, sink &s, style st = {})
{
  if ( !d.alive() ) return fail(error::empty_input);
  const val root = d.root();
  const usize whole = write_bound(d, st);

  if ( whole <= s.chunk_size() ) {
    u8 *w = s.reserve(whole);
    if ( !w ) return fail(s.err() == error::ok ? error::io_error : s.err());
    const max_t n = write_into(d, wbytes{ w, whole }, st);
    if ( n < 0 ) return n;
    s.commit(usize(n));
    return max_t(n);
  }

  const kind k = get_kind(*root.__raw());
  if ( k != kind::array and k != kind::object ) {
    if ( !s.__put_oversized(root, st, whole) ) return fail(s.err());
    return max_t(s.written());
  }

  const bool obj = k == kind::object;
  const u8 open = u8('[') | (u8(obj) << 5);
  if ( !s.put1(open) ) return fail(s.err());

  bool first = true;
  if ( obj ) {
    for ( const member m : root.members() ) {
      if ( !first and !s.put1(u8(',')) ) return fail(s.err());
      first = false;
      u8 *kw = s.reserve(m.key.len * 6 + 2);
      if ( !kw ) return fail(s.err() == error::ok ? error::short_output : s.err());
      u8 *ke = __write::write_string_escaped(kw, reinterpret_cast<const u8 *>(m.key.ptr), m.key.len);
      s.commit(usize(ke - kw));
      if ( !s.put1(u8(':')) ) return fail(s.err());
      if ( !__fl::one(s, m.v, st, 1) ) return fail(s.err());
    }
  } else {
    for ( const val e : root.items() ) {
      if ( !first and !s.put1(u8(',')) ) return fail(s.err());
      first = false;
      if ( !__fl::one(s, e, st, 1) ) return fail(s.err());
    }
  }
  if ( !s.put1(u8(open + 2)) ) return fail(s.err());      // ']' = '[' + 2, '}' = '{' + 2
  return max_t(s.written());
}

inline max_t
write_to(val v, sink &s, style st = {})
{
  if ( !v ) return fail(error::empty_input);
  if ( !__fl::one(s, v, st, 0) ) return fail(s.err());
  return max_t(s.written());
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// writers
//
// rough performance (ryzen 7 3700u)
//  .emit-only (no file)      16,853,129     2.00 GB/s
//  .write_file (buffered)    16,854,613     0.77 GB/s
//  .write_to  (sink)         36,944,296     0.38 GB/s
inline max_t
write_file(const doc &d, const micron::io::path_t &p, style st = {})
{
  mf::file f(p, micron::io::modes::readwritecreate);
  if ( !f.valid() ) return fail(error::io_error);
  wbuf b;
  const max_t w = write_into(d, b, st);
  if ( w < 0 ) return w;
  const max_t r = mf::pwrite(f.raw_fd(), b.data(), usize(w), 0);
  if ( r < 0 or r != w ) return fail(error::io_error);
  return w;
}

inline max_t
write_file(const doc &d, const micron::io::path_t &p, wbuf &b, style st = {})
{
  mf::file f(p, micron::io::modes::readwritecreate);
  if ( !f.valid() ) return fail(error::io_error);
  const max_t w = write_into(d, b, st);
  if ( w < 0 ) return w;
  const max_t r = mf::pwrite(f.raw_fd(), b.data(), usize(w), 0);
  if ( r < 0 or r != w ) return fail(error::io_error);
  return w;
}

inline max_t
stream_file(const doc &d, const micron::io::path_t &p, style st = {}, sink_opts o = {})
{
  mf::file f(p, micron::io::modes::readwritecreate);
  if ( !f.valid() ) return fail(error::io_error);
  sink s(f.raw_fd(), o);
  if ( !s.ok() ) return fail(s.err());
  const max_t w = write_to(d, s, st);
  const max_t dr = s.drain();
  if ( w < 0 ) return w;
  return dr;
}

};      // namespace cjson::flash
