//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// shared harness for tests/fuzz/: a seed corpus, byte mutators, and the exact-size heap
// copy that turns "read past len" into an asan report instead of a silent pass.
//
// Two rules inherited from micron and from tests/fuzz_parse.cpp:
//
//  1. SEEDS ARE FIXED HEX LITERALS. snowball::fuzzing::run_config{}.seed == 0 falls back
//     to rdtsc and the test stops being reproducible. Every check_property call in this
//     directory passes an explicit seed.
//  2. THE EXACT-SIZE COPY IS NOT OPTIONAL. cjson's copy mode pads its own pool, so a
//     bug that reads past the caller's buffer is invisible when the input lives in a
//     comfortably-sized vector. Every entry point is fed a tight allocation.
//
// abcmalloc is mmap-based, so asan puts no redzone around abc::malloc. For the paths
// that must be proven byte-exact rather than merely non-crashing, guarded() places the
// document so its last byte abuts a PROT_NONE page -- an over-read is then a SIGSEGV
// rather than a maybe.
#pragma once

#include "../../src/cjson/cjson.hpp"

#include <snowball/snowball.hpp>

#include <micron/cmalloc.hpp>
#include <micron/memory/mman.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace fz
{

#define FUZZ_FAIL(msg) throw("fuzz: " msg)
#define FUZZ_FAIL_IF(cond, msg)                                                                                                            \
  do {                                                                                                                                     \
    if ( cond ) throw("fuzz: " msg);                                                                                                       \
  } while ( 0 )

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// seed corpus: shapes that reach different parts of the engine

inline const char *const seeds[] = {
  "{}",
  "[]",
  "null",
  "0",
  "-1.5e-10",
  "\"\"",
  R"({"a":1})",
  R"([1,2,3,4,5,6,7,8,9,10])",
  R"({"a":{"b":{"c":{"d":[1,2,{"e":null}]}}}})",
  R"({"esc":"\"\\\/\b\f\n\r\tAé𝄞"})",
  R"([0,-0,1e308,1e-308,9007199254740991,18446744073709551615,-9223372036854775808])",
  R"({"utf8":"aéあ𝄞","del":"","empty":""})",
  R"([[[[[[[[[[1]]]]]]]]]])",
  R"({"k1":true,"k1":false,"k1":null})",
  R"([{"a":[{"b":[{"c":[]}]}]}])",
  R"({"long":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"})",
  R"([1.7976931348623157e308,5e-324,2.2250738585072014e-308,0.1,0.2,0.3])",
  R"({"ws"  :  [ 1 , 2 ] ,  "x" :  {  }  })",
  R"([4539183550709394473162714279012,123456789012345678901234567890])",
  "\xef\xbb\xbf{}",
};

inline constexpr usize seed_count = sizeof(seeds) / sizeof(seeds[0]);

inline usize
slen(const char *s) noexcept
{
  usize n = 0;
  while ( s[n] ) ++n;
  return n;
}

inline micron::vector<u8>
seed_bytes(usize i)
{
  micron::vector<u8> v;
  const char *s = seeds[i % seed_count];
  const usize n = slen(s);
  v.reserve(n + 1);
  for ( usize k = 0; k < n; ++k ) v.push_back(u8(s[k]));
  return v;
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// the exact-size heap copy. lifted from tests/fuzz_parse.cpp, which is where the idiom
// was established -- a tight allocation is what makes an over-read reportable.

struct tight {
  u8 *p = nullptr;
  usize n = 0;

  tight(const u8 *src, usize len) : n(len)
  {
    p = static_cast<u8 *>(abc::malloc(len ? len : 1));
    for ( usize i = 0; i < len; ++i ) p[i] = src[i];
  }

  ~tight()
  {
    if ( p ) abc::free(p);
  }

  tight(const tight &) = delete;
  tight &operator=(const tight &) = delete;

  cjson::bytes
  view() const noexcept
  {
    return cjson::bytes{ p, n };
  }
};

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// guard-page placement: the document's LAST byte is the last readable byte before a
// PROT_NONE page, so any read past len faults instead of quietly succeeding

struct guarded {
  u8 *base = nullptr;
  u8 *doc = nullptr;
  usize n = 0;

  static constexpr usize page = 4096;

  guarded(const u8 *src, usize len) : n(len)
  {
    if ( len > page ) return;      // callers keep documents small; nullptr means "skip"
    addr_t *m = micron::mmap(nullptr, 2 * page, micron::prot_read | micron::prot_write, micron::map_private | micron::map_anonymous, -1, 0);
    base = reinterpret_cast<u8 *>(m);
    if ( micron::mprotect(reinterpret_cast<addr_t *>(base + page), page, micron::prot_none) != 0 ) {
      base = nullptr;
      return;
    }
    doc = base + page - len;
    for ( usize i = 0; i < len; ++i ) doc[i] = src[i];
  }

  ~guarded()
  {
    if ( base ) micron::munmap(reinterpret_cast<addr_t *>(base), 2 * page);
  }

  guarded(const guarded &) = delete;
  guarded &operator=(const guarded &) = delete;

  bool
  ok() const noexcept
  {
    return doc != nullptr;
  }

  cjson::bytes
  view() const noexcept
  {
    return cjson::bytes{ doc, n };
  }
};

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// mutators. bytes that matter to a json parser, biased toward the structural alphabet.

inline const u8 hostile[]
    = { u8('"'),  u8('\\'), u8('{'),  u8('}'),  u8('['), u8(']'), u8(':'), u8(','), u8(0x00), u8(0x1f), u8(0x20), u8(0x7f), u8(0x80),
        u8(0xc0), u8(0xed), u8(0xf5), u8(0xff), u8('e'), u8('u'), u8('0'), u8('-'), u8('+'),  u8('.'),  u8('t'),  u8('n'),  u8('f') };
inline constexpr usize hostile_n = sizeof(hostile) / sizeof(hostile[0]);

// a tiny deterministic prng so mutation does not depend on snowball's generator plumbing
struct rng {
  u64 s;

  constexpr explicit rng(u64 seed) noexcept : s(seed ? seed : 0x9e3779b97f4a7c15ull) { }

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
    return lim == 0 ? 0 : u32(next() % lim);
  }
};

inline void
mutate(micron::vector<u8> &v, rng &r, u32 rounds)
{
  for ( u32 k = 0; k < rounds; ++k ) {
    if ( v.size() == 0 ) {
      v.push_back(hostile[r.below(hostile_n)]);
      continue;
    }
    switch ( r.below(5) ) {
    case 0:      // bit flip
      v[r.below(u32(v.size()))] ^= u8(1u << r.below(8));
      break;
    case 1:      // overwrite with a structurally interesting byte
      v[r.below(u32(v.size()))] = hostile[r.below(hostile_n)];
      break;
    case 2:      // truncate
      if ( v.size() > 1 ) v.set_size(1 + r.below(u32(v.size() - 1)));
      break;
    case 3: {      // splice a prefix of another seed
      auto other = seed_bytes(r.below(u32(seed_count)));
      const u32 take = r.below(u32(other.size() + 1));
      for ( u32 i = 0; i < take; ++i ) v.push_back(other[i]);
      break;
    }
    case 4:      // insert a run of one byte
    default: {
      const u8 c = hostile[r.below(hostile_n)];
      const u32 run = 1 + r.below(8);
      for ( u32 i = 0; i < run; ++i ) v.push_back(c);
      break;
    }
    }
  }
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// comparing two documents without leaning on the writer being correct: walk both and
// compare kinds, sizes, names and scalar payloads.

inline bool
same_value(cjson::val a, cjson::val b, u32 depth = 0)
{
  if ( depth > 64 ) return true;      // deep enough; the shape already matched
  if ( a.type() != b.type() ) return false;
  switch ( a.type() ) {
  case cjson::kind::null:
    return true;
  case cjson::kind::boolean:
    return a.bool_or(false) == b.bool_or(true);
  case cjson::kind::number: {
    const f64 x = a.f64_or(0.0), y = b.f64_or(1.0);
    return __builtin_bit_cast(u64, x) == __builtin_bit_cast(u64, y);
  }
  case cjson::kind::string: {
    auto x = a.str_or(), y = b.str_or();
    if ( x.len != y.len ) return false;
    for ( usize i = 0; i < x.len; ++i )
      if ( x.ptr[i] != y.ptr[i] ) return false;
    return true;
  }
  case cjson::kind::array: {
    if ( a.size() != b.size() ) return false;
    for ( usize i = 0; i < a.size(); ++i )
      if ( !same_value(a.at(i), b.at(i), depth + 1) ) return false;
    return true;
  }
  case cjson::kind::object: {
    if ( a.size() != b.size() ) return false;
    auto ia = a.members().begin();
    auto ib = b.members().begin();
    const auto ea = a.members().end();
    const auto eb = b.members().end();
    while ( ia != ea and ib != eb ) {
      const auto ma = *ia;
      const auto mb = *ib;
      if ( ma.key.len != mb.key.len ) return false;
      for ( usize i = 0; i < ma.key.len; ++i )
        if ( ma.key.ptr[i] != mb.key.ptr[i] ) return false;
      if ( !same_value(ma.v, mb.v, depth + 1) ) return false;
      ++ia;
      ++ib;
    }
    return true;
  }
  default:
    return true;
  }
}

};      // namespace fz
