//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// the on-demand cursor held against the dom, field for field, over the whole corpus.
//
// `cur` and `val` answer the same questions by completely different means: `val` reads a
// materialised 16-byte arena, `cur` walks the structural index over the caller's bytes
// and decodes on demand. Nothing forces them to agree except tests, and the places they
// can drift are specific -- str_raw vs str_or (escapes retained vs decoded), count() vs
// size() (walked vs stored), and the borrow lifetime, which the dom does not have at all.

#include "../../src/cjson/cjson.hpp"

#include "../tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/string/strings.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

// walk a cur and a val in lockstep and require every observable to match
usize
agree(cjson::val v, cjson::cur c, u32 depth = 0)
{
  if ( depth > 100 ) return 0;
  sb::require_true(v.type() == c.peek_kind());
  usize seen = 1;

  switch ( v.type() ) {
  case cjson::kind::number: {
    const f64 a = v.f64_or(0.0), b = c.f64_or(1.0);
    sb::require_true(__builtin_bit_cast(u64, a) == __builtin_bit_cast(u64, b));
    sb::require(v.i64_or(7), c.i64_or(7));
    sb::require(v.u64_or(7), c.u64_or(7));
    break;
  }
  case cjson::kind::boolean:
    sb::require_true(v.bool_or(false) == c.bool_or(false));
    sb::require_true(v.bool_or(true) == c.bool_or(true));
    break;
  case cjson::kind::string: {
    auto a = v.str_or();
    // str(wbytes) decodes into the caller's buffer; it must produce the dom's bytes
    micron::vector<u8> buf;
    buf.reserve(a.len + 64);
    const max_t n = c.str(cjson::wbytes{ buf.begin(), a.len + 32 });
    sb::require_true(n >= 0);
    sb::require(usize(n), a.len);
    for ( usize i = 0; i < a.len; ++i ) sb::require_true(u8(a.ptr[i]) == buf.begin()[i]);
    break;
  }
  case cjson::kind::array: {
    sb::require(v.size(), c.count());
    usize i = 0;
    for ( auto e : c.items() ) {
      seen += agree(v.at(i), e, depth + 1);
      ++i;
    }
    sb::require(i, v.size());
    break;
  }
  case cjson::kind::object: {
    sb::require(v.size(), c.count());
    usize i = 0;
    for ( auto m : c.members() ) {
      auto vm = v.at(i);      // positional, so duplicate names line up
      (void)vm;
      seen += agree(v[m.key], m.v, depth + 1);
      ++i;
    }
    sb::require(i, v.size());
    break;
  }
  default:
    break;
  }
  return seen;
}

};      // namespace

int
main()
{
  {
    sb::test_case("cur and val agree over the whole corpus");
    const char *files[] = { "sample/64kb.json", "sample/128KB.json", "sample/twitter.json", "sample/256KB.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      auto rd = cjson::parse(tutil::view(data));
      sb::require_true(rd.is_first());
      cjson::scratch sc;
      auto rv = cjson::iterate(tutil::view(data), sc);
      sb::require_true(rv.is_first());
      const usize n = agree(rd.cast<cjson::doc>().root(), rv.cast<cjson::view>().root());
      sb::require_greater(n, static_cast<usize>(0));
    }
    sb::end_test_case();
  }
  {
    // str_raw keeps the source spelling; str_or hands back the decoded bytes. Both are
    // correct answers to different questions, and confusing them is the bug this catches.
    sb::test_case("str_raw retains escapes where str_or decodes them");
    // length DEDUCED. Hand-counting a literal that mixes escapes with multi-byte utf-8
    // is exactly how a test ends up measuring a truncated document instead of the one it
    // meant to write.
    static constexpr char j[] = R"({"a":"x\ny","b":"plain","c":"Aé"})";
    constexpr usize jn = sizeof(j) - 1;
    auto rd = cjson::parse(reinterpret_cast<const u8 *>(j), jn);
    sb::require_true(rd.is_first());
    cjson::scratch sc;
    auto rv = cjson::iterate(cjson::bytes{ reinterpret_cast<const u8 *>(j), jn }, sc);
    sb::require_true(rv.is_first());
    auto root = rd.cast<cjson::doc>().root();
    auto croot = rv.cast<cjson::view>().root();

    // decoded: 3 bytes  x \n y
    sb::require(root["a"].str_or().len, static_cast<usize>(3));
    // raw: 4 bytes  x \ n y
    sb::require(croot["a"].str_raw().len, static_cast<usize>(4));

    // with no escapes the two coincide
    sb::require(root["b"].str_or().len, croot["b"].str_raw().len);

    // "Aé" carries no escapes, so the decoded and raw spellings coincide at
    // 1 byte for A plus 2 for the utf-8 e-acute
    sb::require(root["c"].str_or().len, static_cast<usize>(3));
    sb::require(croot["c"].str_raw().len, static_cast<usize>(3));
    sb::end_test_case();
  }
  {
    sb::test_case("cur::str reports short_output rather than truncating");
    const char *j = R"("abcdefghij")";
    cjson::scratch sc;
    auto rv = cjson::iterate(cjson::bytes{ reinterpret_cast<const u8 *>(j), 12 }, sc);
    sb::require_true(rv.is_first());
    auto root = rv.cast<cjson::view>().root();

    micron::vector<u8> big;
    big.reserve(64);
    sb::require(root.str(cjson::wbytes{ big.begin(), 32 }), static_cast<max_t>(10));

    for ( usize cap = 0; cap < 10; ++cap ) {
      micron::vector<u8> small;
      small.reserve(64);
      const max_t rc = root.str(cjson::wbytes{ small.begin(), cap });
      sb::require_true(rc < 0);
      sb::require_true(cjson::as_error(rc) == cjson::error::short_output);
    }
    sb::end_test_case();
  }
  {
    // the borrow rule: a view is valid until the NEXT parse on that scratch, a failed one
    // included. This test does not use-after-free -- it proves the successor is what the
    // scratch now describes, which is the observable half of the contract.
    sb::test_case("a second parse on one scratch re-points the index");
    cjson::scratch sc;
    {
      auto a = cjson::iterate(cjson::bytes{ reinterpret_cast<const u8 *>(R"([1,2,3])"), 7 }, sc);
      sb::require_true(a.is_first());
      sb::require(a.cast<cjson::view>().root().count(), static_cast<usize>(3));
    }
    {
      auto b = cjson::iterate(cjson::bytes{ reinterpret_cast<const u8 *>(R"([1,2,3,4,5])"), 11 }, sc);
      sb::require_true(b.is_first());
      sb::require(b.cast<cjson::view>().root().count(), static_cast<usize>(5));
    }
    // a FAILED parse also consumes the scratch; afterwards it must still work
    {
      auto bad = cjson::iterate(cjson::bytes{ reinterpret_cast<const u8 *>(R"([1,)"), 3 }, sc);
      (void)bad;
      auto c = cjson::iterate(cjson::bytes{ reinterpret_cast<const u8 *>(R"([9])"), 3 }, sc);
      sb::require_true(c.is_first());
      sb::require(c.cast<cjson::view>().root().count(), static_cast<usize>(1));
    }
    sb::end_test_case();
  }
  {
    sb::test_case("a null cursor is falsey and navigates without crashing");
    cjson::scratch sc;
    auto rv = cjson::iterate(cjson::bytes{ reinterpret_cast<const u8 *>(R"({"a":[1]})"), 9 }, sc);
    sb::require_true(rv.is_first());
    auto root = rv.cast<cjson::view>().root();
    sb::require_true(static_cast<bool>(root));
    auto miss = root["nope"];
    sb::require_false(static_cast<bool>(miss));
    sb::require_true(miss.peek_kind() == cjson::kind::none);
    sb::require(miss["deeper"]["deeper"].i64_or(5), static_cast<i64>(5));
    sb::require(miss.count(), static_cast<usize>(0));
    sb::end_test_case();
  }
  {
    sb::test_case("count walks where size stores, and they agree");
    const char *docs[] = { R"([])", R"([1])", R"([1,2,3,4,5,6,7,8,9,10])", R"({})", R"({"a":1,"b":2})", R"([[1,2],[3],[]])" };
    for ( const char *d : docs ) {
      usize n = 0;
      while ( d[n] ) ++n;
      auto rd = cjson::parse(reinterpret_cast<const u8 *>(d), n);
      sb::require_true(rd.is_first());
      cjson::scratch sc;
      auto rv = cjson::iterate(cjson::bytes{ reinterpret_cast<const u8 *>(d), n }, sc);
      sb::require_true(rv.is_first());
      sb::require(rd.cast<cjson::doc>().root().size(), rv.cast<cjson::view>().root().count());
    }
    sb::end_test_case();
  }
  {
    sb::test_case("at_pointer works from a cursor too");
    static constexpr char j[] = R"({"foo":["bar",{"baz":7}],"a/b":1})";
    cjson::scratch sc;
    auto rv = cjson::iterate(cjson::bytes{ reinterpret_cast<const u8 *>(j), sizeof(j) - 1 }, sc);
    sb::require_true(rv.is_first());
    auto root = rv.cast<cjson::view>().root();
    sb::require(root.at_pointer(cjson::as_strv("/foo/1/baz")).i64_or(-1), static_cast<i64>(7));
    sb::require(root.at_pointer(cjson::as_strv("/a~1b")).i64_or(-1), static_cast<i64>(1));
    sb::require_true(root.at_pointer(cjson::as_strv("/nope")).peek_kind() == cjson::kind::none);
    sb::end_test_case();
  }
  return 1;
}
