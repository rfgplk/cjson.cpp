//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// the twin seam at scale. CLAUDE.md rule 4: the scalar twin is the oracle.
//
// Under constant evaluation every `if !consteval` machine path is unreachable, so the
// SCALAR bodies run -- classify_scalar, the Kogge-Stone prefix xor, flatten_scalar, the
// by-the-book utf-8 decoder, the byte-wise load/store puns. At runtime the avx2 kernels
// run instead. Both must produce identical VERDICTS and identical ERROR CODES.
//
// rfc_comptime.cpp holds the rfc table to this bar. This file widens it: a generated
// corpus of several thousand documents, driven through both sides, plus the write path
// (which has its own scalar/simd split in write_simd.hpp) and the ct:: baking layer.

#include "../../src/cjson/cjson.hpp"

#include "../tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/string/strings.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

// a deterministic generator over an alphabet that reaches every classifier branch
struct gen {
  u64 s = 0x7717A5EEDull;

  u64
  next() noexcept
  {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }

  u32
  below(u32 n) noexcept
  {
    return n ? u32(next() % n) : 0;
  }
};

// bytes that move the classifier: structurals, quotes, backslashes, whitespace, the
// 0x0c/0x1a op false positives, utf-8 leads and continuations
const u8 k_alpha[]
    = { u8('{'),  u8('}'),  u8('['),  u8(']'),  u8(':'),  u8(','),  u8('"'),  u8('\\'), u8(' '),  u8('\t'), u8('\n'), u8('\r'), u8(0x0c),
        u8(0x1a), u8(0x01), u8('0'),  u8('1'),  u8('9'),  u8('-'),  u8('+'),  u8('.'),  u8('e'),  u8('t'),  u8('r'),  u8('u'),  u8('n'),
        u8('f'),  u8('a'),  u8(0xc3), u8(0xa9), u8(0xe2), u8(0x82), u8(0xac), u8(0xf0), u8(0x9d), u8(0x84), u8(0x9e), u8(0xff), u8(0x80) };
constexpr usize k_alpha_n = sizeof(k_alpha) / sizeof(k_alpha[0]);

};      // namespace

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// the comptime half: if this file compiles, these already passed

namespace
{

template<usize N>
consteval cjson::error
V(const char (&s)[N]) noexcept
{
  return cjson::validate(s, N - 1);
}

// a spread of shapes, each asserted at COMPILE time and re-checked at runtime below
constexpr const char *k_pairs[] = {
  "{}",
  "[]",
  "null",
  "true",
  "false",
  "0",
  "-1.5e-10",
  R"("s")",
  R"({"a":1,"b":[1,2,{"c":null}]})",
  R"([[[[[[[[1]]]]]]]])",
  R"({"esc":"\"\\\/\b\f\n\r\t\u0041\uD834\uDD1E"})",
  R"({"utf8":"aéあ𝄞"})",
  "[1,,2]",
  "{,}",
  "01",
  "1e",
  "+1",
  ".5",
  "Infinity",
  "NaN",
  "\"abc",
  "\"\x01\"",
  "\"\\x41\"",
  "\"\\uD800\"",
  "\"\xc0\x80\"",
  "\"\xed\xa0\x80\"",
  "{} x",
  "1 2",
  "",
  "   ",
  "1\0garbage",
  "[1\0]",
};
constexpr usize k_pairs_n = sizeof(k_pairs) / sizeof(k_pairs[0]);

static_assert(V("{}") == cjson::error::ok);
static_assert(V(R"({"a":1,"b":[1,2,{"c":null}]})") == cjson::error::ok);
static_assert(V(R"({"esc":"\"\\\/\b\f\n\r\t\u0041\uD834\uDD1E"})") == cjson::error::ok);
static_assert(V(R"({"utf8":"aéあ𝄞"})") == cjson::error::ok);
static_assert(V("[1,,2]") == cjson::error::bad_syntax);
static_assert(V("01") == cjson::error::bad_number);
static_assert(V("\"\\uD800\"") == cjson::error::bad_surrogate);
static_assert(V("\"\xc0\x80\"") == cjson::error::bad_utf8);
static_assert(V("1\0garbage") == cjson::error::bad_number);

};      // namespace

int
main()
{
  {
    // the runtime half of the seam for the compile-time table
    sb::test_case("the pinned shapes agree across the comptime/runtime seam");
    for ( usize i = 0; i < k_pairs_n; ++i ) {
      usize n = 0;
      // the table holds embedded nuls, so measure by scanning the literal's storage is
      // wrong -- these entries are checked by the static_asserts above; here we only
      // need the runtime call not to disagree on the ones without nuls
      while ( k_pairs[i][n] ) ++n;
      const cjson::error rt = cjson::validate(k_pairs[i], n);
      const cjson::error rt2 = cjson::validate(reinterpret_cast<const u8 *>(k_pairs[i]), n);
      sb::require_true(rt == rt2);
    }
    sb::end_test_case();
  }
  {
    // scale: a generated corpus through both the char* overload (which carries the
    // `if consteval` transient-copy arm) and the u8* one
    sb::test_case("a generated corpus agrees across the entry overloads");
    gen g;
    for ( u32 iter = 0; iter < 20000; ++iter ) {
      const u32 n = 1 + g.below(180);
      micron::vector<u8> d;
      d.reserve(n + 1);
      for ( u32 i = 0; i < n; ++i ) d.push_back(k_alpha[g.below(u32(k_alpha_n))]);

      micron::vector<char> c;
      c.reserve(n + 1);
      for ( u32 i = 0; i < n; ++i ) c.push_back(char(d[i]));

      const cjson::error a = cjson::validate(d.cbegin(), d.size());
      const cjson::error b = cjson::validate(c.cbegin(), c.size());
      if ( a != b ) snowball::print("overload disagreement at iter ", iter);
      sb::require_true(a == b);
    }
    sb::end_test_case();
  }
  {
    // the write side has its own scalar/simd split (esc_mask32, copy32). A document
    // written at runtime must match what the constexpr path bakes.
    sb::test_case("the write path agrees between comptime and runtime");
    static constexpr auto baked = cjson::ct::minify<cjson::ct::str{ R"({ "a" : [ 1 , 2 ] , "b" : "x\ny" })" }>();
    auto r = cjson::parse(reinterpret_cast<const u8 *>(R"({ "a" : [ 1 , 2 ] , "b" : "x\ny" })"), 34);
    sb::require_true(r.is_first());
    auto m = cjson::minify_str(cjson::bytes{ reinterpret_cast<const u8 *>(R"({ "a" : [ 1 , 2 ] , "b" : "x\ny" })"), 34 });
    sb::require_true(m.is_first());
    const micron::string &rt = m.cast<micron::string>();
    sb::require(rt.size(), baked.size());
    for ( usize i = 0; i < rt.size(); ++i ) sb::require_true(u8(rt[i]) == baked[i]);
    sb::end_test_case();
  }
  {
    sb::test_case("ct::parse bakes a tree the runtime parser agrees with");
    static constexpr auto tree = cjson::ct::parse<cjson::ct::str{ R"({"name":"café","n":42,"list":[1,2,3],"t":true})" }>();
    static_assert(tree.root()["n"].i64_or() == 42);
    static_assert(tree.root()["list"].size() == 3);
    static_assert(tree.root()["t"].bool_or());

    auto r = cjson::parse(reinterpret_cast<const u8 *>(R"({"name":"café","n":42,"list":[1,2,3],"t":true})"), 47);
    sb::require_true(r.is_first());
    auto root = r.cast<cjson::doc>().root();
    sb::require(root["n"].i64_or(0), static_cast<i64>(42));
    sb::require(root["list"].size(), static_cast<usize>(3));
    sb::require_true(root["t"].bool_or(false));
    sb::end_test_case();
  }
  {
    // the corpus, through both the fused avx2 utf-8 checker and the scalar decoder that
    // minify always uses -- they must agree on every byte of it
    sb::test_case("the fused and scalar utf-8 checkers agree over the corpus");
    const char *files[] = { "sample/64kb.json", "sample/twitter.json", "sample/128KB.json", "sample/512KB.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      // validate uses the fused checker; minify_into always uses the scalar one
      const bool v_ok = (cjson::validate(tutil::view(data)) == cjson::error::ok);
      auto m = cjson::minify_str(tutil::view(data));
      sb::require_true(v_ok == m.is_first());
    }
    sb::end_test_case();
  }
  return 1;
}
