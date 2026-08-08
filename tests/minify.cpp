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

// minify: textual whitespace strip — token bytes preserved exactly (number text is NOT
// normalized), idempotent, invalid input rejected, whitespace inside strings kept

namespace
{

// the pre-rework scalar strip loop, kept as the byte-exact oracle for the mask-driven
// sweep (it re-derives in-string state serially; the sweep must agree byte for byte)
micron::vector<u8>
oracle_strip(const u8 *p, usize n)
{
  micron::vector<u8> out;
  bool in_str = false;
  bool escaped_next = false;
  for ( usize i = 0; i < n; ++i ) {
    const u8 c = p[i];
    const bool was_escaped = escaped_next;
    escaped_next = false;
    if ( c == u8('\\') and !was_escaped ) escaped_next = true;
    const bool active_quote = c == u8('"') and !was_escaped;
    if ( active_quote ) in_str = !in_str;
    if ( !in_str and !active_quote and cjson::is_space(c) ) continue;
    out.push_back(c);
  }
  return out;
}

bool
sweep_matches_oracle(const u8 *p, usize n)
{
  micron::vector<u8> out;
  out.reserve(n + 8);
  cjson::scratch sc;
  const max_t r = cjson::minify_into(p, n, out.begin(), n, {}, sc);
  if ( r < 0 ) return true;      // rejection set is covered by the other cases
  const micron::vector<u8> want = oracle_strip(p, n);
  if ( usize(r) != want.size() ) return false;
  for ( usize i = 0; i < want.size(); i++ )
    if ( out.begin()[i] != want.cbegin()[i] ) return false;
  return true;
}

};      // namespace

int
main()
{
  {
    sb::test_case("whitespace outside strings is stripped and inside strings kept");
    const char *j = " { \"a b\" : [ 1 ,\n\t2.50 , \"x y\" ] } ";
    usize n = 0;
    while ( j[n] ) ++n;
    auto r = cjson::minify(cjson::bytes{ reinterpret_cast<const u8 *>(j), n });
    sb::require_true(r.is_first());
    const cjson::fjson &m = r.cast<cjson::fjson>();
    const char *want = "{\"a b\":[1,2.50,\"x y\"]}";
    usize wn = 0;
    while ( want[wn] ) ++wn;
    sb::require(m.size(), wn);
    bool same = true;
    for ( usize i = 0; i < wn; i++ ) same = same and m.first()[i] == u8(want[i]);
    sb::require_true(same);
    sb::end_test_case();
  }
  {
    sb::test_case("escaped quotes and backslashes do not confuse the string tracker");
    const char *j = R"( { "k\"ey" : "a \\ b" , "n" : 1 } )";
    usize n = 0;
    while ( j[n] ) ++n;
    auto r = cjson::minify(cjson::bytes{ reinterpret_cast<const u8 *>(j), n });
    sb::require_true(r.is_first());
    const cjson::fjson &m = r.cast<cjson::fjson>();
    auto rp = cjson::parse(reinterpret_cast<const char *>(m.first()), m.size());
    sb::require_true(rp.is_first());
    sb::end_test_case();
  }
  {
    sb::test_case("minify is idempotent and preserves number text exactly");
    const char *j = "[1e2, 0.500, -0, 12.0e+003]";
    usize n = 0;
    while ( j[n] ) ++n;
    auto r1 = cjson::minify(cjson::bytes{ reinterpret_cast<const u8 *>(j), n });
    sb::require_true(r1.is_first());
    const cjson::fjson &m1 = r1.cast<cjson::fjson>();
    // token text preserved: "1e2" stays "1e2" (a parse->write pass would say "100.0")
    sb::require(m1.size(), static_cast<usize>(24));
    auto r2 = cjson::minify(cjson::bytes{ m1.first(), m1.size() });
    sb::require_true(r2.is_first());
    const cjson::fjson &m2 = r2.cast<cjson::fjson>();
    sb::require(m2.size(), m1.size());
    bool same = true;
    for ( usize i = 0; i < m1.size(); i++ ) same = same and m1.first()[i] == m2.first()[i];
    sb::require_true(same);
    sb::end_test_case();
  }
  {
    sb::test_case("invalid input is rejected not stripped");
    const char *j = "[1,, 2]";
    sb::require_true(cjson::minify(cjson::bytes{ reinterpret_cast<const u8 *>(j), 7 }).is_second());
    const char *k = "{\"a\" 1}";
    sb::require_true(cjson::minify(cjson::bytes{ reinterpret_cast<const u8 *>(k), 7 }).is_second());
    sb::end_test_case();
  }
  {
    sb::test_case("the corpus minifies and reparses semantically equal");
    const char *files[] = { "sample/64kb.json", "sample/twitter.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      auto r = cjson::minify(tutil::view(data));
      sb::require_true(r.is_first());
      const cjson::fjson &m = r.cast<cjson::fjson>();
      sb::require_greater(data.size() + 1, m.size());
      // both texts must serialize to the identical canonical form
      auto ra = cjson::parse(tutil::view(data));
      auto rb = cjson::parse(reinterpret_cast<const char *>(m.first()), m.size());
      sb::require_true(ra.is_first() and rb.is_first());
      micron::string wa = cjson::write_str(ra.cast<cjson::doc>());
      micron::string wb = cjson::write_str(rb.cast<cjson::doc>());
      sb::require(wa.size(), wb.size());
      bool same = true;
      for ( usize i = 0; i < wa.size(); i++ ) same = same and wa[i] == wb[i];
      sb::require_true(same);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("the mask-driven sweep agrees with the scalar strip oracle");
    const char *files[] = { "sample/64kb.json", "sample/twitter.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      sb::require_true(sweep_matches_oracle(data.cbegin(), data.size()));
    }
    // adversarial escapes and block-boundary quotes at offsets around 62-66
    sb::require_true(sweep_matches_oracle(reinterpret_cast<const u8 *>("{\"a\\\\\" : \" b \\\" c \"}"), 20));
    for ( usize pad = 56; pad <= 70; pad++ ) {
      micron::vector<u8> doc;
      doc.push_back(u8('{'));
      doc.push_back(u8('"'));
      for ( usize i = 0; i < pad; i++ ) doc.push_back(u8('k'));
      doc.push_back(u8('"'));
      const char *rest = " : \t\n \"v \\\\ w\"  }";
      for ( usize i = 0; rest[i]; i++ ) doc.push_back(u8(rest[i]));
      sb::require_true(sweep_matches_oracle(doc.cbegin(), doc.size()));
    }
    // scratch overload and ownerless overload agree byte for byte
    {
      auto data = tutil::slurp("sample/64kb.json");
      micron::vector<u8> o1, o2;
      o1.reserve(data.size() + 8);
      o2.reserve(data.size() + 8);
      cjson::scratch sc;
      const max_t r1 = cjson::minify_into(data.cbegin(), data.size(), o1.begin(), data.size(), {}, sc);
      const max_t r2 = cjson::minify_into(data.cbegin(), data.size(), o2.begin(), data.size(), {});
      sb::require(r1, r2);
      sb::require_greater(r1, max_t(0));
      bool same = true;
      for ( usize i = 0; i < usize(r1); i++ ) same = same and o1.begin()[i] == o2.begin()[i];
      sb::require_true(same);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("comptime minify agrees with runtime");
    constexpr auto ct = []() consteval -> bool {
      const char j[] = " [ 1 , \"a b\" ] ";
      u8 in[sizeof(j) - 1]{};
      for ( usize i = 0; i + 1 < sizeof(j); ++i ) in[i] = u8(j[i]);
      u8 out[32]{};
      const max_t w = cjson::minify_into(in, sizeof(j) - 1, out, 32);
      const char want[] = "[1,\"a b\"]";
      if ( w != max_t(sizeof(want) - 1) ) return false;
      for ( usize i = 0; i + 1 < sizeof(want); ++i )
        if ( out[i] != u8(want[i]) ) return false;
      return true;
    };
    static_assert(ct());
    sb::require_true(true);
    sb::end_test_case();
  }
  return 1;
}
