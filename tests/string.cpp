//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include "../src/cjson/cjson.hpp"

#include "tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/types.hpp>
#include <micron/vector.hpp>

// string semantics: escapes decode by the book, surrogate pairs combine, the noesc
// subtype is set exactly when a string is escape-free, and unescaped output is always
// nul-terminated in the pool

namespace
{

bool
str_is(const char *jdoc, std::initializer_list<int> expect)
{
  usize n = 0;
  while ( jdoc[n] ) ++n;
  auto r = cjson::parse(jdoc, n);
  if ( r.is_second() ) return false;
  const cjson::doc &d = r.cast<cjson::doc>();
  auto s = d.root().str_or();
  if ( s.len != expect.size() ) return false;
  usize i = 0;
  for ( int e : expect )
    if ( u8(s.ptr[i++]) != u8(e) ) return false;
  return s.ptr[s.len] == 0;      // nul-terminated in pool
}

bool
rejects(const char *jdoc)
{
  usize n = 0;
  while ( jdoc[n] ) ++n;
  return cjson::parse(jdoc, n).is_second();
}

// true iff the parsed root string carries the noesc subtype bit
bool
noesc_of(const char *jdoc)
{
  usize n = 0;
  while ( jdoc[n] ) ++n;
  auto r = cjson::parse(jdoc, n);
  if ( r.is_second() ) return false;
  const cjson::doc &d = r.cast<cjson::doc>();
  auto root = d.root();
  return root.type() == cjson::kind::string and root.__subtype() == cjson::s_noesc;
}

};      // namespace

int
main()
{
  {
    sb::test_case("all eight simple escapes decode");
    sb::require_true(
        str_is(R"("\" \\ \/ \b \f \n \r \t")", { '"', ' ', '\\', ' ', '/', ' ', 0x08, ' ', 0x0c, ' ', 0x0a, ' ', 0x0d, ' ', 0x09 }));
    sb::end_test_case();
  }
  {
    sb::test_case("unicode escapes decode to utf-8 at every width");
    sb::require_true(str_is("\"\\u0041\"", { 'A' }));                                // 1-byte
    sb::require_true(str_is("\"\\u00e9\"", { 0xc3, 0xa9 }));                         // 2-byte
    sb::require_true(str_is("\"\\u4e2d\"", { 0xe4, 0xb8, 0xad }));                   // 3-byte
    sb::require_true(str_is("\"\\ud83d\\ude00\"", { 0xf0, 0x9f, 0x98, 0x80 }));      // 4-byte via pair
    sb::require_true(str_is("\"\\u0000x\"", { 0x00, 'x' }));                         // embedded nul is legal json
    sb::require_true(str_is("\"\\uFFFF\"", { 0xef, 0xbf, 0xbf }));
    sb::require_true(str_is("\"\\u0061B\\u0063\"", { 'a', 'B', 'c' }));              // mixed escape/verbatim
    sb::require_true(str_is("\"caf\xc3\xa9\"", { 'c', 'a', 'f', 0xc3, 0xa9 }));      // literal utf-8 passthrough
    sb::end_test_case();
  }
  {
    sb::test_case("mixed escaped and clean spans compact correctly in place");
    sb::require_true(str_is(R"("hello\nworld and a long clean tail 0123456789 abcdefghijklmnopqrstuvwxyz\t!")",
                            { 'h', 'e', 'l', 'l', 'o', 0x0a, 'w', 'o', 'r', 'l', 'd', ' ', 'a', 'n', 'd',  ' ', 'a', ' ', 'l',
                              'o', 'n', 'g', ' ', 'c', 'l',  'e', 'a', 'n', ' ', 't', 'a', 'i', 'l', ' ',  '0', '1', '2', '3',
                              '4', '5', '6', '7', '8', '9',  ' ', 'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h',  'i', 'j', 'k', 'l',
                              'm', 'n', 'o', 'p', 'q', 'r',  's', 't', 'u', 'v', 'w', 'x', 'y', 'z', 0x09, '!' }));
    sb::require_true(str_is(R"("\\\\\\\\")", { '\\', '\\', '\\', '\\' }));
    sb::require_true(str_is(R"("ABC")", { 'A', 'B', 'C' }));
    sb::end_test_case();
  }
  {
    sb::test_case("bad escapes and surrogate misuse are rejected");
    sb::require_true(rejects(R"("\q")"));
    sb::require_true(rejects(R"("\x41")"));
    sb::require_true(rejects(R"("\u00g0")"));
    sb::require_true(rejects(R"("\u123")"));
    sb::require_true(rejects(R"("\ud800")"));
    sb::require_true(rejects(R"("\ud800x")"));
    sb::require_true(rejects(R"("\ud800A")"));
    sb::require_true(rejects(R"("\udc00")"));
    sb::require_true(rejects(R"("\udc00\ud800")"));
    sb::require_true(rejects("\"raw\ttab\""));
    sb::require_true(rejects("\"raw\nnewline\""));
    sb::require_true(rejects(R"("unterminated \")"));
    sb::end_test_case();
  }
  {
    sb::test_case("noesc subtype is set exactly for escape-free strings");
    // internal check against the arena representation
    {
      const char *j = R"(["clean","with\nescape"])";
      usize n = 0;
      while ( j[n] ) ++n;
      auto r = cjson::parse(j, n);
      sb::require_true(r.is_first());
      const cjson::doc &d = r.cast<cjson::doc>();
      auto a0 = d.root()[usize(0)];
      auto a1 = d.root()[usize(1)];
      sb::require_true(a0.type() == cjson::kind::string and a1.type() == cjson::kind::string);
      auto s0 = a0.str_or();
      auto s1 = a1.str_or();
      sb::require(s0.len, static_cast<usize>(5));
      sb::require(s1.len, static_cast<usize>(11));
      sb::require_true(s1.ptr[4] == 0x0a);
    }
    sb::require_true(noesc_of(R"("plain")"));
    sb::require_true(noesc_of("\"caf\xc3\xa9 literal\""));
    sb::require_true(!noesc_of(R"("esc\n")"));
    sb::require_true(!noesc_of("\"\\u0041\""));
    sb::end_test_case();
  }
  {
    sb::test_case("keys unescape and match lookups the same as values");
    const char *j = R"({"café":1,"tab\tkey":2})";
    usize n = 0;
    while ( j[n] ) ++n;
    auto r = cjson::parse(j, n);
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    const char key1[] = { 'c', 'a', 'f', char(0xc3), char(0xa9), 0 };
    sb::require(d.root()[key1].i64_or(-1), static_cast<i64>(1));
    const char key2[] = { 't', 'a', 'b', 0x09, 'k', 'e', 'y', 0 };
    sb::require(d.root()[key2].i64_or(-1), static_cast<i64>(2));
    sb::require(d.root()["missing"].i64_or(-1), static_cast<i64>(-1));
    sb::end_test_case();
  }
  {
    sb::test_case("random escape soup either parses or rejects without corruption");
    tutil::rng rg;
    const char *frag[] = { "\\n", "\\t", "\\\"", "\\\\", "\\u0041", "\\ud83d\\ude00", "x", "yz", " ", "é" };
    for ( u32 iter = 0; iter < 3000; iter++ ) {
      micron::vector<u8> doc;
      doc.push_back(u8('"'));
      const u32 parts = rg.below(12);
      for ( u32 i = 0; i < parts; i++ ) {
        const char *f = frag[rg.below(sizeof(frag) / sizeof(frag[0]))];
        for ( usize j = 0; f[j]; j++ ) doc.push_back(u8(f[j]));
      }
      doc.push_back(u8('"'));
      auto r = cjson::parse(cjson::bytes{ doc.cbegin(), doc.size() });
      sb::require_true(r.is_first());      // every generated doc is valid by construction
    }
    sb::end_test_case();
  }
  return 1;
}
