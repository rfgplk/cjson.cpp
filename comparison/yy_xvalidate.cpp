//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// differential validation against yyjson (0.12.x, built by scripts/vsbuild into
// comparison/.yyjson-build + libs/) and libc strtod. this tu is deliberately outside
// the micron-only rule (comparison/ is the exemption): it exists to disagree with
// cjson as loudly as possible.
//   build: duck build comparison/yy_xvalidate.cpp -i ../micron/tests -i ../micron \
//              -i ../micron/src -i comparison/.yyjson-build --lib yyjson --perf
//   (or:   g++ -std=c++26 -O2 -march=native -fext-numeric-literals -I../micron -I../micron/src \
//              -Icomparison/.yyjson-build comparison/yy_xvalidate.cpp libs/libyyjson.a -lpthread -o bin/yy_xvalidate)
// MUST be FP-safe (--perf / -O2/-O3, never duck's default -Ofast): the strtod oracle
// detects overflow via inf compares, which -ffinite-math-only folds to false.
// exit 1 == pass (duck test contract)

#include "../src/cjson/cjson.hpp"

#include <snowball/snowball.hpp>

#include <yyjson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace
{

struct rng {
  u64 s = 0xa0761d6478bd642full;

  u64
  next()
  {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }

  u32
  below(u32 lim)
  {
    return lim == 0 ? 0 : u32(next() % lim);
  }
};

bool
cj_ok(const char *p, usize n)
{
  return cjson::parse(p, n).is_first();
}

bool
yy_ok(const char *p, usize n)
{
  yyjson_doc *d = yyjson_read(p, n, 0);
  const bool ok = d != nullptr;
  if ( d ) yyjson_doc_free(d);
  return ok;
}

// recursive semantic comparison of a cjson val against a yyjson val
bool
same_tree(const cjson::val &a, yyjson_val *b)
{
  if ( !b ) return false;
  switch ( a.type() ) {
  case cjson::kind::null:
    return yyjson_is_null(b);
  case cjson::kind::boolean:
    return yyjson_is_bool(b) and yyjson_get_bool(b) == a.bool_or(false);
  case cjson::kind::number: {
    if ( !yyjson_is_num(b) ) return false;
    if ( yyjson_is_real(b) ) {
      const f64 x = a.f64_or(0);
      const f64 y = yyjson_get_real(b);
      return __builtin_bit_cast(u64, x) == __builtin_bit_cast(u64, y);
    }
    if ( yyjson_is_sint(b) and yyjson_get_sint(b) < 0 ) {
      auto t = a.try_i64();
      return t.is_first() and t.cast<i64>() == yyjson_get_sint(b);
    }
    auto t = a.try_u64();
    return t.is_first() and t.cast<u64>() == yyjson_get_uint(b);
  }
  case cjson::kind::string: {
    if ( !yyjson_is_str(b) ) return false;
    auto s = a.str_or();
    return s.len == yyjson_get_len(b) and std::memcmp(s.ptr, yyjson_get_str(b), s.len) == 0;
  }
  case cjson::kind::array: {
    if ( !yyjson_is_arr(b) or a.size() != yyjson_arr_size(b) ) return false;
    usize i = 0;
    bool ok = true;
    for ( auto e : a.items() ) {
      ok = ok and same_tree(e, yyjson_arr_get(b, i));
      ++i;
    }
    return ok;
  }
  case cjson::kind::object: {
    if ( !yyjson_is_obj(b) or a.size() != yyjson_obj_size(b) ) return false;
    // walk in document order on both sides
    yyjson_obj_iter it;
    yyjson_obj_iter_init(b, &it);
    bool ok = true;
    for ( auto m : a.members() ) {
      yyjson_val *bk = yyjson_obj_iter_next(&it);
      if ( !bk ) {
        ok = false;
        break;
      }
      ok = ok and yyjson_get_len(bk) == m.key.len and std::memcmp(yyjson_get_str(bk), m.key.ptr, m.key.len) == 0;
      ok = ok and same_tree(m.v, yyjson_obj_iter_get_val(bk));
    }
    return ok;
  }
  default:
    return false;
  }
}

char *
slurp(const char *path, usize &n)
{
  FILE *f = std::fopen(path, "rb");
  if ( !f ) return nullptr;
  std::fseek(f, 0, SEEK_END);
  const long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  char *buf = static_cast<char *>(std::malloc(usize(sz) + 1));
  n = std::fread(buf, 1, usize(sz), f);
  buf[n] = 0;
  std::fclose(f);
  return buf;
}

};      // namespace

int
main()
{
  {
    sb::test_case("accept/reject agreement on adversarial snippets");
    const char *cases[] = {
      "{}",
      "[]",
      "null",
      "1",
      "-0",
      "\"x\"",
      "[1,2,3]",
      "{\"a\":1}",
      "",
      " ",
      "{",
      "[",
      "]",
      "}",
      "[1,]",
      "{\"a\":}",
      "{\"a\" 1}",
      "{1:2}",
      "01",
      "+1",
      ".5",
      "1.",
      "1e",
      "-",
      "tru",
      "truex",
      "nul",
      "fals",
      "\"\\q\"",
      "\"\\u00g0\"",
      "\"\\ud800\"",
      "\"\\udc00\"",
      "\"\\ud800\\udc00\"",
      "\"unterminated",
      "\"tab\ttab\"",
      "[[[[]]]]",
      "1 2",
      "{} {}",
      "1e309",
      "-1e309",
      "1e-400",
      "18446744073709551615",
      "18446744073709551616",
      "-9223372036854775808",
      "-9223372036854775809",
      "0.1e2",
      "[\"\\u0000\"]",
    };
    for ( const char *c : cases ) {
      const usize n = std::strlen(c);
      const bool a = cj_ok(c, n);
      const bool b = yy_ok(c, n);
      if ( a != b ) {
        sb::print("DISAGREE on '", c, "': cjson=", (int)a, " yyjson=", (int)b);
        sb::require_true(false);
      }
    }
    sb::end_test_case();
  }
  {
    sb::test_case("corpus trees are semantically identical");
    // the tracked corpus is all one shape; the sample/web/ entries are the ones that
    // actually stress the number kernel (numbers/canada/mesh) and the unescape path
    // (twitterescaped). absent files are skipped, so this still runs without a fetch.
    const char *files[] = { "sample/64kb.json",
                            "sample/128KB.json",
                            "sample/512KB.json",
                            "sample/1MB.json",
                            "sample/twitter.json",
                            "sample/web/numbers.json",
                            "sample/web/canada.json",
                            "sample/web/mesh.json",
                            "sample/web/twitterescaped.json",
                            "sample/web/citm_catalog.json",
                            "sample/web/countries.geo.json",
                            "sample/web/update-center.json",
                            "sample/web/instruments.json",
                            "sample/web/github_events.json",
                            "sample/web/gsoc-2018.json",
                            "sample/web/marine_ik.json",
                            "sample/web/api.github.com.json" };
    for ( const char *f : files ) {
      usize n = 0;
      char *buf = slurp(f, n);
      sb::require_true(buf != nullptr);
      auto rc = cjson::parse(buf, n);
      yyjson_doc *yd = yyjson_read(buf, n, 0);
      sb::require_true(rc.is_first() and yd != nullptr);
      const cjson::doc &cd = rc.cast<cjson::doc>();
      sb::require_true(same_tree(cd.root(), yyjson_doc_get_root(yd)));
      yyjson_doc_free(yd);
      std::free(buf);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("random decimal texts agree bit for bit with strtod");
    rng rg;
    char txt[128];
    for ( u32 iter = 0; iter < 300000; iter++ ) {
      // random digit string with random exponent, full double range. one shape in four
      // is "0.00...ddd" — the leading-zero-fraction class that once hid a truncation bug
      char *w = txt;
      if ( rg.below(2) ) *w++ = '-';
      const bool zero_int = rg.below(4) == 0;
      if ( zero_int ) {
        *w++ = '0';
      } else {
        *w++ = char('1' + rg.below(9));
        const u32 ints = rg.below(19);
        for ( u32 i = 0; i < ints; i++ ) *w++ = char('0' + rg.below(10));
      }
      if ( zero_int or rg.below(2) ) {
        *w++ = '.';
        if ( zero_int ) {
          const u32 lead = rg.below(10);
          for ( u32 i = 0; i < lead; i++ ) *w++ = '0';
        }
        const u32 fr = 1 + rg.below(30);
        for ( u32 i = 0; i < fr; i++ ) *w++ = char('0' + rg.below(10));
      }
      if ( rg.below(2) ) {
        *w++ = 'e';
        if ( rg.below(2) ) *w++ = '-';
        const u32 mag = 1 + rg.below(320);
        w += std::snprintf(w, 8, "%u", mag);
      }
      *w = 0;
      const usize n = usize(w - txt);
      const f64 want = std::strtod(txt, nullptr);
      auto r = cjson::parse(txt, n);
      if ( want != want or want == __builtin_huge_val() or want == -__builtin_huge_val() ) {
        // strtod overflowed to inf: cjson must reject
        if ( !r.is_second() ) {
          sb::print("expected reject: ", txt);
          sb::require_true(false);
        }
        continue;
      }
      if ( r.is_second() ) {
        sb::print("unexpected reject: ", txt);
        sb::require_true(false);
        continue;
      }
      const cjson::doc &d = r.cast<cjson::doc>();
      const f64 got = d.root().f64_or(1234.5);
      if ( __builtin_bit_cast(u64, got) != __builtin_bit_cast(u64, want) ) {
        sb::print("bit mismatch on '", txt, "'");
        sb::require_true(false);
      }
    }
    sb::end_test_case();
  }
  {
    sb::test_case("random structural documents agree on accept/reject with yyjson");
    rng rg;
    const char alphabet[] = "{}[]:,\"\\ \t\n0123456789.eE+-truefalsnl xyz";
    for ( u32 iter = 0; iter < 50000; iter++ ) {
      char buf[220];
      const u32 n = 1 + rg.below(200);
      for ( u32 i = 0; i < n; i++ ) buf[i] = alphabet[rg.below(sizeof(alphabet) - 1)];
      buf[n] = 0;
      const bool a = cj_ok(buf, n);
      const bool b = yy_ok(buf, n);
      if ( a != b ) {
        sb::print("structural disagree (iter ", iter, ")");
        sb::require_true(false);
      }
    }
    sb::end_test_case();
  }
  return 1;
}
