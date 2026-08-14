//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// Properties that only make sense once a document has parsed, so the input side is
// filtered rather than generated: mutate a seed, and if the result parses, hold the
// writer to it.
//
//   parse -> write -> parse   yields an EQUAL document (walked, not string-compared:
//                             a string comparison would just be testing the writer
//                             against itself)
//   minify                    is idempotent
//   pretty -> minify          == minify
//   write_bound               is never below the actual write length
//   parse_insitu              produces an equal document to parse
//
// Random bytes almost never parse, so the corpus mutator carries this file: the seeds are
// valid and small mutations frequently leave them valid.

#include "fuzz_corpus.hpp"

#include "../tutil.hpp"

#include <snowball/snowball.hpp>
#include <snowball/snowball_fuzz.hpp>

#include <micron/string/strings.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace sbf = snowball::fuzzing;

namespace
{

u32 g_parsed = 0;      // how many inputs actually reached the properties

void
check_roundtrip(const u8 *p, usize n)
{
  fz::tight t(p, n);
  auto r1 = cjson::parse(t.view());
  if ( !r1.is_first() ) return;
  ++g_parsed;
  const cjson::doc &d1 = r1.cast<cjson::doc>();

  // write -> parse -> compare by WALKING both trees
  micron::string w1 = cjson::write_str(d1);
  auto r2 = cjson::parse(reinterpret_cast<const u8 *>(w1.c_str()), w1.size());
  FUZZ_FAIL_IF(!r2.is_first(), "write output does not re-parse");
  const cjson::doc &d2 = r2.cast<cjson::doc>();
  FUZZ_FAIL_IF(!fz::same_value(d1.root(), d2.root()), "write/parse round trip changed the document");

  // and writing the re-parsed document is a fixpoint
  micron::string w2 = cjson::write_str(d2);
  FUZZ_FAIL_IF(w1.size() != w2.size(), "write is not a fixpoint (length)");
  for ( usize i = 0; i < w1.size(); ++i ) FUZZ_FAIL_IF(w1[i] != w2[i], "write is not a fixpoint (bytes)");

  // write_bound must never under-report, at any indent, with or without the O(1) path
  for ( u8 indent : { u8(0), u8(2), u8(4) } ) {
    const cjson::style st{ .indent = indent };
    const usize bound = cjson::write_bound(d1, st);
    micron::vector<u8> out;
    out.reserve(bound + cjson::padding + 1);
    const max_t got = cjson::write_into(d1, cjson::wbytes{ out.begin(), bound }, st);
    FUZZ_FAIL_IF(got < 0, "write_into failed inside its own bound");
    FUZZ_FAIL_IF(usize(got) > bound, "write_bound under-reported the emitted length");
    FUZZ_FAIL_IF(cjson::validate(out.begin(), usize(got)) != cjson::error::ok, "write_into output does not re-parse");
  }

  // minify is idempotent
  {
    fz::tight tm(p, n);
    auto m1 = cjson::minify_str(tm.view());
    FUZZ_FAIL_IF(!m1.is_first(), "minify failed on a document that parsed");
    const micron::string &a = m1.cast<micron::string>();
    auto m2 = cjson::minify_str(cjson::bytes{ reinterpret_cast<const u8 *>(a.c_str()), a.size() });
    FUZZ_FAIL_IF(!m2.is_first(), "minify of a minified document failed");
    const micron::string &b = m2.cast<micron::string>();
    FUZZ_FAIL_IF(a.size() != b.size(), "minify is not idempotent (length)");
    for ( usize i = 0; i < a.size(); ++i ) FUZZ_FAIL_IF(a[i] != b[i], "minify is not idempotent (bytes)");

    // pretty then minify lands back on plain minify -- SEMANTICALLY, not byte for byte.
    //
    // The two are different kinds of operation and it matters here: minify is TEXTUAL
    // (it strips whitespace and copies string contents verbatim, so `\/` stays `\/`),
    // while pretty PARSES AND RE-WRITES, and the writer escapes only control bytes, `"`
    // and `\` -- so a `\/` on the way in comes back out as a bare `/`. That is exactly
    // the s8.3 point that two spellings denote one string, and comparing bytes here
    // would be asserting that the writer preserves an input spelling it never promised
    // to preserve.
    auto pr = cjson::pretty(tm.view(), 2);
    FUZZ_FAIL_IF(!pr.is_first(), "pretty failed on a document that parsed");
    const micron::string &pp = pr.cast<micron::string>();
    auto m3 = cjson::minify_str(cjson::bytes{ reinterpret_cast<const u8 *>(pp.c_str()), pp.size() });
    FUZZ_FAIL_IF(!m3.is_first(), "minify of pretty output failed");
    const micron::string &c = m3.cast<micron::string>();
    auto pa = cjson::parse(reinterpret_cast<const u8 *>(a.c_str()), a.size());
    auto pc = cjson::parse(reinterpret_cast<const u8 *>(c.c_str()), c.size());
    FUZZ_FAIL_IF(!pa.is_first() or !pc.is_first(), "minify or pretty->minify output does not re-parse");
    FUZZ_FAIL_IF(!fz::same_value(pa.cast<cjson::doc>().root(), pc.cast<cjson::doc>().root()),
                 "pretty->minify and minify denote different documents");
  }

  // insitu must build the same tree as copy mode
  {
    fz::tight ti(p, n);
    auto ri = cjson::parse_insitu(cjson::wbytes{ ti.p, ti.n });
    FUZZ_FAIL_IF(!ri.is_first(), "parse_insitu failed where parse succeeded");
    FUZZ_FAIL_IF(!fz::same_value(d1.root(), ri.cast<cjson::doc>().root()), "insitu and copy mode built different documents");
  }

  // and so must the borrowing reuse path
  {
    fz::tight tr(p, n);
    cjson::scratch sc;
    auto rr = cjson::parse_reuse(tr.view(), {}, sc);
    FUZZ_FAIL_IF(!rr.is_first(), "parse_reuse failed where parse succeeded");
    FUZZ_FAIL_IF(!fz::same_value(d1.root(), rr.cast<cjson::doc>().root()), "reuse and owning mode built different documents");
  }
}

// direct callers must catch: FUZZ_FAIL throws, and only check_property has a handler.
// An uncaught throw is a SIGABRT with no message, which is a bad way to learn anything.
void
run(const u8 *p, usize n, const char *what)
{
  try {
    check_roundtrip(p, n);
  } catch ( const char *msg ) {
    snowball::print("round trip FAILED [", what, "]: ", msg);
    snowball::print("   input length ", n);
    micron::string dump;
    for ( usize i = 0; i < n and i < 200; ++i ) dump.push_back(char(p[i] >= 0x20 and p[i] < 0x7f ? char(p[i]) : '.'));
    snowball::print("   input ", dump.c_str());
    sb::require_true(false);
  }
}

};      // namespace

int
main()
{
  {
    sb::test_case("the seed corpus round-trips");
    for ( usize s = 0; s < fz::seed_count; ++s ) {
      auto v = fz::seed_bytes(s);
      run(v.cbegin(), v.size(), "seed");
    }
    sb::require_greater(g_parsed, static_cast<u32>(0));
    sb::end_test_case();
  }
  {
    sb::test_case("mutated seeds that still parse round-trip");
    fz::rng r(0x120057E5ull);
    for ( usize s = 0; s < fz::seed_count; ++s ) {
      for ( u32 iter = 0; iter < 4000; ++iter ) {
        micron::vector<u8> v = fz::seed_bytes(s);
        fz::mutate(v, r, 1 + r.below(2));
        run(v.cbegin(), v.size(), "mutated seed");
      }
    }
    // if mutation left nothing parseable the property never ran, which must not read as
    // a pass -- the seeds are valid, so single mutations should leave many of them valid
    snowball::print("round trip: ", g_parsed, " documents actually reached the properties");
    sb::require_greater(g_parsed, static_cast<u32>(1000));
    sb::end_test_case();
  }
  {
    sbf::check_property(
        "generated byte vectors that parse also round-trip", [](micron::vector<u8> v) { check_roundtrip(v.cbegin(), v.size()); },
        { .seed = 0x120057E6ull, .count = 8000 }, sbf::vector_of(sbf::range<u8>(0, 255)).len(0, 96));
  }
  {
    // the corpus, which exercises shapes the seeds do not: deep nesting, long strings,
    // dense float arrays, escape-heavy text
    sb::test_case("the corpus round-trips");
    const char *files[] = { "sample/64kb.json", "sample/128KB.json", "sample/twitter.json", "sample/256KB.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      run(data.cbegin(), data.size(), f);
    }
    sb::end_test_case();
  }
  snowball::print("fuzz_roundtrip: ", g_parsed, " documents held every property");
  return 1;
}
