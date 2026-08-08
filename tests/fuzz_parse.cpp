//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// mutation fuzz over the validate path: bit-flips, overwrites, truncations, splices and
// pure garbage must produce an error or ok — never a crash, hang, or out-of-bounds.
// run hosted as a regular suite; the real gate is the sanitizer build:
//   duck debug tests/fuzz_parse.cpp --asan --ubsan -i ../micron/tests -i ../micron -i ../micron/src

#include "../src/cjson/cjson.hpp"

#include "tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

// exact-size heap copy so sanitizers catch any read past len
cjson::error
validate_exact(const micron::vector<u8> &v)
{
  if ( v.size() == 0 ) return cjson::validate(static_cast<const u8 *>(nullptr), 0);
  u8 *tight = static_cast<u8 *>(abc::malloc(v.size()));
  for ( usize i = 0; i < v.size(); i++ ) tight[i] = v[i];
  const cjson::error e = cjson::validate(tight, v.size());
  abc::free(tight);
  return e;
}

volatile usize sink = 0;      // keeps the borrowed doc's reads from being folded away

const char *seeds[] = {
  R"({})",
  R"([null,true,false])",
  R"({"a":1,"b":[1.5e-3,-0,"x\ud83d\ude00y"],"c":{"d":"\u0041\\n"}})",
  R"([[[[[[[[]]]]]]]])",
  R"("escapes \" \\ \/ \b \f \n \r \t")",
  R"(-123456789.123456789e-100)",
};

};      // namespace

int
main()
{
  tutil::rng rg;
  {
    sb::test_case("mutated valid documents never crash the validator");
    for ( const char *s : seeds ) {
      micron::vector<u8> base;
      for ( usize i = 0; s[i]; i++ ) base.push_back(u8(s[i]));
      for ( u32 iter = 0; iter < 3000; iter++ ) {
        micron::vector<u8> v = base.clone();
        const u32 muts = 1 + rg.below(4);
        for ( u32 m = 0; m < muts; m++ ) {
          switch ( rg.below(4) ) {
          case 0:      // bit flip
            if ( v.size() ) v[rg.below(u32(v.size()))] ^= u8(1 << rg.below(8));
            break;
          case 1:      // overwrite with a hostile byte
            if ( v.size() ) {
              const char hostile[] = "\"\\{}[]:,\x00\x1f\x80\xff eu";
              v[rg.below(u32(v.size()))] = u8(hostile[rg.below(sizeof(hostile) - 1)]);
            }
            break;
          case 2:      // truncate
            if ( v.size() > 1 ) v.set_size(1 + rg.below(u32(v.size() - 1)));
            break;
          case 3:      // splice a fragment of another seed
          {
            const char *o = seeds[rg.below(sizeof(seeds) / sizeof(seeds[0]))];
            usize olen = 0;
            while ( o[olen] ) ++olen;
            const usize take = 1 + rg.below(u32(olen));
            for ( usize i = 0; i < take; i++ ) v.push_back(u8(o[i]));
            break;
          }
          default:
            break;
          }
        }
        (void)validate_exact(v);      // verdict irrelevant; surviving is the assertion
      }
    }
    sb::require_true(true);
    sb::end_test_case();
  }
  {
    sb::test_case("pure garbage never crashes the validator");
    for ( u32 iter = 0; iter < 20000; iter++ ) {
      micron::vector<u8> v;
      const u32 n = rg.below(160);
      for ( u32 i = 0; i < n; i++ ) v.push_back(u8(rg.below(256)));
      (void)validate_exact(v);
    }
    sb::require_true(true);
    sb::end_test_case();
  }
  {
    // one long-lived scratch across every iteration: the reuse path's geometric growth,
    // its shrink case (a retained pool much larger than the input) and its error paths
    // (which free nothing) all get hammered. under asan a lost __borrowed flag is a
    // double free, a shared release() is a use-after-free, a missing arena write-back
    // is a leak, and a pool sized len instead of len + padding is a heap-buffer-overflow
    // read of size 32 inside classify64
    sb::test_case("reuse mode survives mutation with one long-lived scratch");
    cjson::scratch sc;
    for ( const char *s : seeds ) {
      micron::vector<u8> base;
      for ( usize i = 0; s[i]; i++ ) base.push_back(u8(s[i]));
      for ( u32 iter = 0; iter < 3000; iter++ ) {
        micron::vector<u8> v = base.clone();
        // swing the length hard so the pool grows and shrinks against a retained buffer
        const u32 pad = rg.below(3) == 0 ? rg.below(4096) : 0;
        for ( u32 i = 0; i < pad; i++ ) v.push_back(u8(' '));
        const u32 muts = 1 + rg.below(4);
        for ( u32 m = 0; m < muts; m++ ) {
          if ( v.size() == 0 ) break;
          switch ( rg.below(3) ) {
          case 0:
            v[rg.below(u32(v.size()))] ^= u8(1 << rg.below(8));
            break;
          case 1: {
            const char hostile[] = "\"\\{}[]:,\x00\x1f\x80\xff eu";
            v[rg.below(u32(v.size()))] = u8(hostile[rg.below(sizeof(hostile) - 1)]);
            break;
          }
          default:
            if ( v.size() > 1 ) v.set_size(1 + rg.below(u32(v.size() - 1)));
            break;
          }
        }
        auto r = cjson::parse_reuse(tutil::view(v), {}, sc);
        if ( r.is_first() ) {
          const cjson::doc &d = r.cast<cjson::doc>();
          sb::require_true(d.borrowed());
          sink = sink + d.size();      // touch the borrowed slab while it is live
        }
      }
    }
    // the scratch must still be sound after all that
    sb::require_true(cjson::parse_reuse(R"({"a":1})", 7, {}, sc).is_first());
    sb::end_test_case();
  }
  {
    sb::test_case("validity is stable: a doc accepted once is accepted again");
    for ( const char *s : seeds ) {
      micron::vector<u8> v;
      for ( usize i = 0; s[i]; i++ ) v.push_back(u8(s[i]));
      const cjson::error a = validate_exact(v);
      const cjson::error b = validate_exact(v);
      sb::require_true(a == cjson::error::ok and b == cjson::error::ok);
    }
    sb::end_test_case();
  }
  return 1;
}
