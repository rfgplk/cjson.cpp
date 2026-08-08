// 02_dom.cpp
//
// See also:
//   examples/01_quickstart.cpp — the five-minute tour
//   examples/03_ondemand.cpp   — the same reads with nothing materialized
//   examples/04_functional.cpp — pipelines over the ranges shown here

#include "_ex_common.hpp"

#include <micron/std.hpp>

int
main()
{
  constexpr const char k_src[] = R"({
    "name": "cjson",
    "version": 1,
    "ratio": 0.75,
    "stable": true,
    "notes": null,
    "authors": ["dls"],
    "targets": [
      { "arch": "amd64", "simd": "avx2",  "tier": 1 },
      { "arch": "arm64", "simd": "neon",  "tier": 2 },
      { "arch": "armv7", "simd": "neon",  "tier": 2 }
    ],
    "dup": 1,
    "dup": 2
  })";
  constexpr usize k_len = sizeof(k_src) - 1;

  auto r = cjson::parse(k_src, k_len);
  if ( r.is_second() ) return 1;
  const cjson::doc &d = r.cast<cjson::doc>();
  const cjson::val root = d.root();

  // kinds
  ex::head("kinds");

  mc::echo("root      -> ", root.type_name());
  mc::echo("name      -> ", root["name"].type_name());
  mc::echo("version   -> ", root["version"].type_name());
  mc::echo("notes     -> ", root["notes"].type_name());
  mc::echo("targets   -> ", root["targets"].type_name());
  mc::echo("absent    -> ", root["absent"].type_name());

  // size() means different things by kind: elements for arrays, pairs for objects, bytes for strings
  mc::echo("targets.size() = ", root["targets"].size(), " (elements)");
  mc::echo("root.size()    = ", root.size(), " (pairs)");
  mc::echo("name.size()    = ", root["name"].size(), " (bytes)");

  // lossy vs checked getters
  ex::head("getters");

  // *_or never fails
  mc::echo("version i64_or   = ", root["version"].i64_or(-1));
  mc::echo("name    i64_or   = ", root["name"].i64_or(-1), "   <- mismatch, silently -1");
  mc::echo("absent  i64_or   = ", root["absent"].i64_or(-1), "   <- miss, also -1");

  // try_* distinguishes them
  auto a = root["name"].try_i64();
  mc::echo("name    try_i64  -> ", a.is_first() ? "ok" : cjson::error_name(a.cast<cjson::error>()));
  auto b = root["version"].try_i64();
  mc::echo("version try_i64  -> ", b.is_first() ? "ok" : cjson::error_name(b.cast<cjson::error>()));

  // numbers keep their subtype
  mc::echo("ratio   f64_or   = ", root["ratio"].f64_or(0));
  mc::echo("version f64_or   = ", root["version"].f64_or(0), " (int widened)");

  // iteration
  // items() yields val; members() yields member { strv key; val v; }
  ex::head("iteration");

  for ( auto t : root["targets"].items() ) mc::echo("  arch: ", t["arch"].string_or());

  mc::echo("root keys:");
  for ( auto m : root.members() ) ex::show("  ", m.key);

  // json pointer
  // rfc 6901: ~1 means '/', ~0 means '~'
  ex::head("json pointer");

  mc::echo("/targets/1/simd = ", root.at_pointer("/targets/1/simd").string_or());
  mc::echo("/targets/2/tier = ", root.at_pointer("/targets/2/tier").i64_or(-1));
  mc::echo("/no/such/path   -> ", root.at_pointer("/no/such/path") ? "found" : "miss");
  mc::echo("empty pointer names the root: ", root.at_pointer("").size(), " pairs");

  // duplicate keys
  ex::head("duplicate keys");
  mc::echo("dup -> ", root["dup"].i64_or(-1), " (first wins; both are stored)");

  // lifetime
  ex::head("lifetime");

  {
    cjson::scratch sc;
    auto owned = cjson::parse(k_src, k_len, {}, sc);
    auto borrowed = cjson::parse_reuse(k_src, k_len, {}, sc);
    mc::echo("parse()       borrowed? ", owned.cast<cjson::doc>().borrowed() ? "yes" : "no");
    mc::echo("parse_reuse() borrowed? ", borrowed.cast<cjson::doc>().borrowed() ? "yes" : "no");
    // reading `borrowed` after another parse on sc would be a use-after-free
  }

  mc::echo("");
  return 0;
}
