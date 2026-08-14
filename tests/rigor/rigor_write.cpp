//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// the writers: write / write_str / write_into / minify / subtree writers, at every
// style, plus the bound contract.
//
// write_bound is the sharp one. It has two implementations -- an O(1) accumulation done
// during stage 2 under opts::with_write_bound, and an O(nvals) walk when that was not
// asked for -- and BOTH must be upper bounds on the same document. An under-report is
// not a slow path, it is a truncated write.

#include "../../src/cjson/cjson.hpp"

#include "../tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/string/strings.hpp>
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

const char *k_docs[] = {
  "{}",
  "[]",
  "null",
  "true",
  "0",
  R"("s")",
  R"({"a":1})",
  R"([1,2,3])",
  R"({"a":{"b":{"c":[1,[2,[3,[4]]]]}}})",
  R"({"esc":"\"\\\/\b\f\n\r\t","uni":"Aé𝄞","ctl":"\u0001\u001f"})",
  R"([0,-0,1,-1,1.5,-1.5e-10,1e308,5e-324,9007199254740991,18446744073709551615])",
  R"({"deep":[[[[[[[[[[{"x":null}]]]]]]]]]]})",
  R"({"dup":1,"dup":2,"dup":3})",
  R"([{},[],{"a":[]},[{}]])",
};

void
bound_holds(const cjson::doc &d, cjson::style st, const char *what)
{
  const usize bound = cjson::write_bound(d, st);
  micron::vector<u8> out;
  out.reserve(bound + cjson::padding + 1);
  const max_t n = cjson::write_into(d, cjson::wbytes{ out.begin(), bound }, st);
  if ( n < 0 ) {
    snowball::print("write_into failed inside its own bound: ", what);
    sb::require_true(false);
  }
  if ( usize(n) > bound ) {
    snowball::print("write_bound UNDER-reported: ", what);
    snowball::print("   bound ", bound, " actual ", usize(n));
  }
  sb::require_true(usize(n) <= bound);
  sb::require_true(cjson::validate(out.begin(), usize(n)) == cjson::error::ok);
}

};      // namespace

int
main()
{
  {
    sb::test_case("write output re-parses at every style");
    for ( const char *j : k_docs ) {
      usize n = 0;
      while ( j[n] ) ++n;
      auto r = cjson::parse(reinterpret_cast<const u8 *>(j), n);
      sb::require_true(r.is_first());
      for ( u8 indent : { u8(0), u8(1), u8(2), u8(4), u8(8) } ) {
        micron::string out = cjson::write_str(r.cast<cjson::doc>(), cjson::style{ .indent = indent });
        if ( cjson::validate(reinterpret_cast<const u8 *>(out.c_str()), out.size()) != cjson::error::ok )
          snowball::print("write output does not re-parse: ", j);
        sb::require_true(cjson::validate(reinterpret_cast<const u8 *>(out.c_str()), out.size()) == cjson::error::ok);
      }
    }
    sb::end_test_case();
  }
  {
    sb::test_case("write_bound holds in both its O(1) and walking forms");
    for ( const char *j : k_docs ) {
      usize n = 0;
      while ( j[n] ) ++n;
      for ( bool wb : { false, true } ) {
        auto r = cjson::parse(reinterpret_cast<const u8 *>(j), n, cjson::opts{ .with_write_bound = wb });
        sb::require_true(r.is_first());
        for ( u8 indent : { u8(0), u8(2), u8(4) } ) bound_holds(r.cast<cjson::doc>(), cjson::style{ .indent = indent }, j);
      }
    }
    sb::end_test_case();
  }
  {
    sb::test_case("write_bound holds over the corpus in both forms");
    const char *files[] = { "sample/64kb.json", "sample/twitter.json", "sample/128KB.json", "sample/256KB.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      for ( bool wb : { false, true } ) {
        auto r = cjson::parse(tutil::view(data), cjson::opts{ .with_write_bound = wb });
        sb::require_true(r.is_first());
        for ( u8 indent : { u8(0), u8(2) } ) bound_holds(r.cast<cjson::doc>(), cjson::style{ .indent = indent }, f);
      }
    }
    sb::end_test_case();
  }
  {
    sb::test_case("write_into reports short_output for every buffer below the true length");
    auto r = PJ(R"({"a":[1,2,3],"b":"xyz"})");
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();
    const usize need = cjson::write_bound(d);
    micron::vector<u8> out;
    out.reserve(need + cjson::padding);
    const max_t got = cjson::write_into(d, cjson::wbytes{ out.begin(), need });
    sb::require_true(got > 0);
    for ( usize cap = 0; cap < usize(got); ++cap ) {
      micron::vector<u8> small;
      small.reserve(need + cjson::padding);
      const max_t rc = cjson::write_into(d, cjson::wbytes{ small.begin(), cap });
      sb::require_true(rc < 0);
      sb::require_true(cjson::as_error(rc) == cjson::error::short_output);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("minify_bound is exact and minify never exceeds it");
    const char *files[] = { "sample/64kb.json", "sample/twitter.json" };
    for ( const char *f : files ) {
      auto data = tutil::slurp(f);
      sb::require_greater(data.size(), static_cast<usize>(0));
      const usize bound = cjson::minify_bound(data.size());
      sb::require(bound, data.size());
      micron::vector<u8> out;
      out.reserve(bound + cjson::padding);
      const max_t n = cjson::minify(tutil::view(data), cjson::wbytes{ out.begin(), bound });
      sb::require_true(n >= 0);
      sb::require_true(usize(n) <= bound);
      sb::require_true(cjson::validate(out.begin(), usize(n)) == cjson::error::ok);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("subtree writers emit each node independently");
    auto r = PJ(R"({"a":{"b":[1,{"c":"d"},null]},"e":"f","g":[]})");
    sb::require_true(r.is_first());
    auto root = r.cast<cjson::doc>().root();
    cjson::val nodes[] = { root, root["a"], root["a"]["b"], root["a"]["b"].at(1), root["e"], root["g"] };
    for ( cjson::val v : nodes ) {
      micron::string s = cjson::write_str(v);
      sb::require_greater(s.size(), static_cast<usize>(0));
      sb::require_true(cjson::validate(reinterpret_cast<const u8 *>(s.c_str()), s.size()) == cjson::error::ok);
      // the subtree bound must hold too
      const usize b = cjson::write_bound(v);
      micron::vector<u8> out;
      out.reserve(b + cjson::padding);
      const max_t n = cjson::write_into(v, cjson::wbytes{ out.begin(), b });
      sb::require_true(n >= 0 and usize(n) <= b);
    }
    sb::end_test_case();
  }
  {
    // indentation must be cosmetic: every style denotes the same document
    sb::test_case("indent changes only the layout, never the document");
    for ( const char *j : k_docs ) {
      usize n = 0;
      while ( j[n] ) ++n;
      auto r = cjson::parse(reinterpret_cast<const u8 *>(j), n);
      sb::require_true(r.is_first());
      micron::string flat = cjson::write_str(r.cast<cjson::doc>(), cjson::style{ .indent = 0 });
      for ( u8 indent : { u8(2), u8(4), u8(8) } ) {
        micron::string pretty = cjson::write_str(r.cast<cjson::doc>(), cjson::style{ .indent = indent });
        auto rp = cjson::minify_str(cjson::bytes{ reinterpret_cast<const u8 *>(pretty.c_str()), pretty.size() });
        sb::require_true(rp.is_first());
        const micron::string &m = rp.cast<micron::string>();
        sb::require(m.size(), flat.size());
        for ( usize i = 0; i < flat.size(); ++i ) sb::require_true(m[i] == flat[i]);
      }
    }
    sb::end_test_case();
  }
  {
    sb::test_case("write_str and write agree byte for byte");
    for ( const char *j : k_docs ) {
      usize n = 0;
      while ( j[n] ) ++n;
      auto r = cjson::parse(reinterpret_cast<const u8 *>(j), n);
      sb::require_true(r.is_first());
      micron::string s = cjson::write_str(r.cast<cjson::doc>());
      cjson::fjson f = cjson::write(r.cast<cjson::doc>());
      sb::require(s.size(), f.size());
      for ( usize i = 0; i < s.size(); ++i ) sb::require_true(u8(s[i]) == f[i]);
    }
    sb::end_test_case();
  }
  return 1;
}
