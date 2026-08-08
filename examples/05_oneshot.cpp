// 05_oneshot.cpp
// convenience fn's: text in, answer out
//
// See also:
//   examples/03_ondemand.cpp — what these are built on
//   examples/02_dom.cpp      — when you want the whole document instead
//
// The rest of the library trades convenience for control: you own a scratch, you own a
// doc, and you track which borrows from which. These do not. Give one a document and a
// json pointer and it gives you the value, having freed everything it allocated.

#include "_ex_common.hpp"

#include <micron/string/strings.hpp>

#include <micron/std.hpp>

int
main()
{
  constexpr const char k_cfg[] = R"({
    "listen": { "host": "0.0.0.0", "port": 8080, "tls": false },
    "workers": 4,
    "ratio": 0.25,
    "tags": ["a", "b", "c"],
    "name": "café",
    "nested": { "a/b": 1, "c~d": 2 },
    "nothing": null
  })";

  // get<T>: typed, with a failure reason
  // T is one of i64, u64, f64, bool, micron::string.
  ex::head("get");

  auto port = cjson::get<i64>(k_cfg, "/listen/port");
  if ( port.is_first() ) mc::echo("port    = ", port.cast<i64>());

  mc::echo("workers = ", cjson::get<u64>(k_cfg, "/workers").cast<u64>());
  mc::echo("ratio   = ", cjson::get<f64>(k_cfg, "/ratio").cast<f64>());
  mc::echo("tls     = ", cjson::get<bool>(k_cfg, "/listen/tls").cast<bool>());

  // also possible to marshall through any/pun if you don't know/care about explicit typing
  mc::echo("workers = ", cjson::pun(cjson::get(k_cfg, "/workers")));
  mc::echo("ratio   = ", cjson::pun(cjson::get(k_cfg, "/ratio")));
  mc::echo("tls     = ", cjson::pun(cjson::get(k_cfg, "/listen/tls")));
  // (we have to cast to pun so overload resolution wins for echo)

  auto name = cjson::get<micron::string>(k_cfg, "/name");
  mc::echo("name    = ", name.cast<micron::string>().c_str(), " (", name.cast<micron::string>().size(), " bytes, \\u00e9 decoded)");

  // misses and mismatches stay distinguishable
  ex::head("errors");

  auto miss = cjson::get<i64>(k_cfg, "/absent");
  mc::echo("/absent      -> ", cjson::error_name(miss.cast<cjson::error>()));

  auto wrong = cjson::get<i64>(k_cfg, "/listen/host");
  mc::echo("/listen/host -> ", cjson::error_name(wrong.cast<cjson::error>()), " (present, wrong kind)");

  auto nul = cjson::get<i64>(k_cfg, "/nothing");
  mc::echo("/nothing     -> ", cjson::error_name(nul.cast<cjson::error>()), " (null is a value, not a miss)");

  // get_or collapses all three
  ex::head("get_or");
  mc::echo("good    = ", cjson::get_or<i64>(k_cfg, "/listen/port", i64(-1)));
  mc::echo("miss    = ", cjson::get_or<i64>(k_cfg, "/absent", i64(-1)));
  mc::echo("wrong   = ", cjson::get_or<i64>(k_cfg, "/listen/host", i64(-1)));
  mc::echo("garbage = ", cjson::get_or<i64>("{not json", "/a", i64(-1)));

  // shape, without extracting
  ex::head("shape");

  mc::echo("exists /listen/port : ", cjson::exists(k_cfg, "/listen/port") ? "yes" : "no");
  mc::echo("exists /nothing     : ", cjson::exists(k_cfg, "/nothing") ? "yes" : "no (it is null, but present)");
  mc::echo("exists /nope        : ", cjson::exists(k_cfg, "/nope") ? "yes" : "no");
  mc::echo("count  /tags        = ", cjson::count_at(k_cfg, "/tags").cast<usize>());
  mc::echo("count  /listen      = ", cjson::count_at(k_cfg, "/listen").cast<usize>(), " (pairs)");

  // an empty pointer names the root
  mc::echo("count  \"\"           = ", cjson::count_at(k_cfg, "").cast<usize>());

  // escaped pointer tokens: ~1 is '/', ~0 is '~'
  mc::echo("/nested/a~1b        = ", cjson::get<i64>(k_cfg, "/nested/a~1b").cast<i64>());
  mc::echo("/nested/c~0d        = ", cjson::get<i64>(k_cfg, "/nested/c~0d").cast<i64>());

  // each
  ex::head("each");

  cjson::each(k_cfg, "/tags", [](cjson::cur c) { ex::show("  tag: ", c.str_raw()); });

  // over an object, a callable taking cur_member sees the keys
  cjson::each(k_cfg, "/listen", [](cjson::cur_member m) { ex::show("  key: ", m.key); });

  // whole-document reshaping
  ex::head("reshape");

  mc::echo("valid: ", cjson::valid(k_cfg) ? "yes" : "no");

  auto flat = cjson::compact(k_cfg);
  mc::echo("compact -> ", flat.cast<micron::string>().size(), " bytes");

  auto nice = cjson::pretty("{\"a\":[1,2]}");
  mc::echo("pretty:");
  mc::echo(nice.cast<micron::string>().c_str());

  auto wide = cjson::reformat("{\"a\":1}", cjson::style{ .indent = 4 });
  mc::echo("reformat(indent 4):");
  mc::echo(wide.cast<micron::string>().c_str());

  ex::head("with a scratch");

  cjson::scratch sc;
  for ( u32 i = 0; i < 3; i++ ) mc::echo("  read ", i, ": port = ", cjson::get<i64>(k_cfg, "/listen/port", sc).cast<i64>());

  // and with a scratch you can borrow a string instead of copying it
  auto raw = cjson::get_str_raw(k_cfg, "/name", sc);
  if ( raw.is_first() ) ex::show("borrowed raw name = ", raw.cast<cjson::strv>());

  mc::echo("");
  return 0;
}
