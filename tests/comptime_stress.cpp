//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// comptime stress tier — built ONLY by scripts/ctbuild (raised constexpr limits; duck
// cannot pass -fconstexpr-* flags). compiling this file proves multi-kilobyte comptime
// parse/minify round-trips and the czlib::ct interop (comptime json -> comptime gzip).
// without CJSON_CT_STRESS the tu degenerates to a trivial pass so `duck test tests/`
// stays green

#include "../src/cjson/cjson.hpp"

#ifdef CJSON_CT_STRESS

#include <czlib/czlib.hpp>

namespace
{

// a ~2kb config-like document, generated at compile time
consteval auto
gen_doc()
{
  struct out {
    char text[2200];
    usize len = 0;
  } o{};

  auto app = [&](const char *s) consteval {
    for ( usize i = 0; s[i]; ++i ) o.text[o.len++] = s[i];
  };
  app("{\"routes\":[");
  for ( u32 i = 0; i < 40; ++i ) {
    if ( i ) app(",");
    app("{\"path\":\"/api/resource/");
    char d[4] = { char('0' + i / 10), char('0' + i % 10), 0, 0 };
    app(d);
    app("\",\"limit\":");
    char l[8] = { char('0' + (i % 9) + 1), '0', '0', 0, 0, 0, 0, 0 };
    app(l);
    app("}");
  }
  app("],\"tls\":true,\"ratio\":0.125}");
  o.text[o.len] = 0;
  return o;
}

inline constexpr auto k_doc = gen_doc();

consteval auto
make_str()
{
  cjson::ct::str<2200> s{};
  for ( usize i = 0; i < k_doc.len; ++i ) s.data[i] = u8(k_doc.text[i]);
  s.len = k_doc.len;
  return s;
}

inline constexpr auto k_str = make_str();

static_assert(cjson::ct::validate<k_str>());

constexpr auto k_tree = cjson::ct::parse<k_str>();

static_assert(k_tree.root()["routes"].size() == 40);
static_assert(k_tree.root()["routes"][usize(7)]["limit"].i64_or(0) == 800);
static_assert(k_tree.root()["tls"].bool_or(false));

constexpr auto k_min = cjson::ct::minify<k_str>();

static_assert(k_min.size() == k_str.len);      // generator emits no strippable whitespace

// comptime json -> comptime gzip: cjson::ct types are czlib::ct-compatible
constexpr auto k_gz = czlib::ct::gzip<k_min>();

static_assert(k_gz.size() > 16 and k_gz.size() < k_min.size());

constexpr auto k_back = czlib::ct::gunzip<k_gz>();

static_assert(k_back.len == k_min.size());

};      // namespace

#endif      // CJSON_CT_STRESS

int
main()
{
  return 1;      // pass sentinel; the static_asserts above are the test
}
