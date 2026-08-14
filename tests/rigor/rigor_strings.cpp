//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// the unescape path under adversarial layouts. rfc_strings.cpp owns WHICH escapes are
// legal; this file owns whether they decode correctly once the layout gets awkward:
// escapes straddling the 64-byte block seam, long clean runs between escapes, backslash
// runs that flip the escape parity, and the overlap-tolerant forward copy that moves the
// decoded bytes down over the source.

#include "../../src/cjson/cjson.hpp"

#include "../tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/string/strings.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

// {"k":"<pad>xxxx<body>"} -- the pad slides `body` across every block boundary
micron::vector<u8>
lay(u32 pad, const char *body)
{
  micron::vector<u8> d;
  const char *pre = "{\"k\":\"";
  for ( usize i = 0; pre[i]; ++i ) d.push_back(u8(pre[i]));
  for ( u32 i = 0; i < pad; ++i ) d.push_back(u8('x'));
  for ( usize i = 0; body[i]; ++i ) d.push_back(u8(body[i]));
  d.push_back(u8('"'));
  d.push_back(u8('}'));
  return d;
}

void
decodes_to(u32 pad, const char *body, const u8 *want, usize want_n)
{
  auto d = lay(pad, body);
  auto r = cjson::parse(cjson::bytes{ d.cbegin(), d.size() });
  if ( !r.is_first() ) {
    snowball::print("unescape FAILED to parse at pad ", pad, ": ", body);
    sb::require_true(false);
  }
  auto s = r.cast<cjson::doc>().root()["k"].str_or();
  if ( s.len != pad + want_n ) {
    snowball::print("unescape length wrong at pad ", pad, ": ", body);
    snowball::print("   got ", s.len, " want ", usize(pad + want_n));
  }
  sb::require(s.len, static_cast<usize>(pad + want_n));
  for ( u32 i = 0; i < pad; ++i ) sb::require_true(s.ptr[i] == 'x');
  for ( usize i = 0; i < want_n; ++i ) {
    if ( u8(s.ptr[pad + i]) != want[i] ) snowball::print("unescape byte wrong at pad ", pad, " offset ", i);
    sb::require_true(u8(s.ptr[pad + i]) == want[i]);
  }
}

};      // namespace

int
main()
{
  {
    // 200 offsets carries every body across the 64-byte block boundary, the 128-byte
    // pair boundary, and the tail peel
    sb::test_case("every escape decodes identically at every offset across the seams");

    struct tc {
      const char *body;
      u8 want[8];
      usize n;
    };

    const tc t[] = {
      { R"(\")", { 0x22 }, 1 },
      { R"(\\)", { 0x5c }, 1 },
      { R"(\/)", { 0x2f }, 1 },
      { R"(\b)", { 0x08 }, 1 },
      { R"(\f)", { 0x0c }, 1 },
      { R"(\n)", { 0x0a }, 1 },
      { R"(\r)", { 0x0d }, 1 },
      { R"(\t)", { 0x09 }, 1 },
      { R"(A)", { 0x41 }, 1 },
      { R"(\u0000)", { 0x00 }, 1 },
      { R"(é)", { 0xc3, 0xa9 }, 2 },
      { R"(€)", { 0xe2, 0x82, 0xac }, 3 },
      { R"(𝄞)", { 0xf0, 0x9d, 0x84, 0x9e }, 4 },
      { R"(\n\n\n\n)", { 0x0a, 0x0a, 0x0a, 0x0a }, 4 },
      { R"(\\\\)", { 0x5c, 0x5c }, 2 },
      { R"(\"\\\/)", { 0x22, 0x5c, 0x2f }, 3 },
    };
    for ( const tc &c : t )
      for ( u32 pad = 0; pad < 200; ++pad ) decodes_to(pad, c.body, c.want, c.n);
    sb::end_test_case();
  }
  {
    // backslash runs flip the escape parity; an off-by-one in resolve_escapes shows up
    // as a closing quote being swallowed or a legal escape being missed
    sb::test_case("backslash runs of every length resolve with the right parity");
    for ( u32 run = 1; run <= 70; ++run ) {
      if ( run & 1 ) continue;      // an odd run leaves a dangling escape; that is a reject case
      for ( u32 pad = 0; pad < 140; pad += 7 ) {
        micron::vector<u8> body;
        for ( u32 i = 0; i < run; ++i ) {
          body.push_back(u8('\\'));
          body.push_back(u8('\\'));
        }
        body.push_back(u8(0));
        micron::vector<u8> want;
        for ( u32 i = 0; i < run; ++i ) want.push_back(u8('\\'));
        decodes_to(pad, reinterpret_cast<const char *>(body.cbegin()), want.cbegin(), want.size());
      }
    }
    sb::end_test_case();
  }
  {
    sb::test_case("an odd backslash run before the closing quote is rejected");
    for ( u32 pad = 0; pad < 140; pad += 11 ) {
      micron::vector<u8> d;
      const char *pre = "{\"k\":\"";
      for ( usize i = 0; pre[i]; ++i ) d.push_back(u8(pre[i]));
      for ( u32 i = 0; i < pad; ++i ) d.push_back(u8('x'));
      d.push_back(u8('\\'));      // escapes the closing quote
      d.push_back(u8('"'));
      d.push_back(u8('}'));
      sb::require_true(cjson::parse(cjson::bytes{ d.cbegin(), d.size() }).is_second());
    }
    sb::end_test_case();
  }
  {
    // long clean runs between escapes exercise the block copy rather than the byte path
    sb::test_case("long clean runs between escapes copy correctly");
    for ( u32 run : { 1u, 15u, 16u, 17u, 31u, 32u, 33u, 63u, 64u, 65u, 127u, 128u, 129u, 1000u } ) {
      micron::vector<u8> d;
      const char *pre = "{\"k\":\"";
      for ( usize i = 0; pre[i]; ++i ) d.push_back(u8(pre[i]));
      d.push_back(u8('\\'));
      d.push_back(u8('n'));
      for ( u32 i = 0; i < run; ++i ) d.push_back(u8('a' + (i % 26)));
      d.push_back(u8('\\'));
      d.push_back(u8('t'));
      d.push_back(u8('"'));
      d.push_back(u8('}'));

      auto r = cjson::parse(cjson::bytes{ d.cbegin(), d.size() });
      sb::require_true(r.is_first());
      auto s = r.cast<cjson::doc>().root()["k"].str_or();
      sb::require(s.len, static_cast<usize>(run + 2));
      sb::require_true(s.ptr[0] == '\n');
      for ( u32 i = 0; i < run; ++i ) sb::require_true(s.ptr[1 + i] == char('a' + (i % 26)));
      sb::require_true(s.ptr[run + 1] == '\t');
    }
    sb::end_test_case();
  }
  {
    // the decoded bytes move DOWN over the source, so the copy has to tolerate overlap;
    // a string that is nothing but escapes maximises the shift
    sb::test_case("dense escape runs decode under maximum overlap");
    for ( u32 count : { 1u, 8u, 64u, 256u, 1024u } ) {
      micron::vector<u8> d;
      const char *pre = "{\"k\":\"";
      for ( usize i = 0; pre[i]; ++i ) d.push_back(u8(pre[i]));
      for ( u32 i = 0; i < count; ++i ) {
        d.push_back(u8('\\'));
        d.push_back(u8('u'));
        d.push_back(u8('0'));
        d.push_back(u8('0'));
        d.push_back(u8('4'));
        d.push_back(u8('1'));
      }
      d.push_back(u8('"'));
      d.push_back(u8('}'));

      auto r = cjson::parse(cjson::bytes{ d.cbegin(), d.size() });
      sb::require_true(r.is_first());
      auto s = r.cast<cjson::doc>().root()["k"].str_or();
      sb::require(s.len, static_cast<usize>(count));
      for ( u32 i = 0; i < count; ++i ) sb::require_true(s.ptr[i] == 'A');
    }
    sb::end_test_case();
  }
  {
    // names are unescaped by the same code as values, and a name with escapes must still
    // be findable by its DECODED spelling (rfc s8.3)
    sb::test_case("escaped names decode and remain findable");
    for ( u32 pad = 0; pad < 140; pad += 13 ) {
      micron::vector<u8> d;
      d.push_back(u8('{'));
      d.push_back(u8('"'));
      for ( u32 i = 0; i < pad; ++i ) d.push_back(u8('x'));
      const char *esc = "a\\u005Cb\\n";
      for ( usize i = 0; esc[i]; ++i ) d.push_back(u8(esc[i]));
      d.push_back(u8('"'));
      d.push_back(u8(':'));
      d.push_back(u8('7'));
      d.push_back(u8('}'));

      auto r = cjson::parse(cjson::bytes{ d.cbegin(), d.size() });
      sb::require_true(r.is_first());

      micron::vector<char> key;
      for ( u32 i = 0; i < pad; ++i ) key.push_back('x');
      key.push_back('a');
      key.push_back('\\');
      key.push_back('b');
      key.push_back('\n');
      sb::require(r.cast<cjson::doc>().root()[cjson::strv{ key.cbegin(), key.size() }].i64_or(-1), static_cast<i64>(7));
    }
    sb::end_test_case();
  }
  {
    sb::test_case("the escape-heavy corpus decodes and round-trips");
    auto data = tutil::slurp("sample/twitter.json");
    sb::require_greater(data.size(), static_cast<usize>(0));
    auto r = cjson::parse(tutil::view(data));
    sb::require_true(r.is_first());
    micron::string out = cjson::write_str(r.cast<cjson::doc>());
    auto r2 = cjson::parse(reinterpret_cast<const u8 *>(out.c_str()), out.size());
    sb::require_true(r2.is_first());
    micron::string out2 = cjson::write_str(r2.cast<cjson::doc>());
    sb::require(out.size(), out2.size());
    for ( usize i = 0; i < out.size(); ++i ) sb::require_true(out[i] == out2[i]);
    sb::end_test_case();
  }
  return 1;
}
