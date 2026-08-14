//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// scratch.hpp's contract, stated deliberately. tests/fuzz/fuzz_scratch.cpp hammers the
// lifecycle randomly; this file walks the documented rules one at a time:
//
//   parse / parse_insitu             OWNING doc; may outlive the scratch
//   parse_reuse / *_insitu_reuse     BORROWING doc; valid until the next parse on that
//                                    scratch (a FAILED one included) or sc.release()
//   iterate                          BORROWING view, same rule
//   ensure / ensure_pool / ensure_vals   the three buffers grow independently
//   release()                        drops all three
//
// The retention numbers matter to callers: a warm scratch holds idx (4x len) +
// pool (len + 64) + vals (16 x nidx), so a shard that once saw a 26 MB document keeps
// ~180 MB until release(). That is a documented cost, not a leak.

#include "../../src/cjson/cjson.hpp"

#include "../tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/string/strings.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

micron::vector<u8>
arr_of(u32 n)
{
  micron::vector<u8> d;
  d.reserve(2 * n + 4);
  d.push_back(u8('['));
  for ( u32 i = 0; i < n; ++i ) {
    if ( i ) d.push_back(u8(','));
    d.push_back(u8('0' + (i % 10)));
  }
  d.push_back(u8(']'));
  return d;
}

};      // namespace

int
main()
{
  {
    sb::test_case("a fresh scratch parses without any priming");
    cjson::scratch sc;
    auto r = cjson::parse_reuse(R"({"a":1})", 7, {}, sc);
    sb::require_true(r.is_first());
    sb::require_true(r.cast<cjson::doc>().borrowed());
    sb::require(r.cast<cjson::doc>().root()["a"].i64_or(-1), static_cast<i64>(1));
    sb::end_test_case();
  }
  {
    sb::test_case("reuse borrows, plain parse owns");
    cjson::scratch sc;
    {
      auto b = cjson::parse_reuse(R"([1,2,3])", 7, {}, sc);
      sb::require_true(b.is_first());
      sb::require_true(b.cast<cjson::doc>().borrowed());
    }
    {
      auto o = cjson::parse(cjson::bytes{ reinterpret_cast<const u8 *>(R"([1,2,3])"), 7 }, {}, sc);
      sb::require_true(o.is_first());
      sb::require_false(o.cast<cjson::doc>().borrowed());
    }
    sb::end_test_case();
  }
  {
    // the difference that matters: an owning doc survives release()
    sb::test_case("an owning doc survives release, and stays readable");
    micron::string text;
    {
      cjson::scratch sc;
      auto d = arr_of(2000);
      auto r = cjson::parse(cjson::bytes{ d.cbegin(), d.size() }, {}, sc);
      sb::require_true(r.is_first());
      sb::require_false(r.cast<cjson::doc>().borrowed());
      sc.release();
      // the scratch is gone; the document is not
      sb::require(r.cast<cjson::doc>().root().size(), static_cast<usize>(2000));
      text = cjson::write_str(r.cast<cjson::doc>());
    }
    sb::require_true(cjson::validate(reinterpret_cast<const u8 *>(text.c_str()), text.size()) == cjson::error::ok);
    sb::end_test_case();
  }
  {
    sb::test_case("the three buffers grow independently and on demand");
    cjson::scratch sc;
    sb::require_true(sc.ensure(4096));
    sb::require_true(sc.ensure_pool(4096));
    sb::require_true(sc.ensure_vals(4096));
    // growing one must not disturb the others
    sb::require_true(sc.ensure(1 << 20));
    auto r = cjson::parse_reuse(R"({"a":[1,2,3]})", 13, {}, sc);
    sb::require_true(r.is_first());
    sb::require(r.cast<cjson::doc>().root()["a"].size(), static_cast<usize>(3));
    sb::end_test_case();
  }
  {
    sb::test_case("release is idempotent and leaves a usable scratch");
    cjson::scratch sc;
    sb::require_true(cjson::parse_reuse(R"([1])", 3, {}, sc).is_first());
    sc.release();
    sc.release();
    sc.release();
    sb::require_true(cjson::parse_reuse(R"([1])", 3, {}, sc).is_first());
    sb::end_test_case();
  }
  {
    // growth: each parse is bigger than the last, so the geometric path runs many times
    sb::test_case("a scratch grows monotonically without losing correctness");
    cjson::scratch sc;
    for ( u32 n = 1; n <= 20000; n = n * 2 + 1 ) {
      auto d = arr_of(n);
      auto r = cjson::parse_reuse(cjson::bytes{ d.cbegin(), d.size() }, {}, sc);
      sb::require_true(r.is_first());
      sb::require(r.cast<cjson::doc>().root().size(), static_cast<usize>(n));
    }
    sb::end_test_case();
  }
  {
    // shrink: a large document then many small ones. A stale retained size would show up
    // as a wrong element count or a bad read here.
    sb::test_case("a warm scratch handles much smaller documents afterwards");
    cjson::scratch sc;
    {
      auto big = arr_of(100000);
      auto r = cjson::parse_reuse(cjson::bytes{ big.cbegin(), big.size() }, {}, sc);
      sb::require_true(r.is_first());
      sb::require(r.cast<cjson::doc>().root().size(), static_cast<usize>(100000));
    }
    for ( u32 n = 1; n <= 40; ++n ) {
      auto small = arr_of(n);
      auto r = cjson::parse_reuse(cjson::bytes{ small.cbegin(), small.size() }, {}, sc);
      sb::require_true(r.is_first());
      sb::require(r.cast<cjson::doc>().root().size(), static_cast<usize>(n));
    }
    sb::end_test_case();
  }
  {
    // a FAILED parse consumes the scratch too, and must leave it sound
    sb::test_case("a failed parse invalidates the borrow but not the scratch");
    cjson::scratch sc;
    sb::require_true(cjson::parse_reuse(R"([1,2,3])", 7, {}, sc).is_first());
    // this one fails
    sb::require_true(cjson::parse_reuse(R"([1,)", 3, {}, sc).is_second());
    // and the scratch still works
    auto r = cjson::parse_reuse(R"([9,8])", 5, {}, sc);
    sb::require_true(r.is_first());
    sb::require(r.cast<cjson::doc>().root().size(), static_cast<usize>(2));
    sb::end_test_case();
  }
  {
    sb::test_case("insitu_reuse rewrites the caller's buffer and borrows the scratch");
    cjson::scratch sc;
    micron::vector<u8> buf;
    const char *j = R"({"name":"café","list":[1,2,3]})";
    for ( usize i = 0; j[i]; ++i ) buf.push_back(u8(j[i]));
    auto r = cjson::parse_insitu_reuse(cjson::wbytes{ buf.begin(), buf.size() }, {}, sc);
    sb::require_true(r.is_first());
    sb::require_true(r.cast<cjson::doc>().borrowed());
    sb::require_true(r.cast<cjson::doc>().pool() == buf.begin());      // aliased, not copied
    sb::require(r.cast<cjson::doc>().root()["list"].size(), static_cast<usize>(3));
    sb::end_test_case();
  }
  {
    sb::test_case("one scratch serves every mode in turn");
    cjson::scratch sc;
    auto d = arr_of(500);
    const cjson::bytes v{ d.cbegin(), d.size() };
    for ( u32 round = 0; round < 200; ++round ) {
      {
        auto r = cjson::parse(v, {}, sc);
        sb::require_true(r.is_first());
      }
      {
        auto r = cjson::parse_reuse(v, {}, sc);
        sb::require_true(r.is_first());
      }
      {
        auto r = cjson::iterate(v, sc);
        sb::require_true(r.is_first());
        sb::require(r.cast<cjson::view>().root().count(), static_cast<usize>(500));
      }
      {
        micron::vector<u8> mut = d.clone();
        auto r = cjson::parse_insitu_reuse(cjson::wbytes{ mut.begin(), mut.size() }, {}, sc);
        sb::require_true(r.is_first());
      }
      if ( (round % 37) == 0 ) sc.release();
    }
    sb::end_test_case();
  }
  {
    sb::test_case("a scratch handles the largest corpus document");
    auto data = tutil::slurp("sample/5MB.json");
    sb::require_greater(data.size(), static_cast<usize>(0));
    cjson::scratch sc;
    auto r = cjson::parse_reuse(tutil::view(data), {}, sc);
    sb::require_true(r.is_first());
    sb::require_greater(r.cast<cjson::doc>().size(), static_cast<usize>(0));
    sc.release();
    sb::require_true(cjson::parse_reuse(R"([1])", 3, {}, sc).is_first());
    sb::end_test_case();
  }
  return 1;
}
