// 01_quickstart.cpp
// See also:
//   examples/02_dom.cpp        — the owning document in depth
//   examples/03_ondemand.cpp   — the zero-materialization read path
//   examples/04_functional.cpp — pipelines over json values
//   examples/05_oneshot.cpp    — one-call field extraction
//
// cjson is header-only and micron-only. One include gets everything:
//
//   #include <cjson/cjson.hpp>
//
// Build (from the repo root):
//   duck batch examples.duck && ./bin/01_quickstart

// ex:: namespaced printing helper fns
#include "_ex_common.hpp"

#include <micron/std.hpp>

int
main()
{
  constexpr const char k_body[] = R"({
    "service": "billing",
    "port": 8080,
    "tls": false,
    "peers": ["10.0.0.1", "10.0.0.2"],
    "limits": { "rps": 2500, "burst": 400 }
  })";
  constexpr usize k_len = sizeof(k_body) - 1;

  // a) parse into an owning document
  // parse() returns micron::option<doc, error>
  ex::head("parse");

  auto r = cjson::parse(k_body, k_len);
  if ( r.is_second() ) {
    mc::echo("parse failed: ", cjson::error_name(r.cast<cjson::error>()));
    return 1;
  }
  cjson::doc &d = r.cast<cjson::doc>();
  mc::echo("value slots in the arena: ", d.size());

  // b) read fields
  // NOTE: navigation never crashes; indexing a missing key yields a dead cursor that keeps chaining
  ex::head("read");

  auto root = d.root();
  mc::echo("port        = ", root["port"].i64_or(0));
  mc::echo("tls         = ", root["tls"].bool_or(true));
  mc::echo("rps         = ", root["limits"]["rps"].i64_or(0));
  mc::echo("service     = ", root["service"].string_or());
  // if you don't want to deduce the type it will automatically convert to a micron::any
  // NOTE: we need to explicitly cast to pun so the operator overload wins for echo
  mc::echo("service     = ", cjson::pun(root["service"]));

  // at() indexes arrays
  mc::echo("peers[1]    = ", root["peers"].at(1).string_or());
  mc::echo("peer count  = ", root["peers"].size());

  // a miss chains safely instead of crashing, and reads as false
  mc::echo("missing     = ", root["nope"]["deeper"]["still"].i64_or(-1));
  mc::echo("missing is falsy: ", !root["nope"] ? "yes" : "no");

  // rfc 6901 json pointer
  mc::echo("/limits/burst = ", root.at_pointer("/limits/burst").i64_or(0));

  // c) edit the parsed doc in place, then export it
  // NOTE: indexing a non-const doc hands back a mutable proxy
  ex::head("mutate");

  d["service"] = "billing-v2";        // strings may grow: the pool is appended to
  d["limits"]["rps"] = 50;            // numbers, bools and null are rewritten in place
  d["tls"] = true;
  d.edit().insert("region") = "eu-west-1";      // add a member that was never there
  d["peers"].push_back() = "10.0.0.3";          // grow an array
  d.edit().erase("nope");                       // erasing a miss is reported, not fatal

  mc::echo("mut_error   = ", cjson::error_name(d.mut_error()));
  d.clear_mut_error();

  mc::echo("edited      = ", cjson::write_str(d));

  // d) build a response
  ex::head("build");

  cjson::builder b;
  b.obj()
      .kv("service", root["service"].str_or())
      .kv("accepted", true)
      .kv("port", root["port"].i64_or(0))
      .key("peers")
      .arr()
      .value("10.0.0.1")
      .value("10.0.0.2")
      .end()
      .end();

  if ( b.err() != cjson::error::ok )
    mc::echo("builder error: ", cjson::error_name(b.err()));
  else
    ex::show("built: ", b.out());

  // d) write a document
  // write() returns an owning micron::slice<u8>; write_into() fills a buffer
  // style{.indent} switches minified/pretty.
  ex::head("write");

  cjson::fjson minified = cjson::write(d);
  mc::echo("minified bytes: ", minified.size());

  cjson::fjson pretty = cjson::write(d, cjson::style{ .indent = 2 });
  mc::echo("pretty bytes:   ", pretty.size());

  // c) one-shot, when you just want one field
  ex::head("oneshot");

  mc::echo("get_or(/port)     = ", cjson::get_or<i64>(k_body, "/port", i64(-1)));
  mc::echo("exists(/limits)   = ", cjson::exists(k_body, "/limits") ? "yes" : "no");
  mc::echo("valid             = ", cjson::valid(k_body) ? "yes" : "no");

  // d) comptime: bake a config into the binary
  ex::head("comptime");

  static constexpr cjson::ct::str k_cfg{ R"({"port":8080,"name":"baked"})" };
  static_assert(cjson::ct::validate<k_cfg>());
  constexpr auto k_tree = cjson::ct::parse<k_cfg>();
  static_assert(k_tree.root()["port"].i64_or(0) == 8080);
  static_assert(k_tree.root()["name"].str_is("baked"));
  mc::echo("baked port (proven at compile time) = ", k_tree.root()["port"].i64_or(0));

  return 0;
}
