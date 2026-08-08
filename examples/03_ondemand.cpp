// 03_ondemand.cpp
//
// See also:
//   examples/02_dom.cpp      — the owning document, same reads, more memory
//   examples/05_oneshot.cpp  — this path with the bookkeeping removed

#include "_ex_common.hpp"
#include <micron/std.hpp>

int
main()
{
  constexpr const char k_src[] = R"({
    "sub": "user-42",
    "exp": 1735689600,
    "admin": false,
    "scopes": ["read", "write", "admin"],
    "profile": { "name": "ada", "team": { "id": 7, "name": "core" } },
    "escaped": "a\nb"
  })";
  constexpr usize k_len = sizeof(k_src) - 1;

  // one scratch, reused for every read below
  cjson::scratch sc;

  auto rv = cjson::iterate(k_src, k_len, sc);
  if ( rv.is_second() ) {
    mc::echo("iterate failed: ", cjson::error_name(rv.cast<cjson::error>()));
    return 1;
  }
  const cjson::view v = rv.cast<cjson::view>();
  const cjson::cur root = v.root();

  // scalars
  ex::head("scalars");

  mc::echo("exp        = ", root["exp"].i64_or(0));
  mc::echo("admin      = ", root["admin"].bool_or(true));
  ex::show("sub        = ", root["sub"].str_raw());

  // NOTE str_raw is RAW: escapes are not decoded
  ex::show("escaped raw= ", root["escaped"].str_raw());

  // to decode, hand it a buffer
  u8 buf[64];
  const max_t w = root["escaped"].str(cjson::wbytes{ buf, sizeof(buf) });
  if ( w >= 0 ) mc::echo("escaped decoded length = ", usize(w), " (the \\n became one byte)");

  // navigation
  ex::head("navigation");

  ex::show("profile.team.name = ", root["profile"]["team"]["name"].str_raw());
  mc::echo("profile.team.id   = ", root["profile"]["team"]["id"].i64_or(-1));
  ex::show("scopes[2]         = ", root["scopes"].at(2).str_raw());
  mc::echo("misses chain: ", !root["nope"]["deeper"] ? "yes" : "no");

  // json pointer works here too
  ex::show("/profile/name     = ", root.at_pointer("/profile/name").str_raw());

  // iteration
  // items()/members() walk the structural index
  ex::head("iteration");

  mc::echo("scopes:");
  for ( auto s : root["scopes"].items() ) ex::show("  ", s.str_raw());

  mc::echo("profile members:");
  for ( auto m : root["profile"].members() ) ex::show("  ", m.key);

  // count() also walks
  mc::echo("scopes count = ", root["scopes"].count());

  // NOTE: prefer items() to at(i) in a loop: at(i) restarts from the container's opening bracket every call
  {
    usize by_walk = 0;
    for ( auto s : root["scopes"].items() ) by_walk += s.str_raw().len;
    usize by_index = 0;
    for ( usize i = 0; i < root["scopes"].count(); ++i ) by_index += root["scopes"].at(i).str_raw().len;
    mc::echo("same answer, different cost: ", by_walk, " == ", by_index);
  }

  // borrowing
  ex::head("borrowing");

  mc::echo("root still readable: exp = ", root["exp"].i64_or(0));

  // a second iterate on the same scratch invalidates everything above
  // (even a failing one)
  {
    auto dead = cjson::iterate("{ nope", 6, sc);
    mc::echo("second (failed) iterate on sc: ", dead.is_second() ? "rejected" : "accepted");
    mc::echo("-> `root` and `v` are now DANGLING. Reading them here would be a bug.");
  }

  // restart again on the same scratch
  auto rv2 = cjson::iterate(k_src, k_len, sc);
  if ( rv2.is_first() ) mc::echo("re-iterated, exp = ", rv2.cast<cjson::view>().root()["exp"].i64_or(0));

  // release() frees the scratch's buffers
  sc.release();
  mc::echo("scratch released");

  mc::echo("");
  return 0;
}
