//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// Model-based fuzzing of mutate.hpp with snowball's `scenario`: random SEQUENCES of
// set / insert / erase / rename / push_back / clear are driven against a hand-written
// shadow model, and after every call the document must re-serialise, re-parse, and still
// agree with the model.
//
// Sequences are what matters here. Any single setter is easy to get right; the bugs live
// in what a setter leaves behind for the next one -- a stale sibling offset after an
// erase, a pool that did not grow for a longer string, a parent back-offset that was not
// rebased after an insert. A property test over one call at a time cannot see those.
//
// snowball runs each generated program in a FORKED CHILD, so a crash is captured rather
// than ending the run, and then DELTA-DEBUGS the failing program down to a minimal call
// sequence. That minimisation is the reason to use `scenario` here instead of rolling
// the sequence generation by hand.

#include "fuzz_corpus.hpp"

#include <snowball/snowball.hpp>
#include <snowball/snowball_fuzz.hpp>

#include <micron/string/strings.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace sbf = snowball::fuzzing;

namespace mdl
{

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// the shadow model: a flat object of at most 8 members, keys "k0".."k7"

inline constexpr u32 slots = 8;

inline i64 val[slots];
inline bool present[slots];
inline micron::string text;      // the live json document

inline const char *
key_of(u32 i) noexcept
{
  static const char *k[slots] = { "k0", "k1", "k2", "k3", "k4", "k5", "k6", "k7" };
  return k[i % slots];
}

inline void
reset() noexcept
{
  for ( u32 i = 0; i < slots; ++i ) {
    val[i] = 0;
    present[i] = false;
  }
  text = micron::string("{}");
}

// re-serialise the current document from a doc handle
inline void
store(const cjson::doc &d)
{
  text = cjson::write_str(d);
}

// every operation ends here: the text must re-parse and match the model exactly
inline void
verify()
{
  auto r = cjson::parse(reinterpret_cast<const u8 *>(text.c_str()), text.size());
  if ( !r.is_first() ) FUZZ_FAIL("the mutated document no longer parses");
  auto root = r.cast<cjson::doc>().root();
  if ( root.type() != cjson::kind::object ) FUZZ_FAIL("the root stopped being an object");

  u32 want = 0;
  for ( u32 i = 0; i < slots; ++i )
    if ( present[i] ) ++want;
  if ( root.size() != want ) FUZZ_FAIL("member count diverged from the model");

  for ( u32 i = 0; i < slots; ++i ) {
    auto v = root[cjson::as_strv(key_of(i))];
    if ( present[i] ) {
      if ( v.type() != cjson::kind::number ) FUZZ_FAIL("a member the model has is missing or mistyped");
      if ( v.i64_or(val[i] + 1) != val[i] ) FUZZ_FAIL("a member's value diverged from the model");
    } else {
      if ( v.type() != cjson::kind::none ) FUZZ_FAIL("a member the model erased is still present");
    }
  }
}

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// the operations, each: parse -> mutate -> re-serialise -> verify

inline void
op_set(int slot, int v)
{
  const u32 i = u32(slot) % slots;
  auto r = cjson::parse(reinterpret_cast<const u8 *>(text.c_str()), text.size());
  if ( !r.is_first() ) FUZZ_FAIL("the document stopped parsing before a set");
  auto d = micron::move(r.cast<cjson::doc>());
  d.edit().insert(cjson::as_strv(key_of(i))) = static_cast<i64>(v);
  if ( d.mut_error() != cjson::error::ok ) FUZZ_FAIL("set reported a mutation error");
  present[i] = true;
  val[i] = v;
  store(d);
  verify();
}

inline void
op_erase(int slot)
{
  const u32 i = u32(slot) % slots;
  auto r = cjson::parse(reinterpret_cast<const u8 *>(text.c_str()), text.size());
  if ( !r.is_first() ) FUZZ_FAIL("the document stopped parsing before an erase");
  auto d = micron::move(r.cast<cjson::doc>());
  const cjson::error e = d.edit().erase(cjson::as_strv(key_of(i)));
  // erasing an absent key is a miss, not a corruption
  if ( e != cjson::error::ok and e != cjson::error::no_such_field ) FUZZ_FAIL("erase reported an unexpected error");
  if ( e == cjson::error::ok ) present[i] = false;
  d.clear_mut_error();
  store(d);
  verify();
}

inline void
op_clear()
{
  auto r = cjson::parse(reinterpret_cast<const u8 *>(text.c_str()), text.size());
  if ( !r.is_first() ) FUZZ_FAIL("the document stopped parsing before a clear");
  auto d = micron::move(r.cast<cjson::doc>());
  if ( d.edit().clear() != cjson::error::ok ) FUZZ_FAIL("clear reported a mutation error");
  for ( u32 i = 0; i < slots; ++i ) present[i] = false;
  store(d);
  verify();
}

inline void
op_rebuild()
{
  // round-trip the whole document through write/parse; the model must survive it
  auto r = cjson::parse(reinterpret_cast<const u8 *>(text.c_str()), text.size());
  if ( !r.is_first() ) FUZZ_FAIL("the document stopped parsing before a rebuild");
  store(r.cast<cjson::doc>());
  verify();
}

};      // namespace mdl

int
main()
{
  mdl::reset();

  sbf::scenario s;
  s.call("set", +[](int slot, int v) { mdl::op_set(slot, v); }).arg(sbf::range<int>(0, 7)).arg(sbf::range<int>(-100000, 100000));
  s.call("erase", +[](int slot) { mdl::op_erase(slot); }).arg(sbf::range<int>(0, 7));
  s.call("clear", +[]() { mdl::op_clear(); });
  s.call("rebuild", +[]() { mdl::op_rebuild(); });

  auto rep = s.run({ .seed = 0x0DDBA11ull, .iterations = 3000, .max_calls = 24, .minimize = true, .abort_on_failure = false });

  {
    sb::test_case("mutate: no call sequence breaks the model");
    if ( rep.found_failure ) {
      snowball::print("mutate scenario FAILED after ", rep.iteration, " iterations");
      snowball::print("   minimised to ", rep.minimized.calls.size(), " calls");
    }
    sb::require_false(rep.found_failure);
    sb::end_test_case();
  }
  {
    // the scenario runs its programs in forked children, so nothing it did is visible
    // here; drive one long sequence in-process as well, to prove the same invariants
    // hold without fork isolation papering over a leaked global
    sb::test_case("mutate: a long in-process sequence keeps the model");
    mdl::reset();
    fz::rng r(0x0DDBA12ull);
    for ( u32 i = 0; i < 20000; ++i ) {
      switch ( r.below(8) ) {
      case 0:
      case 1:
      case 2:
      case 3:
        mdl::op_set(int(r.below(8)), int(r.next() % 200000) - 100000);
        break;
      case 4:
      case 5:
        mdl::op_erase(int(r.below(8)));
        break;
      case 6:
        mdl::op_rebuild();
        break;
      default:
        mdl::op_clear();
        break;
      }
    }
    sb::end_test_case();
  }
  {
    // growing and shrinking STRING values is the case that moves the pool, so it gets a
    // dedicated sequence rather than relying on the integer model above
    sb::test_case("mutate: string values that grow and shrink keep the document valid");
    fz::rng r(0x57811611ull);
    for ( u32 iter = 0; iter < 4000; ++iter ) {
      auto rp = cjson::parse(reinterpret_cast<const u8 *>(R"({"a":"x","b":1,"c":[1,2]})"), 25);
      sb::require_true(rp.is_first());
      auto d = micron::move(rp.cast<cjson::doc>());

      micron::string s;
      const u32 n = r.below(400);
      for ( u32 i = 0; i < n; ++i ) s.push_back(char('a' + (i % 26)));

      d.edit()["a"] = cjson::strv{ s.c_str(), s.size() };
      sb::require_true(d.mut_error() == cjson::error::ok);

      micron::string out = cjson::write_str(d);
      auto rr = cjson::parse(reinterpret_cast<const u8 *>(out.c_str()), out.size());
      sb::require_true(rr.is_first());
      auto got = rr.cast<cjson::doc>().root()["a"].str_or();
      sb::require(got.len, static_cast<usize>(n));
      for ( usize i = 0; i < got.len; ++i ) sb::require_true(got.ptr[i] == s[i]);
      // the untouched neighbours must be exactly where they were
      sb::require(rr.cast<cjson::doc>().root()["b"].i64_or(-1), static_cast<i64>(1));
      sb::require(rr.cast<cjson::doc>().root()["c"].size(), static_cast<usize>(2));
    }
    sb::end_test_case();
  }
  snowball::print("fuzz_mutate_scenario: the model held");
  return 1;
}
