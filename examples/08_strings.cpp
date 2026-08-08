// 08_strings.cpp
// Feeding cjson: every entry point takes every kind of text you might be holding.

#include "_ex_common.hpp"

#include <micron/string/sstring.hpp>
#include <micron/string/strings.hpp>
#include <micron/vector.hpp>
#include <micron/std.hpp>

static_assert(cjson::byte_source<micron::string>);
static_assert(micron::is_string<micron::string>);
static_assert(cjson::text_source<micron::string>);

static_assert(cjson::text_source<micron::vector<u8>>);      // container, not a string
static_assert(cjson::text_source<micron::sstr<64>>);        // stack string

static_assert(!cjson::text_source<cjson::strv>);
static_assert(!cjson::text_source<cjson::bytes>);

int
main()
{
  constexpr const char k_src[] = R"({"port":8080,"name":"widget","tags":["a","b"]})";
  constexpr usize k_len = sizeof(k_src) - 1;

  // the same document, held six different ways
  micron::string ms{};
  ms.append(k_src, k_len);

  micron::sstr<128> ss{};
  ss.try_append(k_src, k_len);

  micron::vector<u8> vb;
  vb.reserve(k_len + 1);
  for ( usize i = 0; i < k_len; i++ ) vb.push_back(u8(k_src[i]));

  const cjson::strv sv{ k_src, k_len };
  const cjson::bytes bs{ reinterpret_cast<const u8 *>(k_src), k_len };

  // parse
  ex::head("parse");

  auto show = [](const char *what, auto &&r) {
    mc::echo("  ", what, " -> ", r.is_first() ? r.template cast<cjson::doc>().root()["port"].i64_or(0) : -1);
  };

  show("const char*, len ", cjson::parse(k_src, k_len));
  show("const u8*, len   ", cjson::parse(reinterpret_cast<const u8 *>(k_src), k_len));
  show("strv             ", cjson::parse(sv));
  show("bytes            ", cjson::parse(bs));
  show("micron::string   ", cjson::parse(ms));
  show("micron::sstr<128>", cjson::parse(ss));
  show("vector<u8>       ", cjson::parse(vb));

  // validate, iterate, minify
  ex::head("the rest of the api");

  mc::echo("validate(micron::string) : ", cjson::validate(ms) == cjson::error::ok ? "ok" : "bad");
  mc::echo("is_valid(vector<u8>)     : ", cjson::is_valid(vb) ? "yes" : "no");
  mc::echo("minify_str(sstr)         : ", cjson::minify_str(ss).cast<micron::string>().size(), " bytes");

  {
    cjson::scratch sc;
    auto rv = cjson::iterate(ms, sc);
    if ( rv.is_first() ) mc::echo("iterate(micron::string)  : port = ", rv.cast<cjson::view>().root()["port"].i64_or(0));
  }

  // keys and pointers
  ex::head("keys and pointers");

  auto r = cjson::parse(ms);
  const cjson::val root = r.cast<cjson::doc>().root();

  micron::string kname{};
  kname.append("name", 4);
  micron::sstr<32> kport{};
  kport.try_append("port", 4);
  micron::string ptr{};
  ptr.append("/tags/1", 7);

  ex::show("root[micron::string] = ", root[kname].str_or());
  mc::echo("root[sstr<32>]       = ", root[kport].i64_or(-1));
  ex::show("at_pointer(string)   = ", root.at_pointer(ptr).str_or());
  ex::show("at_pointer(literal)  = ", root.at_pointer("/tags/0").str_or());

  ex::head("builder");

  micron::string name{};
  name.append("ada", 3);

  cjson::builder b;
  b.obj().kv("name", name).kv(kname, name).key(kname).value(name).end();
  ex::show("built: ", b.out());

  // in situ
  ex::head("in situ");

  micron::vector<u8> mut = vb;
  mut.reserve(k_len + cjson::padding + 8);
  auto ri = cjson::parse_insitu(mut);
  mc::echo("parse_insitu(vector<u8>&) port = ", ri.is_first() ? ri.cast<cjson::doc>().root()["port"].i64_or(-1) : -1);

  mc::echo("");
  return 0;
}
