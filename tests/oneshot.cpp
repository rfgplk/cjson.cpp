//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include "../src/cjson/cjson.hpp"

#include <snowball/snowball.hpp>

#include <micron/string/strings.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

constexpr const char k_cfg[] = R"({
  "listen": { "host": "0.0.0.0", "port": 8080, "tls": false },
  "workers": 4,
  "ratio": 0.25,
  "tags": ["a","b","c"],
  "name": "café",
  "esc": "a\nb",
  "nested": { "a/b": 1, "c~d": 2 },
  "nothing": null
})";
constexpr usize k_cfg_len = sizeof(k_cfg) - 1;

bool
same(cjson::strv a, const char *b)
{
  usize n = 0;
  while ( b[n] ) ++n;
  if ( a.len != n ) return false;
  for ( usize i = 0; i < n; i++ )
    if ( a.ptr[i] != b[i] ) return false;
  return true;
}

bool
seq(const micron::string &s, const char *b)
{
  return same(cjson::strv{ s.c_str(), s.size() }, b);
}

};      // namespace

int
main()
{
  {
    sb::test_case("get pulls typed scalars through a json pointer");
    auto port = cjson::get<i64>(k_cfg, "/listen/port");
    sb::require_true(port.is_first());
    sb::require_true(port.cast<i64>() == 8080);

    sb::require_true(cjson::get<u64>(k_cfg, "/workers").cast<u64>() == 4u);
    sb::require_true(cjson::get<f64>(k_cfg, "/ratio").cast<f64>() == 0.25);
    sb::require_true(cjson::get<bool>(k_cfg, "/listen/tls").cast<bool>() == false);

    auto host = cjson::get<micron::string>(k_cfg, "/listen/host");
    sb::require_true(host.is_first());
    sb::require_true(seq(host.cast<micron::string>(), "0.0.0.0"));
    sb::end_test_case();
  }
  {
    sb::test_case("array indices and escaped pointer tokens resolve");
    sb::require_true(seq(cjson::get<micron::string>(k_cfg, "/tags/1").cast<micron::string>(), "b"));

    sb::require_true(cjson::get<i64>(k_cfg, "/nested/a~1b").cast<i64>() == 1);
    sb::require_true(cjson::get<i64>(k_cfg, "/nested/c~0d").cast<i64>() == 2);
    sb::end_test_case();
  }
  {
    sb::test_case("returned strings are owning, escape-decoded copies");
    auto name = cjson::get<micron::string>(k_cfg, "/name");
    sb::require_true(name.is_first());
    const micron::string &s = name.cast<micron::string>();

    sb::require(s.size(), usize(5));
    sb::require_true(u8(s.c_str()[3]) == 0xc3 and u8(s.c_str()[4]) == 0xa9);
    sb::end_test_case();
  }
  {
    sb::test_case("a miss and a type mismatch stay distinguishable");
    auto miss = cjson::get<i64>(k_cfg, "/absent");
    sb::require_true(miss.is_second());
    sb::require_true(miss.cast<cjson::error>() == cjson::error::no_such_field);

    auto deep_miss = cjson::get<i64>(k_cfg, "/listen/absent/deeper");
    sb::require_true(deep_miss.cast<cjson::error>() == cjson::error::no_such_field);

    auto wrong = cjson::get<i64>(k_cfg, "/listen/host");
    sb::require_true(wrong.is_second());
    sb::require_true(wrong.cast<cjson::error>() == cjson::error::wrong_type);

    auto nul = cjson::get<i64>(k_cfg, "/nothing");
    sb::require_true(nul.cast<cjson::error>() == cjson::error::wrong_type);

    sb::require_true(cjson::get<i64>("{\"a\":", "/a").is_second());
    sb::end_test_case();
  }
  {
    sb::test_case("get_or collapses miss, mismatch and malformed to the default");
    sb::require_true(cjson::get_or<i64>(k_cfg, "/listen/port", i64(-1)) == 8080);
    sb::require_true(cjson::get_or<i64>(k_cfg, "/absent", i64(-1)) == -1);
    sb::require_true(cjson::get_or<i64>(k_cfg, "/listen/host", i64(-1)) == -1);
    sb::require_true(cjson::get_or<i64>("nonsense{", "/a", i64(-1)) == -1);
    sb::require_true(cjson::get_or<bool>(k_cfg, "/listen/tls", true) == false);
    sb::end_test_case();
  }
  {
    sb::test_case("the empty pointer names the root");
    sb::require_true(cjson::kind_at(k_cfg, "") == cjson::kind::object);
    sb::require_true(cjson::exists(k_cfg, ""));
    sb::require_true(cjson::count_at(k_cfg, "").cast<usize>() == 8u);
    sb::end_test_case();
  }
  {
    sb::test_case("exists, kind_at and count_at describe shape without extracting");
    sb::require_true(cjson::exists(k_cfg, "/listen/port"));
    sb::require_true(cjson::exists(k_cfg, "/nothing"));
    sb::require_true(!cjson::exists(k_cfg, "/nope"));
    sb::require_true(!cjson::exists(k_cfg, "/tags/9"));

    sb::require_true(cjson::kind_at(k_cfg, "/tags") == cjson::kind::array);
    sb::require_true(cjson::kind_at(k_cfg, "/listen") == cjson::kind::object);
    sb::require_true(cjson::kind_at(k_cfg, "/name") == cjson::kind::string);
    sb::require_true(cjson::kind_at(k_cfg, "/nothing") == cjson::kind::null);
    sb::require_true(cjson::kind_at(k_cfg, "/nope") == cjson::kind::none);

    sb::require_true(cjson::count_at(k_cfg, "/tags").cast<usize>() == 3u);
    sb::require_true(cjson::count_at(k_cfg, "/listen").cast<usize>() == 3u);
    sb::require_true(cjson::count_at(k_cfg, "/workers").is_second());
    sb::require_true(cjson::count_at(k_cfg, "/nope").cast<cjson::error>() == cjson::error::no_such_field);
    sb::end_test_case();
  }
  {
    sb::test_case("each streams array elements and object members");
    usize n = 0;
    usize bytes = 0;
    sb::require_true(cjson::each(k_cfg, "/tags",
                                 [&](cjson::cur c) {
                                   ++n;
                                   bytes += c.str_raw().len;
                                 })
                     == cjson::error::ok);
    sb::require(n, usize(3));
    sb::require(bytes, usize(3));

    usize keys = 0;
    sb::require_true(cjson::each(k_cfg, "/listen", [&](cjson::cur_member m) { keys += m.key.len; }) == cjson::error::ok);
    sb::require(keys, usize(4 + 4 + 3));

    usize vals = 0;
    sb::require_true(cjson::each(k_cfg, "/listen", [&](cjson::cur) { ++vals; }) == cjson::error::ok);
    sb::require(vals, usize(3));

    sb::require_true(cjson::each(k_cfg, "/workers", [](cjson::cur) { }) == cjson::error::wrong_type);
    sb::require_true(cjson::each(k_cfg, "/nope", [](cjson::cur) { }) == cjson::error::no_such_field);
    sb::end_test_case();
  }
  {
    sb::test_case("valid, compact, pretty and reformat reshape whole documents");
    sb::require_true(cjson::valid(k_cfg));
    sb::require_true(!cjson::valid("{\"a\":}"));

    auto c = cjson::compact(k_cfg);
    sb::require_true(c.is_first());
    sb::require_true(c.cast<micron::string>().size() < k_cfg_len);
    sb::require_true(cjson::valid(c.cast<micron::string>()));

    auto p = cjson::pretty("{\"a\":[1,2]}");
    sb::require_true(p.is_first());
    sb::require_true(seq(p.cast<micron::string>(), "{\n  \"a\": [\n    1,\n    2\n  ]\n}"));

    auto p4 = cjson::reformat("{\"a\":1}", cjson::style{ .indent = 4 });
    sb::require_true(p4.is_first());
    sb::require_true(seq(p4.cast<micron::string>(), "{\n    \"a\": 1\n}"));

    auto flat = cjson::reformat(p.cast<micron::string>(), cjson::style{});
    sb::require_true(flat.is_first());
    sb::require_true(seq(flat.cast<micron::string>(), "{\"a\":[1,2]}"));

    sb::require_true(cjson::pretty("{oops").is_second());
    sb::end_test_case();
  }
  {
    sb::test_case("the scratch-taking twins agree and reuse the index");
    cjson::scratch sc;
    for ( u32 i = 0; i < 3; i++ ) {
      sb::require_true(cjson::get<i64>(k_cfg, "/listen/port", sc).cast<i64>() == 8080);
      sb::require_true(cjson::exists(k_cfg, "/tags", {}, sc));
      sb::require_true(cjson::kind_at(k_cfg, "/tags", {}, sc) == cjson::kind::array);
      sb::require_true(cjson::count_at(k_cfg, "/tags", {}, sc).cast<usize>() == 3u);
      sb::require_true(cjson::get_or<i64>(k_cfg, "/workers", i64(0), {}, sc) == 4);
    }
    sb::end_test_case();
  }
  {
    sb::test_case("get_str_raw borrows undecoded bytes off a caller scratch");
    cjson::scratch sc;

    auto s = cjson::get_str_raw(k_cfg, "/esc", sc);
    sb::require_true(s.is_first());
    sb::require(s.cast<cjson::strv>().len, usize(4));
    sb::require_true(same(s.cast<cjson::strv>(), "a\\nb"));
    sb::require(cjson::get<micron::string>(k_cfg, "/esc").cast<micron::string>().size(), usize(3));

    sb::require_true(same(cjson::get_str_raw(k_cfg, "/listen/host", sc).cast<cjson::strv>(), "0.0.0.0"));
    sb::require_true(cjson::get_str_raw(k_cfg, "/workers", sc).cast<cjson::error>() == cjson::error::wrong_type);
    sb::end_test_case();
  }
  {
    sb::test_case("every text and pointer flavour reaches the oneshots");
    micron::string doc{};
    doc.append(k_cfg, k_cfg_len);
    micron::string ptr{};
    ptr.append("/listen/port", 12);
    const cjson::strv sdoc{ k_cfg, k_cfg_len };
    const cjson::strv sptr{ "/listen/port", 12 };
    micron::vector<u8> vdoc;
    vdoc.reserve(k_cfg_len + 1);
    for ( usize i = 0; i < k_cfg_len; i++ ) vdoc.push_back(u8(k_cfg[i]));

    sb::require_true(cjson::get<i64>(doc, ptr).cast<i64>() == 8080);
    sb::require_true(cjson::get<i64>(doc, "/listen/port").cast<i64>() == 8080);
    sb::require_true(cjson::get<i64>(sdoc, sptr).cast<i64>() == 8080);
    sb::require_true(cjson::get<i64>(vdoc, ptr).cast<i64>() == 8080);
    sb::require_true(cjson::get<i64>(k_cfg, sptr).cast<i64>() == 8080);
    sb::require_true(cjson::get<i64>(cjson::bytes{ reinterpret_cast<const u8 *>(k_cfg), k_cfg_len }, "/listen/port").cast<i64>() == 8080);

    sb::require_true(cjson::valid(doc));
    sb::require_true(cjson::valid(sdoc));
    sb::require_true(cjson::valid(vdoc));
    sb::require_true(cjson::exists(doc, ptr));
    sb::end_test_case();
  }
  return 1;
}
