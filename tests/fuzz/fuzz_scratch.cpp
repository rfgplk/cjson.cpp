//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// The scratch/borrow lifecycle, hammered. This is the part of cjson where the ownership
// rules are subtle enough to be worth a fuzzer rather than a checklist:
//
//   parse / parse_insitu              return an OWNING doc that may outlive the scratch
//   parse_reuse / parse_insitu_reuse  return a BORROWING doc, valid only until the next
//                                     parse on that scratch (a FAILED one included) or
//                                     until sc.release()
//   iterate                           returns a BORROWING view, same rules
//
// The --asan arm is the real assertion here, and each bug class has a signature:
//   a lost __borrowed flag            -> double free
//   a shared release()                -> use-after-free
//   a missing arena write-back        -> leak
//   a pool sized len instead of
//     len + padding                   -> heap-buffer-overflow read inside classify64
//
// Input sizes swing hard on purpose so the retained buffers both grow and shrink: a
// scratch that once saw a large document keeps idx (4x len) + pool (len + 64) +
// vals (16 x nidx) until release().

#include "fuzz_corpus.hpp"

#include "../tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

volatile usize sink = 0;

// build a document of roughly the requested size out of the seed corpus
micron::vector<u8>
sized_doc(u32 target, fz::rng &r)
{
  micron::vector<u8> d;
  d.push_back(u8('['));
  bool first = true;
  while ( d.size() < target ) {
    if ( !first ) d.push_back(u8(','));
    first = false;
    const char *s = fz::seeds[r.below(u32(fz::seed_count))];
    const usize n = fz::slen(s);
    // only splice in seeds that are themselves valid values
    if ( cjson::validate(reinterpret_cast<const u8 *>(s), n) != cjson::error::ok ) {
      d.push_back(u8('0'));
      continue;
    }
    for ( usize i = 0; i < n; ++i ) d.push_back(u8(s[i]));
  }
  d.push_back(u8(']'));
  return d;
}

};      // namespace

int
main()
{
  {
    // one long-lived scratch, sizes swinging by orders of magnitude, so the reuse path's
    // geometric growth AND its shrink case both run many times over
    sb::test_case("one scratch survives wildly varying document sizes");
    cjson::scratch sc;
    fz::rng r(0x5C4A7C11ull);
    for ( u32 iter = 0; iter < 4000; ++iter ) {
      const u32 target = (r.below(4) == 0) ? (64 + r.below(60000)) : (8 + r.below(400));
      auto d = sized_doc(target, r);
      auto rr = cjson::parse_reuse(cjson::bytes{ d.cbegin(), d.size() }, {}, sc);
      sb::require_true(rr.is_first());
      const cjson::doc &doc = rr.cast<cjson::doc>();
      sb::require_true(doc.borrowed());
      sink = sink + doc.size();
      // touch the borrowed slab so the reads are not folded away
      auto root = doc.root();
      sink = sink + root.size();
      if ( root.size() ) sink = sink + usize(root[usize(0)].type());
    }
    // the scratch must still be sound after all that
    sb::require_true(cjson::parse_reuse(R"({"a":1})", 7, {}, sc).is_first());
    sb::end_test_case();
  }
  {
    // FAILED parses on a reuse scratch free nothing, and must leave it usable
    sb::test_case("a failed parse leaves the scratch usable");
    cjson::scratch sc;
    fz::rng r(0xFA17EDull);
    for ( u32 iter = 0; iter < 8000; ++iter ) {
      micron::vector<u8> v = fz::seed_bytes(r.below(u32(fz::seed_count)));
      fz::mutate(v, r, 1 + r.below(4));
      auto rr = cjson::parse_reuse(cjson::bytes{ v.cbegin(), v.size() }, {}, sc);
      if ( rr.is_first() ) sink = sink + rr.cast<cjson::doc>().size();
      // after any outcome, a known-good document must still parse on this scratch
      if ( (iter & 63) == 0 ) sb::require_true(cjson::parse_reuse(R"([1,2,3])", 7, {}, sc).is_first());
    }
    sb::require_true(cjson::parse_reuse(R"({"a":1})", 7, {}, sc).is_first());
    sb::end_test_case();
  }
  {
    // release() between uses: each of the three buffers releases independently, and the
    // scratch must rebuild them on demand
    sb::test_case("release() is safe at any point in the cycle");
    cjson::scratch sc;
    fz::rng r(0x2E1EA5E0ull);
    for ( u32 iter = 0; iter < 3000; ++iter ) {
      auto d = sized_doc(16 + r.below(2000), r);
      {
        auto rr = cjson::parse_reuse(cjson::bytes{ d.cbegin(), d.size() }, {}, sc);
        sb::require_true(rr.is_first());
        sink = sink + rr.cast<cjson::doc>().size();
      }      // the borrowing doc dies here, BEFORE release
      if ( r.below(3) == 0 ) sc.release();
    }
    sb::require_true(cjson::parse_reuse(R"([0])", 3, {}, sc).is_first());
    sb::end_test_case();
  }
  {
    // an OWNING doc must outlive the scratch it was built with -- that is the whole
    // difference between parse and parse_reuse
    sb::test_case("an owning doc outlives its scratch");
    micron::string written;
    {
      fz::rng r(0x0A11EEull);
      cjson::scratch sc;
      auto d = sized_doc(4000, r);
      auto rr = cjson::parse(cjson::bytes{ d.cbegin(), d.size() }, {}, sc);
      sb::require_true(rr.is_first());
      sb::require_false(rr.cast<cjson::doc>().borrowed());
      sc.release();
      // the scratch is gone; the doc is not
      written = cjson::write_str(rr.cast<cjson::doc>());
    }
    sb::require_greater(written.size(), static_cast<usize>(0));
    sb::require_true(cjson::validate(reinterpret_cast<const u8 *>(written.c_str()), written.size()) == cjson::error::ok);
    sb::end_test_case();
  }
  {
    // iterate borrows both the caller's bytes and the scratch's index
    sb::test_case("a borrowed view is valid until the next parse on its scratch");
    cjson::scratch sc;
    fz::rng r(0xB0110Cull);
    for ( u32 iter = 0; iter < 4000; ++iter ) {
      auto d = sized_doc(16 + r.below(3000), r);
      auto rv = cjson::iterate(cjson::bytes{ d.cbegin(), d.size() }, sc);
      sb::require_true(rv.is_first());
      auto root = rv.cast<cjson::view>().root();
      sink = sink + root.count();
      u32 guard = 0;
      for ( auto e : root.items() ) {
        sink = sink + usize(e.type());
        if ( ++guard > 32 ) break;
      }
    }
    sb::end_test_case();
  }
  {
    // mixing the modes on one scratch: owning, borrowing and on-demand in sequence
    sb::test_case("owning, borrowing and on-demand interleave on one scratch");
    cjson::scratch sc;
    fz::rng r(0x1717E12Eull);
    for ( u32 iter = 0; iter < 3000; ++iter ) {
      auto d = sized_doc(16 + r.below(1500), r);
      const cjson::bytes v{ d.cbegin(), d.size() };
      switch ( iter % 4 ) {
      case 0: {
        auto rr = cjson::parse(v, {}, sc);
        sb::require_true(rr.is_first());
        sink = sink + rr.cast<cjson::doc>().size();
        break;
      }
      case 1: {
        auto rr = cjson::parse_reuse(v, {}, sc);
        sb::require_true(rr.is_first());
        sink = sink + rr.cast<cjson::doc>().size();
        break;
      }
      case 2: {
        auto rv = cjson::iterate(v, sc);
        sb::require_true(rv.is_first());
        sink = sink + rv.cast<cjson::view>().root().count();
        break;
      }
      default: {
        micron::vector<u8> mut = d.clone();
        auto rr = cjson::parse_insitu_reuse(cjson::wbytes{ mut.begin(), mut.size() }, {}, sc);
        sb::require_true(rr.is_first());
        sink = sink + rr.cast<cjson::doc>().size();
        break;
      }
      }
    }
    sb::end_test_case();
  }
  {
    // a scratch that once saw a big document keeps its buffers; that is deliberate, and
    // the point of release(). What must NOT happen is a stale-size bug on the way down.
    sb::test_case("a warm scratch shrinks correctly after a large document");
    cjson::scratch sc;
    fz::rng r(0x5480E5ull);
    {
      auto big = sized_doc(200000, r);
      auto rr = cjson::parse_reuse(cjson::bytes{ big.cbegin(), big.size() }, {}, sc);
      sb::require_true(rr.is_first());
      sink = sink + rr.cast<cjson::doc>().size();
    }
    for ( u32 iter = 0; iter < 2000; ++iter ) {
      auto small = sized_doc(4 + r.below(40), r);
      auto rr = cjson::parse_reuse(cjson::bytes{ small.cbegin(), small.size() }, {}, sc);
      sb::require_true(rr.is_first());
      sink = sink + rr.cast<cjson::doc>().size();
    }
    sc.release();
    sb::require_true(cjson::parse_reuse(R"([1])", 3, {}, sc).is_first());
    sb::end_test_case();
  }
  snowball::print("fuzz_scratch: the scratch survived; sink = ", usize(sink));
  return 1;
}
