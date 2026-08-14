//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// arbitrary bytes and corpus mutations through EVERY entry point: parse, parse_insitu,
// parse_reuse, parse_insitu_reuse, validate, is_valid, iterate, process, minify.
//
// The oracle here is survival, not a verdict -- the assertion is that nothing crashes,
// hangs, or reads out of bounds no matter what it is handed. tests/fuzz_parse.cpp already
// does this for `validate` alone; this widens it to the whole surface, and the real gate
// is the --asan / --ubsan arm of tests/fuzz/fuzz.duck.
//
// Also carries the two F4 regressions, which are the reason the guard-page helper exists:
//   F4a  process() on a failed index must return an EMPTY view, not one with a negative
//        index count
//   F4b  read_number must not scan past `len` -- iterate hands it the caller's raw,
//        unpadded bytes, and before the fix a long-significand number at the end of an
//        exactly-sized buffer both faulted and changed the parsed value

#include "fuzz_corpus.hpp"

#include <snowball/snowball.hpp>
#include <snowball/snowball_fuzz.hpp>

#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace sbf = snowball::fuzzing;

namespace
{

volatile usize sink = 0;      // keeps borrowed reads from being folded away under -Ofast

// drive one byte run through every entry point out of a TIGHT allocation
void
all_entry_points(const u8 *p, usize n)
{
  {
    fz::tight t(p, n);
    (void)cjson::validate(t.view());
    (void)cjson::is_valid(t.view());
  }
  {
    fz::tight t(p, n);
    auto r = cjson::parse(t.view());
    if ( r.is_first() ) sink = sink + r.cast<cjson::doc>().size();
  }
  {
    fz::tight t(p, n);
    auto r = cjson::parse(t.view(), cjson::opts{ .numbers_as_raw = true });
    if ( r.is_first() ) sink = sink + r.cast<cjson::doc>().size();
  }
  {
    fz::tight t(p, n);
    auto r = cjson::parse(t.view(), cjson::opts{ .stop_when_done = true });
    if ( r.is_first() ) sink = sink + r.cast<cjson::doc>().consumed();
  }
  {
    // insitu rewrites its input, so it gets its own copy
    fz::tight t(p, n);
    auto r = cjson::parse_insitu(cjson::wbytes{ t.p, t.n });
    if ( r.is_first() ) sink = sink + r.cast<cjson::doc>().size();
  }
  {
    fz::tight t(p, n);
    cjson::scratch sc;
    auto r = cjson::parse_reuse(t.view(), {}, sc);
    if ( r.is_first() ) sink = sink + r.cast<cjson::doc>().size();
  }
  {
    fz::tight t(p, n);
    cjson::scratch sc;
    auto r = cjson::parse_insitu_reuse(cjson::wbytes{ t.p, t.n }, {}, sc);
    if ( r.is_first() ) sink = sink + r.cast<cjson::doc>().size();
  }
  {
    // iterate BORROWS the caller's bytes and never pads them -- this is the arm that
    // caught F4b, so every scalar getter is exercised, not just the walk
    fz::tight t(p, n);
    cjson::scratch sc;
    auto r = cjson::iterate(t.view(), sc);
    if ( r.is_first() ) {
      auto root = r.cast<cjson::view>().root();
      sink = sink + usize(root.type());
      sink = sink + usize(root.i64_or(0));
      sink = sink + usize(root.u64_or(0));
      sink = sink + usize(root.f64_or(0.0) != 0.0);
      sink = sink + root.str_raw().len;
      sink = sink + root.count();
      // walk one level, which is where the cursor does most of its arithmetic
      if ( root.type() == cjson::kind::array ) {
        u32 guard = 0;
        for ( auto e : root.items() ) {
          sink = sink + usize(e.type()) + usize(e.i64_or(0)) + e.str_raw().len;
          if ( ++guard > 256 ) break;
        }
      } else if ( root.type() == cjson::kind::object ) {
        u32 guard = 0;
        for ( auto m : root.members() ) {
          sink = sink + m.key.len + usize(m.v.type()) + usize(m.v.i64_or(0));
          if ( ++guard > 256 ) break;
        }
      }
    }
  }
  {
    fz::tight t(p, n);
    cjson::scratch sc;
    auto r = cjson::iterate(t.view(), cjson::opts{ .check_grammar = true }, sc);
    if ( r.is_first() ) sink = sink + usize(r.cast<cjson::view>().root().type());
  }
  {
    // the no-error-handling direct API. F4a lived here.
    fz::tight t(p, n);
    cjson::scratch sc;
    cjson::view v = cjson::process(t.view(), sc);
    if ( v.alive() ) sink = sink + usize(v.root().type());
  }
  {
    fz::tight t(p, n);
    micron::vector<u8> out;
    out.reserve(cjson::minify_bound(n) + cjson::padding + 1);
    const max_t m = cjson::minify(t.view(), cjson::wbytes{ out.begin(), cjson::minify_bound(n) });
    if ( m > 0 ) sink = sink + usize(m);
  }
}

};      // namespace

int
main()
{
  {
    sb::test_case("mutated seeds survive every entry point");
    fz::rng r(0xF0071E5Eull);
    for ( usize s = 0; s < fz::seed_count; ++s ) {
      for ( u32 iter = 0; iter < 2000; ++iter ) {
        micron::vector<u8> v = fz::seed_bytes(s);
        fz::mutate(v, r, 1 + r.below(4));
        all_entry_points(v.cbegin(), v.size());
      }
    }
    sb::require_true(true);
    sb::end_test_case();
  }
  {
    sb::test_case("uniform random bytes survive every entry point");
    fz::rng r(0xBADC0FFEEull);
    for ( u32 iter = 0; iter < 20000; ++iter ) {
      const u32 n = r.below(200);
      micron::vector<u8> v;
      v.reserve(n + 1);
      for ( u32 i = 0; i < n; ++i ) v.push_back(u8(r.below(256)));
      all_entry_points(v.cbegin(), v.size());
    }
    sb::require_true(true);
    sb::end_test_case();
  }
  {
    // snowball's own generator, so the kit drives the corpus as well as our mutator.
    // alpha::full never emits 0x00 and alpha::utf8 aliases it, so raw bytes come from
    // vector_of(range<u8>) -- which is exactly what a json parser needs to see.
    sbf::check_property(
        "arbitrary byte vectors never crash any entry point", [](micron::vector<u8> v) { all_entry_points(v.cbegin(), v.size()); },
        { .seed = 0x0C7B17E5ull, .count = 20000 }, sbf::vector_of(sbf::range<u8>(0, 255)).len(0, 192));

    sbf::check_property(
        "structural-alphabet soup never crashes any entry point",
        [](micron::vector<u8> idx) {
          micron::vector<u8> v;
          v.reserve(idx.size() + 1);
          for ( usize i = 0; i < idx.size(); ++i ) v.push_back(fz::hostile[idx[i] % fz::hostile_n]);
          all_entry_points(v.cbegin(), v.size());
        },
        { .seed = 0x5717C7A1ull, .count = 20000 }, sbf::vector_of(sbf::range<u8>(0, 255)).len(0, 128));
  }
  {
    // F4a: a failed index must produce an empty view, never one carrying a negative count
    sb::test_case("F4a: process() returns an empty view when the index fails");
    const u8 bad_utf8[] = { u8('['), u8('"'), 0xff, 0xfe, u8('"'), u8(']') };
    cjson::scratch sc;
    sb::require_false(cjson::process(cjson::bytes{ bad_utf8, 6 }, sc).alive());

    const u8 unterminated[] = { u8('['), u8('"'), u8('a') };
    cjson::scratch sc2;
    sb::require_false(cjson::process(cjson::bytes{ unterminated, 3 }, sc2).alive());

    cjson::scratch sc3;
    sb::require_false(cjson::process(cjson::bytes{ reinterpret_cast<const u8 *>(""), 0 }, sc3).alive());

    // and a good document still works through the same call
    cjson::scratch sc4;
    cjson::view v = cjson::process(cjson::bytes{ reinterpret_cast<const u8 *>(R"({"a":1})"), 7 }, sc4);
    sb::require_true(v.alive());
    sb::require(v.root()["a"].i64_or(-1), static_cast<i64>(1));
    sb::end_test_case();
  }
  {
    // F4b: the number reader must not touch a byte past `len`. Against a guard page an
    // over-read is a SIGSEGV, so surviving IS the assertion; the value check that follows
    // proves the bound did not also change the answer.
    sb::test_case("F4b: iterate never reads past the caller's buffer");
    const char *hard[] = {
      "4539183550709394473162714279012",
      "123456789012345678901234567890",
      "0.1234567890123456789012345678901234567890",
      "9007199254740993",
      "1.7976931348623157",
      "2.2250738585072011",
    };
    for ( const char *h : hard ) {
      const usize n = fz::slen(h);
      fz::guarded g(reinterpret_cast<const u8 *>(h), n);
      sb::require_true(g.ok());

      cjson::scratch sc;
      auto ri = cjson::iterate(g.view(), sc);
      sb::require_true(ri.is_first());
      const f64 a = ri.cast<cjson::view>().root().f64_or(-1.0);

      auto rp = cjson::parse(reinterpret_cast<const u8 *>(h), n);
      sb::require_true(rp.is_first());
      const f64 b = rp.cast<cjson::doc>().root().f64_or(-2.0);

      // on-demand and the DOM must agree bit for bit
      if ( __builtin_bit_cast(u64, a) != __builtin_bit_cast(u64, b) ) snowball::print("F4b: iterate and parse disagree on ", h);
      sb::require_true(__builtin_bit_cast(u64, a) == __builtin_bit_cast(u64, b));
    }
    sb::end_test_case();
  }
  {
    // the same property, stated the way it actually bit: the parsed value must not
    // depend on bytes past the document. two buffers, identical for `len` bytes,
    // different after it, must produce identical results.
    sb::test_case("F4b: the parsed value never depends on bytes past len");
    fz::rng r(0x4B0175A1ull);
    constexpr usize cap = 256;
    u8 a[cap];
    u8 b[cap];
    u32 diffs = 0;
    for ( u32 iter = 0; iter < 200000; ++iter ) {
      const usize nd = 20 + (r.next() % 21);
      a[0] = b[0] = u8('1' + r.below(9));
      for ( usize i = 1; i < nd; ++i ) a[i] = b[i] = u8('0' + r.below(10));
      for ( usize i = nd; i < cap; ++i ) {
        a[i] = u8('7');      // more digits after the end
        b[i] = u8(' ');      // whitespace after the end
      }
      cjson::scratch sa, sbx;
      auto ra = cjson::iterate(cjson::bytes{ a, nd }, sa);
      auto rb = cjson::iterate(cjson::bytes{ b, nd }, sbx);
      if ( !ra.is_first() or !rb.is_first() ) continue;
      const f64 x = ra.cast<cjson::view>().root().f64_or(-1.0);
      const f64 y = rb.cast<cjson::view>().root().f64_or(-2.0);
      if ( __builtin_bit_cast(u64, x) != __builtin_bit_cast(u64, y) ) ++diffs;
    }
    if ( diffs ) snowball::print("F4b: ", diffs, " documents changed value with the bytes past len");
    sb::require(diffs, static_cast<u32>(0));
    sb::end_test_case();
  }
  {
    // a document at a guard page must survive the whole entry-point sweep, not just the
    // number path -- stage 1's borrowed mode promises an unpadded input is safe
    sb::test_case("borrowed mode never reads past an unpadded buffer");
    fz::rng r(0x9A8DEC01ull);
    for ( usize s = 0; s < fz::seed_count; ++s ) {
      for ( u32 iter = 0; iter < 200; ++iter ) {
        micron::vector<u8> v = fz::seed_bytes(s);
        fz::mutate(v, r, 1 + r.below(3));
        if ( v.size() == 0 or v.size() > 4096 ) continue;
        fz::guarded g(v.cbegin(), v.size());
        if ( !g.ok() ) continue;

        (void)cjson::validate(g.view());
        cjson::scratch sc;
        auto ri = cjson::iterate(g.view(), sc);
        if ( ri.is_first() ) {
          auto root = ri.cast<cjson::view>().root();
          sink = sink + usize(root.type()) + usize(root.i64_or(0)) + root.str_raw().len;
        }
        auto rp = cjson::parse(g.view());
        if ( rp.is_first() ) sink = sink + rp.cast<cjson::doc>().size();
      }
    }
    sb::require_true(true);
    sb::end_test_case();
  }
  snowball::print("fuzz_bytes: every entry point survived; sink = ", usize(sink));
  return 1;
}
