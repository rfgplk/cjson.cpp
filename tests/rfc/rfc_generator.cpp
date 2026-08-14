//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// rfc 8259 s10, in full: "A JSON generator produces JSON text. The resulting text MUST
// strictly conform to the JSON grammar."
//
// So every byte cjson emits -- write, write_str, write_into, minify, compact, pretty,
// reformat, builder -- is fed straight back through the STRICT reader, and the reader is
// the judge. Anything that fails to re-parse is an s10 violation regardless of how
// reasonable it looks.
//
// The two places a json writer classically breaks s10 are non-finite doubles (emitting
// bare Infinity/NaN, which are not in the s6 grammar) and unescaped control characters in
// strings (s7 requires U+0000..U+001F be escaped). Both get their own block.

#include "rfc_cases.hpp"

#include "../tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

// lengths are DEDUCED from the literal, never hand-counted. A count that is too small
// does not fail loudly -- it silently parses a shorter, still-valid document, and the
// test then asserts things about a document nobody wrote. This bit twice while these
// files were being written.
template<usize N>
inline cjson::result<cjson::doc>
PJ(const char (&s)[N])
{
  return cjson::parse(reinterpret_cast<const u8 *>(s), N - 1);
}

// re-parse whatever was emitted; the strict reader is the s10 judge
void
must_reparse(const char *p, usize n, const char *what)
{
  const cjson::error e = cjson::validate(reinterpret_cast<const u8 *>(p), n);
  if ( e != cjson::error::ok ) {
    snowball::print("s10 VIOLATION: emitted text does not re-parse [", what, "]");
    snowball::print("   error ", cjson::error_name(e));
    snowball::print("   text  ", p);
  }
  sb::require_true(e == cjson::error::ok);
}

void
must_reparse(const micron::string &s, const char *what)
{
  must_reparse(s.c_str(), s.size(), what);
}

// a document is round-trip stable when writing it twice through a re-parse is a fixpoint
void
round_trip(const cjson::bytes in, const char *what)
{
  auto r1 = cjson::parse(in);
  sb::require_true(r1.is_first());
  micron::string w1 = cjson::write_str(r1.cast<cjson::doc>());
  must_reparse(w1, what);

  auto r2 = cjson::parse(reinterpret_cast<const u8 *>(w1.c_str()), w1.size());
  sb::require_true(r2.is_first());
  micron::string w2 = cjson::write_str(r2.cast<cjson::doc>());

  if ( w1.size() != w2.size() ) snowball::print("s10 round trip is not a fixpoint: ", what);
  sb::require(w1.size(), w2.size());
  for ( usize i = 0; i < w1.size(); ++i ) sb::require_true(w1[i] == w2[i]);
}

bool
contains(const micron::string &s, const char *needle)
{
  usize m = 0;
  while ( needle[m] ) ++m;
  if ( s.size() < m ) return false;
  for ( usize i = 0; i + m <= s.size(); ++i ) {
    bool hit = true;
    for ( usize j = 0; j < m; ++j )
      if ( s[i + j] != needle[j] ) {
        hit = false;
        break;
      }
    if ( hit ) return true;
  }
  return false;
}

};      // namespace

int
main()
{
  {
    sb::test_case("s10: write output re-parses at every indent");
    const char *docs[] = {
      "{}",
      "[]",
      "null",
      "42",
      "-1.5e-10",
      R"("a string")",
      R"({"a":1,"b":[1,2,3],"c":{"d":null},"e":true,"f":false})",
      R"([[],[[]],[[[]]],{},{"a":{}}])",
      R"({"esc":"\"\\\/\b\f\n\r\t","uni":"Aé𝄞"})",
      R"({"nums":[0,-0,1e100,1e-100,9007199254740991,1.7976931348623157e308]})",
    };
    for ( const char *d : docs ) {
      usize n = 0;
      while ( d[n] ) ++n;
      auto r = cjson::parse(reinterpret_cast<const u8 *>(d), n);
      sb::require_true(r.is_first());
      const cjson::doc &doc = r.cast<cjson::doc>();
      for ( u8 indent : { u8(0), u8(2), u8(4) } ) {
        micron::string out = cjson::write_str(doc, cjson::style{ .indent = indent });
        must_reparse(out, d);
      }
      round_trip(cjson::bytes{ reinterpret_cast<const u8 *>(d), n }, d);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("s10: minify, pretty, compact and reformat all emit conforming text");
    const char *files[] = { "sample/64kb.json", "sample/128KB.json", "sample/twitter.json", "sample/512KB.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      const cjson::bytes v = tutil::view(data);

      auto mc = cjson::minify_str(v);
      sb::require_true(mc.is_first());
      must_reparse(mc.cast<micron::string>(), f);

      auto pc = cjson::pretty(v, 2);
      sb::require_true(pc.is_first());
      must_reparse(pc.cast<micron::string>(), f);

      auto cc = cjson::compact(v);
      sb::require_true(cc.is_first());
      must_reparse(cc.cast<micron::string>(), f);

      round_trip(v, f);
    }
    sb::end_test_case();
  }
  {
    // minify is idempotent, and pretty then minify lands back on plain minify -- if
    // either fails, one of the two writers is emitting something the other re-reads
    // differently, which is an s10 problem hiding as a formatting one
    sb::test_case("s10: minify is idempotent and pretty round-trips to it");
    const char *files[] = { "sample/64kb.json", "sample/twitter.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));

      auto m1 = cjson::minify_str(tutil::view(data));
      sb::require_true(m1.is_first());
      const micron::string &a = m1.cast<micron::string>();

      auto m2 = cjson::minify_str(cjson::bytes{ reinterpret_cast<const u8 *>(a.c_str()), a.size() });
      sb::require_true(m2.is_first());
      const micron::string &b = m2.cast<micron::string>();
      sb::require(a.size(), b.size());
      for ( usize i = 0; i < a.size(); ++i ) sb::require_true(a[i] == b[i]);

      // pretty->minify equals minify SEMANTICALLY, not byte for byte: minify is textual
      // and preserves an input's escape spelling, while pretty parses and re-writes, so
      // a `\/` on the way in returns as a bare `/`. s8.3 -- two spellings, one string.
      auto p = cjson::pretty(tutil::view(data), 4);
      sb::require_true(p.is_first());
      const micron::string &pp = p.cast<micron::string>();
      auto m3 = cjson::minify_str(cjson::bytes{ reinterpret_cast<const u8 *>(pp.c_str()), pp.size() });
      sb::require_true(m3.is_first());
      const micron::string &c = m3.cast<micron::string>();
      auto pa = cjson::parse(reinterpret_cast<const u8 *>(a.c_str()), a.size());
      auto pc = cjson::parse(reinterpret_cast<const u8 *>(c.c_str()), c.size());
      sb::require_true(pa.is_first() and pc.is_first());
      micron::string wa = cjson::write_str(pa.cast<cjson::doc>());
      micron::string wc = cjson::write_str(pc.cast<cjson::doc>());
      sb::require(wa.size(), wc.size());
      for ( usize i = 0; i < wa.size(); ++i ) sb::require_true(wa[i] == wc[i]);
    }
    sb::end_test_case();
  }
  {
    // s6: "Numeric values that cannot be represented in the grammar below (such as
    // Infinity and NaN) are not permitted." A generator that emits them breaks s10.
    // cjson writes null instead (documented js-parity departure) -- pin BOTH halves:
    // the text conforms, AND the forbidden spellings never appear.
    sb::test_case("s10: non-finite doubles never reach the output as Infinity or NaN");
    // the non-finite values are built from BIT PATTERNS, never from 1.0/0.0: the default
    // recipe is -Ofast, whose -ffinite-math-only lets the compiler fold a division-derived
    // infinity into anything it likes. This is the same trap CLAUDE.md records for
    // comparison/yy_xvalidate.cpp. The writer's own check is a bit test on the exponent
    // field, so it survives -Ofast -- which is exactly what this asserts.
    const f64 inf = __builtin_bit_cast(f64, 0x7ff0000000000000ull);
    const f64 ninf = __builtin_bit_cast(f64, 0xfff0000000000000ull);
    const f64 nan = __builtin_bit_cast(f64, 0x7ff8000000000000ull);
    sb::require_true(__builtin_bit_cast(u64, inf) == 0x7ff0000000000000ull);

    // NOTE the keys are deliberately opaque. Naming them "inf"/"nan" would plant those
    // very spellings in the output and make the substring search below assert nothing.
    cjson::builder b;
    b.obj().key("a").value(inf).key("b").value(ninf).key("c").value(nan).key("d").value(1.5).end();
    micron::string out = b.take();
    if ( cjson::validate(reinterpret_cast<const u8 *>(out.c_str()), out.size()) != cjson::error::ok )
      snowball::print("   emitted: ", out.c_str());
    must_reparse(out, "builder with non-finite doubles");

    // none of the s6-forbidden spellings may appear anywhere in the emitted text
    sb::require_false(contains(out, "Infinity"));
    sb::require_false(contains(out, "infinity"));
    sb::require_false(contains(out, "INF"));
    sb::require_false(contains(out, "inf"));
    sb::require_false(contains(out, "NaN"));
    sb::require_false(contains(out, "nan"));
    sb::require_false(contains(out, "NAN"));
    sb::require_false(contains(out, "1e999"));
    sb::require_true(contains(out, "null"));

    auto r = cjson::parse(reinterpret_cast<const u8 *>(out.c_str()), out.size());
    sb::require_true(r.is_first());
    sb::require_true(r.cast<cjson::doc>().root()["a"].is_null());
    sb::require_true(r.cast<cjson::doc>().root()["b"].is_null());
    sb::require_true(r.cast<cjson::doc>().root()["c"].is_null());
    sb::require_true(r.cast<cjson::doc>().root()["d"].f64_or(0.0) == 1.5);

    // the DOM writer takes the same route as the builder
    {
      auto rd = PJ(R"({"x":1.5})");
      sb::require_true(rd.is_first());
      auto d = micron::move(rd.cast<cjson::doc>());
      d.edit()["x"] = inf;
      sb::require_true(d.mut_error() == cjson::error::ok);
      micron::string w = cjson::write_str(d);
      must_reparse(w, "dom writer with a non-finite double");
      sb::require_false(contains(w, "inf"));
      sb::require_true(contains(w, "null"));
    }
    sb::end_test_case();
  }
  {
    // s7 requires U+0000..U+001F be escaped. Feed every one of them in as an escape,
    // write it back out, and require that the emitted text carries no RAW control byte
    // and still re-parses to the identical string.
    sb::test_case("s10: control characters are always escaped on the way out");
    for ( u32 c = 0x00; c <= 0x1f; ++c ) {
      // build {"k":"<\uXXXX>"} as input text
      micron::vector<u8> in;
      const char *pre = "{\"k\":\"\\u00";
      for ( usize i = 0; pre[i]; ++i ) in.push_back(u8(pre[i]));
      const char *hex = "0123456789ABCDEF";
      in.push_back(u8(hex[(c >> 4) & 0xf]));
      in.push_back(u8(hex[c & 0xf]));
      in.push_back(u8('"'));
      in.push_back(u8('}'));

      auto r = cjson::parse(cjson::bytes{ in.cbegin(), in.size() });
      sb::require_true(r.is_first());
      micron::string out = cjson::write_str(r.cast<cjson::doc>());
      must_reparse(out, "control character round trip");

      // no raw control byte may appear anywhere in the emitted text
      for ( usize i = 0; i < out.size(); ++i ) {
        if ( u8(out[i]) <= 0x1f ) snowball::print("s10 VIOLATION: raw control byte in output, code ", c);
        sb::require_true(u8(out[i]) > 0x1f);
      }

      // and the value survives
      auto r2 = cjson::parse(reinterpret_cast<const u8 *>(out.c_str()), out.size());
      sb::require_true(r2.is_first());
      auto s = r2.cast<cjson::doc>().root()["k"].str_or();
      sb::require(s.len, static_cast<usize>(1));
      sb::require_true(u8(s.ptr[0]) == u8(c));
    }
    sb::end_test_case();
  }
  {
    sb::test_case("s10: quotation mark and reverse solidus are escaped on the way out");
    auto r = PJ(R"({"k":"a\"b\\c/d"})");
    sb::require_true(r.is_first());
    micron::string out = cjson::write_str(r.cast<cjson::doc>());
    must_reparse(out, "quote/backslash round trip");
    auto r2 = cjson::parse(reinterpret_cast<const u8 *>(out.c_str()), out.size());
    sb::require_true(r2.is_first());
    auto a = r.cast<cjson::doc>().root()["k"].str_or();
    auto b = r2.cast<cjson::doc>().root()["k"].str_or();
    sb::require(a.len, b.len);
    for ( usize i = 0; i < a.len; ++i ) sb::require_true(a.ptr[i] == b.ptr[i]);
    sb::end_test_case();
  }
  {
    sb::test_case("s10: builder output conforms for every value kind");
    cjson::builder b;
    b.obj()
        .key("null")
        .null()
        .key("true")
        .value(true)
        .key("false")
        .value(false)
        .key("i64")
        .value(static_cast<i64>(-9223372036854775807LL - 1))
        .key("u64")
        .value(static_cast<u64>(18446744073709551615ULL))
        .key("f64")
        .value(-1.5e-10)
        .key("str")
        .value("a\"b\\c\nd")
        .key("arr")
        .arr()
        .value(static_cast<i64>(1))
        .value("two")
        .null()
        .end()
        .key("obj")
        .obj()
        .key("nested")
        .value(true)
        .end()
        .end();
    micron::string out = b.take();
    must_reparse(out, "builder, all kinds");

    auto r = cjson::parse(reinterpret_cast<const u8 *>(out.c_str()), out.size());
    sb::require_true(r.is_first());
    auto root = r.cast<cjson::doc>().root();
    sb::require_true(root["null"].is_null());
    sb::require_true(root["true"].bool_or(false));
    sb::require_false(root["false"].bool_or(true));
    sb::require(root["u64"].u64_or(0), static_cast<u64>(18446744073709551615ULL));
    sb::require(root["arr"].size(), static_cast<usize>(3));
    sb::require_true(root["obj"]["nested"].bool_or(false));
    sb::end_test_case();
  }
  {
    // s10 is a promise the LIBRARY keeps. builder::raw() hands the promise to the caller
    // by contract ("preserialized, trusted verbatim"), so the trust boundary is pinned
    // here as a tested property rather than left as a silent hazard.
    sb::test_case("s10: builder::raw is a documented caller-trusted hole");
    {
      cjson::builder good;
      good.obj().key("a").raw(cjson::as_strv("[1,2,3]")).end();
      micron::string out = good.take();
      must_reparse(out, "builder::raw with conforming json");
    }
    {
      cjson::builder bad;
      bad.obj().key("a").raw(cjson::as_strv("[1,,3]")).end();
      micron::string out = bad.take();
      // the library does not re-validate what raw() was handed: garbage in, garbage out
      sb::require_true(cjson::validate(reinterpret_cast<const u8 *>(out.c_str()), out.size()) != cjson::error::ok);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("s10: write_into short-output is reported, never a truncated document");
    auto r = PJ(R"({"a":[1,2,3],"b":"xyz"})");
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    const usize need = cjson::write_bound(d);

    micron::vector<u8> big;
    big.reserve(need + cjson::padding);
    const max_t got = cjson::write_into(d, cjson::wbytes{ big.begin(), need });
    sb::require_true(got > 0);
    must_reparse(reinterpret_cast<const char *>(big.begin()), static_cast<usize>(got), "write_into exact bound");

    // every buffer smaller than the true length must FAIL, not emit a prefix
    for ( usize cap = 0; cap < static_cast<usize>(got); ++cap ) {
      micron::vector<u8> small;
      small.reserve(static_cast<usize>(got) + cjson::padding);
      const max_t rc = cjson::write_into(d, cjson::wbytes{ small.begin(), cap });
      sb::require_true(rc < 0);
      sb::require_true(cjson::as_error(rc) == cjson::error::short_output);
    }
    sb::end_test_case();
  }
  {
    // write_bound is a promise the writer relies on; if it ever under-reports, write_into
    // truncates for real
    sb::test_case("s10: write_bound never under-reports the emitted length");
    const char *files[] = { "sample/64kb.json", "sample/twitter.json", "sample/128KB.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      for ( bool wb : { false, true } ) {
        auto r = cjson::parse(tutil::view(data), cjson::opts{ .with_write_bound = wb });
        sb::require_true(r.is_first());
        const cjson::doc &d = r.cast<cjson::doc>();
        for ( u8 indent : { u8(0), u8(2) } ) {
          const cjson::style st{ .indent = indent };
          const usize bound = cjson::write_bound(d, st);
          micron::vector<u8> out;
          out.reserve(bound + cjson::padding);
          const max_t n = cjson::write_into(d, cjson::wbytes{ out.begin(), bound }, st);
          sb::require_true(n >= 0);
          sb::require_true(static_cast<usize>(n) <= bound);
          must_reparse(reinterpret_cast<const char *>(out.begin()), static_cast<usize>(n), f);
        }
      }
    }
    sb::end_test_case();
  }
  {
    // subtree writers are generators too
    sb::test_case("s10: subtree writers emit conforming text");
    auto r = PJ(R"({"a":{"b":[1,{"c":"d"},null]},"e":"f"})");
    sb::require_true(r.is_first());
    auto root = r.cast<cjson::doc>().root();
    must_reparse(cjson::write_str(root), "subtree root");
    must_reparse(cjson::write_str(root["a"]), "subtree object");
    must_reparse(cjson::write_str(root["a"]["b"]), "subtree array");
    must_reparse(cjson::write_str(root["a"]["b"][usize(1)]), "subtree nested object");
    must_reparse(cjson::write_str(root["e"]), "subtree string");
    sb::end_test_case();
  }
  return 1;
}
