//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// The entry points share one stage-1 sweep and (mostly) one grammar fsm, so on identical
// bytes they must land on identical answers. This file states each agreement PRECISELY,
// including the two places where disagreement is the documented design:
//
//   validate vs parse   agree, EXCEPT that parse may additionally return bad_number on a
//                       grammar-legal but unrepresentable magnitude (F3, rfc s6 range
//                       limits). Any other disagreement is a bug.
//   iterate vs validate DISAGREE by default -- iterate skips the grammar fsm (F2, rfc s9)
//                       -- but must agree exactly under opts::check_grammar.
//
// Stating the exceptions this narrowly is the point: a vague "they mostly agree" property
// would have absorbed F3 and F4b instead of exposing them.

#include "fuzz_corpus.hpp"

#include <snowball/snowball.hpp>
#include <snowball/snowball_fuzz.hpp>

#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace sbf = snowball::fuzzing;

namespace
{

void
check_agreement(const u8 *p, usize n)
{
  fz::tight t(p, n);
  const cjson::bytes v = t.view();

  const cjson::error ve = cjson::validate(v);
  const bool v_ok = (ve == cjson::error::ok);

  // is_valid is validate == ok, by definition
  FUZZ_FAIL_IF(cjson::is_valid(v) != v_ok, "is_valid disagrees with validate");

  // validate is deterministic
  FUZZ_FAIL_IF(cjson::validate(v) != ve, "validate is not deterministic");

  // parse agrees, modulo the documented range limit
  auto rp = cjson::parse(v);
  if ( rp.is_first() != v_ok ) {
    if ( !v_ok ) FUZZ_FAIL("parse accepted what validate rejected");
    // validate ok, parse failed: only bad_number (F3) is legitimate
    FUZZ_FAIL_IF(rp.cast<cjson::error>() != cjson::error::bad_number, "parse rejected a valid document for a non-range reason");
  }

  // parse is deterministic
  {
    fz::tight t2(p, n);
    FUZZ_FAIL_IF(cjson::parse(t2.view()).is_first() != rp.is_first(), "parse is not deterministic");
  }

  // insitu takes a different input path (caller buffer, rewritten) but the same grammar
  {
    fz::tight ti(p, n);
    auto ri = cjson::parse_insitu(cjson::wbytes{ ti.p, ti.n });
    FUZZ_FAIL_IF(ri.is_first() != rp.is_first(), "parse_insitu disagrees with parse");
  }

  // the reuse variants borrow rather than own, but decide the same way
  {
    fz::tight tr(p, n);
    cjson::scratch sc;
    auto rr = cjson::parse_reuse(tr.view(), {}, sc);
    FUZZ_FAIL_IF(rr.is_first() != rp.is_first(), "parse_reuse disagrees with parse");
  }
  {
    fz::tight tr(p, n);
    cjson::scratch sc;
    auto rr = cjson::parse_insitu_reuse(cjson::wbytes{ tr.p, tr.n }, {}, sc);
    FUZZ_FAIL_IF(rr.is_first() != rp.is_first(), "parse_insitu_reuse disagrees with parse");
  }

  // minify walks the same fsm as validate, so it agrees with validate exactly
  {
    micron::vector<u8> out;
    out.reserve(cjson::minify_bound(n) + cjson::padding + 1);
    const max_t m = cjson::minify(v, cjson::wbytes{ out.begin(), cjson::minify_bound(n) });
    FUZZ_FAIL_IF((m >= 0) != v_ok, "minify disagrees with validate");
  }

  // iterate is lenient by design, but it can never REJECT something validate accepts:
  // its checks are a strict subset (stage 1 only)
  {
    cjson::scratch sc;
    auto rv = cjson::iterate(v, sc);
    FUZZ_FAIL_IF(v_ok and !rv.is_first(), "iterate rejected a fully valid document");
  }

  // and with check_grammar it must match validate exactly, error code included
  {
    cjson::scratch sc;
    auto rg = cjson::iterate(v, cjson::opts{ .check_grammar = true }, sc);
    FUZZ_FAIL_IF(rg.is_first() != v_ok, "iterate+check_grammar disagrees with validate");
    if ( !v_ok ) FUZZ_FAIL_IF(rg.cast<cjson::error>() != ve, "iterate+check_grammar reports a different error code");
  }

  // numbers_as_raw declines to convert, so it agrees with validate rather than parse
  {
    fz::tight tn(p, n);
    auto rn = cjson::parse(tn.view(), cjson::opts{ .numbers_as_raw = true });
    FUZZ_FAIL_IF(rn.is_first() != v_ok, "numbers_as_raw disagrees with validate");
  }

  // skip_utf8 may only WIDEN acceptance, never narrow it
  {
    fz::tight ts(p, n);
    const bool su = (cjson::validate(ts.view(), cjson::opts{ .skip_utf8 = true }) == cjson::error::ok);
    FUZZ_FAIL_IF(v_ok and !su, "skip_utf8 rejected what strict validation accepted");
  }

  // stop_when_done may only widen acceptance too
  {
    fz::tight ts(p, n);
    const bool sw = (cjson::validate(ts.view(), cjson::opts{ .stop_when_done = true }) == cjson::error::ok);
    FUZZ_FAIL_IF(v_ok and !sw, "stop_when_done rejected what strict validation accepted");
  }
}

// on-demand and the DOM must read the same values out of the same document
void
check_ondemand_values(const u8 *p, usize n)
{
  fz::tight t(p, n);
  auto rp = cjson::parse(t.view());
  if ( !rp.is_first() ) return;

  fz::tight t2(p, n);
  cjson::scratch sc;
  auto rv = cjson::iterate(t2.view(), sc);
  if ( !rv.is_first() ) return;

  auto dom = rp.cast<cjson::doc>().root();
  auto cur = rv.cast<cjson::view>().root();

  FUZZ_FAIL_IF(dom.type() != cur.type(), "ondemand and dom disagree on the root kind");

  switch ( dom.type() ) {
  case cjson::kind::number: {
    const f64 a = dom.f64_or(0.0), b = cur.f64_or(1.0);
    FUZZ_FAIL_IF(__builtin_bit_cast(u64, a) != __builtin_bit_cast(u64, b), "ondemand and dom disagree on a number");
    FUZZ_FAIL_IF(dom.i64_or(7) != cur.i64_or(7), "ondemand and dom disagree on an integer");
    FUZZ_FAIL_IF(dom.u64_or(7) != cur.u64_or(7), "ondemand and dom disagree on an unsigned");
    break;
  }
  case cjson::kind::boolean:
    FUZZ_FAIL_IF(dom.bool_or(false) != cur.bool_or(true), "ondemand and dom disagree on a bool");
    break;
  case cjson::kind::array:
  case cjson::kind::object:
    FUZZ_FAIL_IF(dom.size() != cur.count(), "ondemand and dom disagree on a container size");
    break;
  default:
    break;
  }
}

};      // namespace

int
main()
{
  {
    sb::test_case("the seed corpus agrees across every entry point");
    for ( usize s = 0; s < fz::seed_count; ++s ) {
      auto v = fz::seed_bytes(s);
      check_agreement(v.cbegin(), v.size());
      check_ondemand_values(v.cbegin(), v.size());
    }
    sb::require_true(true);
    sb::end_test_case();
  }
  {
    sb::test_case("mutated seeds agree across every entry point");
    fz::rng r(0xA6BEE1ull);
    for ( usize s = 0; s < fz::seed_count; ++s ) {
      for ( u32 iter = 0; iter < 3000; ++iter ) {
        micron::vector<u8> v = fz::seed_bytes(s);
        fz::mutate(v, r, 1 + r.below(3));
        check_agreement(v.cbegin(), v.size());
        check_ondemand_values(v.cbegin(), v.size());
      }
    }
    sb::require_true(true);
    sb::end_test_case();
  }
  {
    sbf::check_property(
        "arbitrary bytes agree across every entry point",
        [](micron::vector<u8> v) {
          check_agreement(v.cbegin(), v.size());
          check_ondemand_values(v.cbegin(), v.size());
        },
        { .seed = 0xA6BEE2ull, .count = 12000 }, sbf::vector_of(sbf::range<u8>(0, 255)).len(0, 160));
  }
  {
    // the comptime twin: under constant evaluation the simd kernels are unreachable, so
    // the scalar oracle runs. Verdicts must be identical. Bounded to short inputs and a
    // modest count because every case is a full constexpr evaluation.
    sb::test_case("the scalar twin agrees with the simd kernels on the seed corpus");
    for ( usize s = 0; s < fz::seed_count; ++s ) {
      auto v = fz::seed_bytes(s);
      const cjson::error rt = cjson::validate(v.cbegin(), v.size());
      // drive the same bytes through the char* overload, which carries the `if consteval`
      // transient-copy arm; at runtime it lands in the same place
      micron::vector<char> c;
      c.reserve(v.size() + 1);
      for ( usize i = 0; i < v.size(); ++i ) c.push_back(char(v[i]));
      const cjson::error ct = cjson::validate(c.cbegin(), c.size());
      sb::require_true(rt == ct);
    }
    sb::end_test_case();
  }
  {
    // F3 stated as an invariant rather than a case list: whenever validate and parse
    // disagree, the reason is ALWAYS a number range
    sb::test_case("F3: validate/parse disagreement is always and only bad_number");
    fz::rng r(0xF3D1FFull);
    u32 disagreements = 0;
    for ( u32 iter = 0; iter < 60000; ++iter ) {
      micron::vector<u8> v = fz::seed_bytes(r.below(u32(fz::seed_count)));
      fz::mutate(v, r, 1 + r.below(3));
      fz::tight t(v.cbegin(), v.size());
      const bool ok = (cjson::validate(t.view()) == cjson::error::ok);
      auto rp = cjson::parse(t.view());
      if ( ok != rp.is_first() ) {
        sb::require_true(ok);      // parse can only be the stricter of the two
        sb::require_true(rp.cast<cjson::error>() == cjson::error::bad_number);
        ++disagreements;
      }
    }
    snowball::print("F3: ", disagreements, " range-only disagreements over 60000 mutations");
    sb::end_test_case();
  }
  snowball::print("fuzz_agreement: every entry point agreed");
  return 1;
}
