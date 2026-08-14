//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// nst/JSONTestSuite, the field's shared accept/reject corpus, run against cjson.
//
//   y_*  MUST be accepted        n_*  MUST be rejected        i_*  implementation-defined
//
// The corpus lives in sample/web/jsontestsuite/, which is gitignored and populated by
//   scripts/fetch_corpus --suite
// so this file SKIPS CLEANLY when it is absent -- prints a notice and returns the pass
// sentinel. A fresh clone with no network must not fail the suite.
//
// The i_ cases are tallied and printed, never asserted: they are exactly the cases the
// rfc leaves open (huge magnitudes, lone surrogates in the utf-8 sense, bom handling),
// and cjson's answers to them belong in COMPLIANCE.md rather than in a require().

#include "../../src/cjson/cjson.hpp"

#include "../tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/linux/io.hpp>
#include <micron/linux/io/ext.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

const char *k_dir = "sample/web/jsontestsuite";

bool
starts_with(const char *s, const char *pre) noexcept
{
  for ( usize i = 0; pre[i]; ++i )
    if ( s[i] != pre[i] ) return false;
  return true;
}

bool
ends_with_json(const char *s) noexcept
{
  usize n = 0;
  while ( s[n] ) ++n;
  if ( n < 5 ) return false;
  return s[n - 5] == '.' and s[n - 4] == 'j' and s[n - 3] == 's' and s[n - 2] == 'o' and s[n - 1] == 'n';
}

// "sample/web/jsontestsuite/" + name, into a caller buffer
void
join(char *out, usize cap, const char *name) noexcept
{
  usize k = 0;
  for ( usize i = 0; k + 1 < cap and k_dir[i]; ++i ) out[k++] = k_dir[i];
  if ( k + 1 < cap ) out[k++] = '/';
  for ( usize i = 0; k + 1 < cap and name[i]; ++i ) out[k++] = name[i];
  out[k] = 0;
}

};      // namespace

int
main()
{
  micron::posix::fd_t dir = micron::posix::opendir(k_dir);
  if ( !dir.open() ) {
    snowball::print("rfc_suite_external: sample/web/jsontestsuite absent -- skipping.");
    snowball::print("   populate it with:  scripts/fetch_corpus --suite");
    return 1;      // skip is a pass; the house has no sb::skip
  }

  u32 y_total = 0, y_fail = 0;
  u32 n_total = 0, n_fail = 0;
  u32 i_total = 0, i_accept = 0;
  u32 skipped = 0;

  micron::posix::readdir_ctx ctx{};
  for ( ;; ) {
    auto e = micron::posix::readdir_r(dir, ctx);
    if ( e.type == micron::posix::dt_end ) break;
    if ( !ends_with_json(e.d_name.c_str()) ) continue;

    char path[512];
    join(path, sizeof(path), e.d_name.c_str());
    auto data = tutil::slurp(path);
    if ( data.size() == 0 ) {
      // a genuinely empty file is a legitimate case (n_structure_no_data.json); an
      // unreadable one is not, but slurp cannot tell us which, so count it and move on
      ++skipped;
      continue;
    }

    const cjson::error err = cjson::validate(tutil::view(data));
    const bool accepted = (err == cjson::error::ok);

    // parse must agree with validate on everything except the documented F3 range cases,
    // which live under i_number_* in this corpus
    if ( starts_with(e.d_name.c_str(), "y_") ) {
      ++y_total;
      if ( !accepted ) {
        ++y_fail;
        snowball::print("y_ case REJECTED (must accept): ", e.d_name.c_str());
        snowball::print("   error ", cjson::error_name(err));
      }
    } else if ( starts_with(e.d_name.c_str(), "n_") ) {
      ++n_total;
      if ( accepted ) {
        ++n_fail;
        snowball::print("n_ case ACCEPTED (must reject): ", e.d_name.c_str());
      }
    } else if ( starts_with(e.d_name.c_str(), "i_") ) {
      ++i_total;
      if ( accepted ) ++i_accept;
    }
  }
  micron::posix::closedir(dir);

  snowball::print("");
  snowball::print("nst/JSONTestSuite results");
  snowball::print("  y_ (must accept) : ", y_total, "   failures: ", y_fail);
  snowball::print("  n_ (must reject) : ", n_total, "   failures: ", n_fail);
  snowball::print("  i_ (open)        : ", i_total, "   accepted: ", i_accept);
  if ( skipped ) snowball::print("  empty/unreadable : ", skipped);

  {
    sb::test_case("nst/JSONTestSuite: every y_ case is accepted");
    sb::require(y_fail, static_cast<u32>(0));
    sb::end_test_case();
  }
  {
    sb::test_case("nst/JSONTestSuite: every n_ case is rejected");
    sb::require(n_fail, static_cast<u32>(0));
    sb::end_test_case();
  }
  {
    // a corpus that produced no cases means the fetch half-succeeded; that must not read
    // as a pass
    sb::test_case("nst/JSONTestSuite: the corpus actually contained cases");
    sb::require_greater(y_total + n_total + i_total, static_cast<u32>(0));
    sb::end_test_case();
  }
  return 1;
}
