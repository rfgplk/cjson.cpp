//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// builder.hpp under random CALL SEQUENCES, valid and invalid alike.
//
// The builder is the one place a caller can drive cjson into an inconsistent state by
// misuse -- .key() inside an array, .value() where a name belongs, .end() with nothing
// open. The contract is that misuse is absorbed into a sticky error rather than emitted,
// so the safety invariant, checked after EVERY call, is:
//
//   1. out() is empty unless the builder is balanced and clean
//   2. whenever out() is non-empty it re-parses as conforming json (rfc s10)
//   3. err() is sticky: once set it never clears itself
//
// Those three hold regardless of what the sequence was, which is why they can be checked
// against a generator that deliberately produces nonsense. A second block builds only
// WELL-FORMED sequences and checks the output against the tree that was requested.

#include "fuzz_corpus.hpp"

#include <snowball/snowball.hpp>
#include <snowball/snowball_fuzz.hpp>

#include <micron/string/strings.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace sbf = snowball::fuzzing;

namespace bld
{

inline cjson::builder *b = nullptr;
inline bool saw_error = false;

inline void
reset()
{
  if ( b ) delete b;
  b = new cjson::builder();
  saw_error = false;
}

// the safety invariant, after every single call
inline void
invariant()
{
  const cjson::error e = b->err();
  if ( e != cjson::error::ok ) saw_error = true;
  // stickiness: once an error has been seen it must never clear
  if ( saw_error and b->err() == cjson::error::ok ) FUZZ_FAIL("builder::err cleared itself after being set");

  auto o = b->out();
  if ( o.len != 0 ) {
    if ( cjson::validate(reinterpret_cast<const u8 *>(o.ptr), o.len) != cjson::error::ok )
      FUZZ_FAIL("builder handed back text that is not conforming json");
  }
}

inline void
op_obj()
{
  b->obj();
  invariant();
}

inline void
op_arr()
{
  b->arr();
  invariant();
}

inline void
op_end()
{
  b->end();
  invariant();
}

inline void
op_null()
{
  b->null();
  invariant();
}

inline void
op_key(int k)
{
  static const char *keys[8] = { "a", "b", "c", "d", "ee", "fff", "", "\"\\\n" };
  b->key(cjson::as_strv(keys[u32(k) % 8]));
  invariant();
}

inline void
op_value_int(int v)
{
  b->value(static_cast<i64>(v));
  invariant();
}

inline void
op_value_str(int k)
{
  static const char *vals[8] = { "x", "", "a\"b", "c\\d", "e\nf", "gé𝄞", "0123456789", "\x7f" };
  b->value(cjson::as_strv(vals[u32(k) % 8]));
  invariant();
}

inline void
op_value_bool(int v)
{
  b->value((v & 1) != 0);
  invariant();
}

inline void
op_value_f64(int v)
{
  b->value(static_cast<f64>(v) / 7.0);
  invariant();
}

inline void
op_reset()
{
  reset();
  invariant();
}

};      // namespace bld

namespace
{

// build a random WELL-FORMED tree, emitting it through the builder and recording the
// shape, so the output can be checked against what was asked for rather than merely
// against the grammar
struct shaper {
  fz::rng r;
  cjson::builder &b;
  u32 nodes = 0;

  shaper(u64 seed, cjson::builder &bb) : r(seed), b(bb) { }

  void
  emit(u32 depth)
  {
    ++nodes;
    if ( depth >= 6 or nodes > 200 ) {
      b.value(static_cast<i64>(r.below(1000)));
      return;
    }
    switch ( r.below(6) ) {
    case 0:
      b.null();
      break;
    case 1:
      b.value((r.below(2) != 0));
      break;
    case 2:
      b.value(static_cast<i64>(r.next() % 100000) - 50000);
      break;
    case 3:
      b.value(cjson::as_strv("s"));
      break;
    case 4: {
      b.arr();
      const u32 n = r.below(5);
      for ( u32 i = 0; i < n; ++i ) emit(depth + 1);
      b.end();
      break;
    }
    default: {
      b.obj();
      const u32 n = r.below(5);
      static const char *keys[6] = { "k0", "k1", "k2", "k3", "k4", "k5" };
      for ( u32 i = 0; i < n; ++i ) {
        b.key(cjson::as_strv(keys[i % 6]));
        emit(depth + 1);
      }
      b.end();
      break;
    }
    }
  }
};

};      // namespace

int
main()
{
  bld::reset();

  sbf::scenario s;
  s.call("obj", +[]() { bld::op_obj(); });
  s.call("arr", +[]() { bld::op_arr(); });
  s.call("end", +[]() { bld::op_end(); });
  s.call("null", +[]() { bld::op_null(); });
  s.call("key", +[](int k) { bld::op_key(k); }).arg(sbf::range<int>(0, 7));
  s.call("value_int", +[](int v) { bld::op_value_int(v); }).arg(sbf::range<int>(-100000, 100000));
  s.call("value_str", +[](int k) { bld::op_value_str(k); }).arg(sbf::range<int>(0, 7));
  s.call("value_bool", +[](int v) { bld::op_value_bool(v); }).arg(sbf::range<int>(0, 1));
  s.call("value_f64", +[](int v) { bld::op_value_f64(v); }).arg(sbf::range<int>(-1000, 1000));
  s.call("reset", +[]() { bld::op_reset(); });

  auto rep = s.run({ .seed = 0xB011DEEull, .iterations = 3000, .max_calls = 30, .minimize = true, .abort_on_failure = false });

  {
    sb::test_case("builder: no call sequence, valid or not, breaks the safety invariant");
    if ( rep.found_failure ) {
      snowball::print("builder scenario FAILED after ", rep.iteration, " iterations");
      snowball::print("   minimised to ", rep.minimized.calls.size(), " calls");
    }
    sb::require_false(rep.found_failure);
    sb::end_test_case();
  }
  {
    // the same invariant in-process, so a leaked global cannot hide behind fork isolation
    sb::test_case("builder: a long in-process sequence keeps the safety invariant");
    bld::reset();
    fz::rng r(0xB011DEFull);
    for ( u32 i = 0; i < 40000; ++i ) {
      switch ( r.below(10) ) {
      case 0:
        bld::op_obj();
        break;
      case 1:
        bld::op_arr();
        break;
      case 2:
        bld::op_end();
        break;
      case 3:
        bld::op_null();
        break;
      case 4:
        bld::op_key(int(r.below(8)));
        break;
      case 5:
        bld::op_value_int(int(r.next() % 200000) - 100000);
        break;
      case 6:
        bld::op_value_str(int(r.below(8)));
        break;
      case 7:
        bld::op_value_bool(int(r.below(2)));
        break;
      case 8:
        bld::op_value_f64(int(r.next() % 2000) - 1000);
        break;
      default:
        bld::op_reset();
        break;
      }
    }
    sb::end_test_case();
  }
  {
    // well-formed sequences: the output must be exactly the tree that was requested
    sb::test_case("builder: well-formed sequences emit the requested tree");
    for ( u32 iter = 0; iter < 4000; ++iter ) {
      cjson::builder b;
      shaper sh(0xB0115EED + iter, b);
      sh.emit(0);
      micron::string out = b.take();

      if ( out.size() == 0 ) {
        snowball::print("builder produced nothing for a well-formed sequence, iter ", iter);
      }
      sb::require_greater(out.size(), static_cast<usize>(0));

      auto r = cjson::parse(reinterpret_cast<const u8 *>(out.c_str()), out.size());
      if ( !r.is_first() ) snowball::print("builder output does not parse: ", out.c_str());
      sb::require_true(r.is_first());

      // and it round-trips: write it again and it must be the same document
      micron::string again = cjson::write_str(r.cast<cjson::doc>());
      auto r2 = cjson::parse(reinterpret_cast<const u8 *>(again.c_str()), again.size());
      sb::require_true(r2.is_first());
      sb::require_true(fz::same_value(r.cast<cjson::doc>().root(), r2.cast<cjson::doc>().root()));
    }
    sb::end_test_case();
  }
  {
    // the sticky-error contract, stated directly
    sb::test_case("builder: err is sticky and out stays empty once unbalanced");
    {
      cjson::builder b;
      b.obj().key(cjson::as_strv("a")).value(static_cast<i64>(1));
      // still open: out() must be empty
      sb::require(b.out().len, static_cast<usize>(0));
      b.end();
      sb::require_greater(b.out().len, static_cast<usize>(0));
    }
    {
      cjson::builder b;
      b.end();      // nothing open
      sb::require_true(b.err() != cjson::error::ok);
      const cjson::error first = b.err();
      b.obj().key(cjson::as_strv("a")).value(static_cast<i64>(1)).end();
      sb::require_true(b.err() == first);      // sticky
      sb::require(b.out().len, static_cast<usize>(0));
    }
    {
      cjson::builder b;
      b.arr().key(cjson::as_strv("a"));      // a name inside an array
      sb::require_true(b.err() != cjson::error::ok);
      sb::require(b.out().len, static_cast<usize>(0));
    }
    sb::end_test_case();
  }
  // bld::reset() heap-allocates, so the last builder has to go back or LeakSanitizer
  // reports the suite's own bookkeeping as a finding and buries a real one
  if ( bld::b ) {
    delete bld::b;
    bld::b = nullptr;
  }
  snowball::print("fuzz_builder_scenario: the builder held its contract");
  return 1;
}
