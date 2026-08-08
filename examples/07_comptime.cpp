// 07_comptime.cpp
// cjson::ct; parsing json with the compiler
//
// See also:
//   examples/01_quickstart.cpp — the runtime API
//   ARCHITECTURE.md            — "The constexpr contract"

#include "_ex_common.hpp"

#include <micron/std.hpp>

// validate at compile time
static constexpr cjson::ct::str k_good{ R"({"port":8080,"hosts":["a.example","b.example"]})" };
static constexpr cjson::ct::str k_bad{ R"({"port":})" };

static_assert(cjson::ct::validate<k_good>());
static_assert(!cjson::ct::validate<k_bad>());

// parse at compile time
// ct::parse yields a tree<NV, NS>: two flat arrays (values + text pool) sized by a
// counting pass

constexpr auto k_tree = cjson::ct::parse<k_good>();

static_assert(k_tree.root()["port"].i64_or(0) == 8080);
static_assert(k_tree.root()["hosts"].size() == 2);
static_assert(k_tree.root()["hosts"][usize(0)].str_is("a.example"));
static_assert(k_tree.root()["hosts"][usize(1)].str_is("b.example"));
static_assert(!k_tree.root()["absent"]);

static constexpr cjson::ct::str k_utf{ R"({"name":"café"})" };
constexpr auto k_utf_tree = cjson::ct::parse<k_utf>();
static_assert(k_utf_tree.root()["name"].str_len() == 5);
static_assert(k_utf_tree.root()["name"].str_at(3) == 0xc3);
static_assert(k_utf_tree.root()["name"].str_at(4) == 0xa9);

// minify at compile time
static constexpr cjson::ct::str k_spaced{ R"( { "s" : [ 1 , 2 ] } )" };
constexpr auto k_min = cjson::ct::minify<k_spaced>();

static_assert(k_min.len == 11);
static_assert(k_min.data[0] == '{');
static_assert(k_min.data[1] == '"');

// serialize at compile time
constexpr auto k_out = cjson::ct::write<k_tree>();
static_assert(k_out.len > 0);
static_assert(k_out.data[0] == '{');

// and pretty-printing is just a style
constexpr auto k_out_pretty = cjson::ct::write<k_tree, cjson::style{ .indent = 2 }>();
static_assert(k_out_pretty.len > k_out.len);

int
main()
{
  ex::head("comptime");

  mc::echo("baked port  = ", k_tree.root()["port"].i64_or(0), "   (computed by the compiler)");
  mc::echo("baked hosts = ", k_tree.root()["hosts"].size());

  ex::head("minify, computed at build time");
  {
    micron::string s{};
    s.append(reinterpret_cast<const char *>(k_min.data), k_min.len);
    mc::echo(s.c_str());
  }

  ex::head("serialize, computed at build time");
  {
    micron::string s{};
    s.append(reinterpret_cast<const char *>(k_out.data), k_out.len);
    mc::echo(s.c_str());
  }

  ex::head("runtime agrees with comptime");
  {
    // re-parse the same text at runtime and compare the answers
    auto r = cjson::parse(reinterpret_cast<const char *>(k_good.data), k_good.len);
    const bool same = r.is_first() && r.cast<cjson::doc>().root()["port"].i64_or(0) == k_tree.root()["port"].i64_or(0);
    mc::echo("port matches: ", same ? "yes" : "no");

    // and so does the writer
    auto rt = cjson::minify_str(reinterpret_cast<const char *>(k_spaced.data), k_spaced.len);
    const bool msame = rt.is_first() && rt.cast<micron::string>().size() == k_min.len;
    mc::echo("minify length matches: ", msame ? "yes" : "no");
  }

  mc::echo("");
  mc::echo("(every static_assert above was checked at build time — this binary only prints)");
  return 0;
}
