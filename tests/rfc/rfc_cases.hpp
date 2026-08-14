//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// shared machinery for the rfc 8259 conformance suite: a {bytes, length, verdict, why}
// case record and a runner that drives one case through every STRICT entry point and
// requires them all to agree.
//
// lengths come from `const char (&)[N]`, never strlen, so a case may contain a raw 0x00 --
// several rfc cases must (s2 is `ws value ws` and 0x00 is not ws). tutil::view(const
// char *) cannot express those and is deliberately not used here.
//
// `discretionary` marks a case where the rfc permits either verdict (s8.1's byte order
// mark MAY, s4's duplicate-name SHOULD, s6/s9's range and depth limits). Those are still
// asserted -- pinning them is the whole point -- but the flag is what tests/rfc/
// COMPLIANCE.md is generated against, so a documented choice can never quietly become an
// undocumented one.
#pragma once

#include "../../src/cjson/cjson.hpp"

#include <snowball/snowball.hpp>

#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace rfc
{

enum class verdict : u8 { accept, reject };

struct kase {
  const char *p;
  usize n;
  verdict v;
  bool discretionary;
  const char *why;
};

// N - 1 drops the compiler's terminator and is correct across embedded nuls
template<usize N>
constexpr kase
K(const char (&s)[N], verdict v, const char *why) noexcept
{
  return kase{ s, N - 1, v, false, why };
}

template<usize N>
constexpr kase
KD(const char (&s)[N], verdict v, const char *why) noexcept
{
  return kase{ s, N - 1, v, true, why };
}

inline const u8 *
bytes_of(const kase &k) noexcept
{
  return reinterpret_cast<const u8 *>(k.p);
}

inline void
fail_ctx(const kase &k, const char *entry, const char *got) noexcept
{
  snowball::print("rfc case FAILED [", entry, "]: ", k.why);
  snowball::print("   wanted ", k.v == verdict::accept ? "accept" : "reject", ", got ", got);
  snowball::print("   length ", k.n);
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// the strict entry points. every one of these implements `JSON-text = ws value ws` in
// full and must return the same accept/reject answer for the same bytes.

inline void
expect(const kase &k) noexcept
{
  const bool want = (k.v == verdict::accept);
  const u8 *p = bytes_of(k);

  // 1. validate
  const cjson::error ve = cjson::validate(p, k.n);
  if ( (ve == cjson::error::ok) != want ) fail_ctx(k, "validate", cjson::error_name(ve));
  sb::require_true((ve == cjson::error::ok) == want);

  // 2. is_valid agrees with validate
  sb::require_true(cjson::is_valid(p, k.n) == (ve == cjson::error::ok));

  // 3. parse
  auto r = cjson::parse(p, k.n);
  if ( r.is_first() != want ) fail_ctx(k, "parse", r.is_first() ? "ok" : cjson::error_name(r.cast<cjson::error>()));
  sb::require_true(r.is_first() == want);

  // 4. parse_insitu over a writable copy carrying slack
  {
    micron::vector<u8> buf;
    buf.reserve(k.n + cjson::padding);
    for ( usize i = 0; i < k.n; ++i ) buf.push_back(p[i]);
    auto ri = cjson::parse_insitu(cjson::wbytes{ buf.begin(), k.n });
    if ( ri.is_first() != want ) fail_ctx(k, "parse_insitu", ri.is_first() ? "ok" : cjson::error_name(ri.cast<cjson::error>()));
    sb::require_true(ri.is_first() == want);
  }

  // 5. minify walks the same grammar fsm
  {
    micron::vector<u8> out;
    out.reserve(cjson::minify_bound(k.n) + cjson::padding);
    const max_t m = cjson::minify(cjson::bytes{ p, k.n }, cjson::wbytes{ out.begin(), cjson::minify_bound(k.n) });
    if ( (m >= 0) != want ) fail_ctx(k, "minify", m >= 0 ? "ok" : cjson::error_name(cjson::as_error(m)));
    sb::require_true((m >= 0) == want);
  }

  // 6. iterate is lenient BY DESIGN (rfc s9); opts::check_grammar makes it strict, and in
  //    that mode it must land on the same verdict as validate
  {
    cjson::scratch sc;
    auto rv = cjson::iterate(cjson::bytes{ p, k.n }, cjson::opts{ .check_grammar = true }, sc);
    if ( rv.is_first() != want ) fail_ctx(k, "iterate+check_grammar", rv.is_first() ? "ok" : cjson::error_name(rv.cast<cjson::error>()));
    sb::require_true(rv.is_first() == want);
  }
}

inline void
expect_all(const kase *ks, usize count) noexcept
{
  for ( usize i = 0; i < count; ++i ) expect(ks[i]);
}

template<usize N>
inline void
expect_all(const kase (&ks)[N]) noexcept
{
  expect_all(ks, N);
}

// how many of a table the rfc leaves to the implementation -- COMPLIANCE.md quotes this
template<usize N>
inline usize
discretionary_count(const kase (&ks)[N]) noexcept
{
  usize c = 0;
  for ( usize i = 0; i < N; ++i )
    if ( ks[i].discretionary ) ++c;
  return c;
}

};      // namespace rfc
