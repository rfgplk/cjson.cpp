//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include "../src/cjson/cjson.hpp"

#include <snowball/snowball.hpp>

#include <micron/string/sstring.hpp>
#include <micron/string/strings.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

// the text seam: every public entry point accepts raw pointers, strv, byte containers
// and any micron::is_string. the hazard this suite pins is AMBIGUITY — micron strings
// satisfy byte_source AND is_string, so the overload set must stay single-valued.

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// concept shape

static_assert(cjson::byte_source<micron::string>);
static_assert(micron::is_string<micron::string>);
static_assert(cjson::text_source<micron::string>);

static_assert(cjson::text_source<micron::vector<u8>>);      // container, not a string
static_assert(!micron::is_string<micron::vector<u8>>);

static_assert(cjson::text_source<micron::sstr<64>>);      // stack string
static_assert(micron::is_string<micron::sstr<64>>);

// the view currency stays OUT of the container/text overload set, so raw_slice never
// competes with the templates (ARCHITECTURE.md, "The micron seam")
static_assert(!cjson::text_source<cjson::bytes>);
static_assert(!cjson::text_source<cjson::wbytes>);
static_assert(!cjson::text_source<cjson::strv>);

// raw char pointers are not containers and never were
static_assert(!cjson::text_source<const char *>);

namespace
{

constexpr const char k_doc[] = R"({"port":8080,"name":"widget","tags":["a","b"],"on":true})";
constexpr usize k_len = sizeof(k_doc) - 1;

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

micron::string
mstr()
{
  micron::string s{};
  s.append(k_doc, k_len);
  return s;
}

micron::sstr<128>
sstr()
{
  micron::sstr<128> s{};
  s.try_append(k_doc, k_len);
  return s;
}

micron::vector<u8>
vbytes()
{
  micron::vector<u8> v;
  v.reserve(k_len + 1);
  for ( usize i = 0; i < k_len; i++ ) v.push_back(u8(k_doc[i]));
  return v;
}

// as_strv normalises every text flavour to {ptr,len} — the rgx::as_subject shape
static_assert(cjson::as_strv("abc").len == 3);

};      // namespace

int
main()
{
  {
    sb::test_case("parse accepts pointers, strv, byte containers and micron strings alike");
    const micron::string ms = mstr();
    const micron::sstr<128> ss = sstr();
    const micron::vector<u8> vb = vbytes();
    const cjson::strv sv{ k_doc, k_len };

    auto a = cjson::parse(k_doc, k_len);
    auto b = cjson::parse(reinterpret_cast<const u8 *>(k_doc), k_len);
    auto c = cjson::parse(sv);
    auto d = cjson::parse(ms);
    auto e = cjson::parse(ss);
    auto f = cjson::parse(vb);

    sb::require_true(a.is_first() and b.is_first() and c.is_first());
    sb::require_true(d.is_first() and e.is_first() and f.is_first());

    // every route must land on the same document
    sb::require(a.cast<cjson::doc>().root()["port"].i64_or(0), i64(8080));
    sb::require(c.cast<cjson::doc>().root()["port"].i64_or(0), i64(8080));
    sb::require(d.cast<cjson::doc>().root()["port"].i64_or(0), i64(8080));
    sb::require(e.cast<cjson::doc>().root()["port"].i64_or(0), i64(8080));
    sb::require(f.cast<cjson::doc>().root()["port"].i64_or(0), i64(8080));
    sb::end_test_case();
  }
  {
    sb::test_case("the scratch-taking parse overloads accept the same text flavours");
    const micron::string ms = mstr();
    const cjson::strv sv{ k_doc, k_len };
    cjson::scratch sc;

    sb::require_true(cjson::parse(k_doc, k_len, {}, sc).is_first());
    sb::require_true(cjson::parse(reinterpret_cast<const u8 *>(k_doc), k_len, {}, sc).is_first());
    sb::require_true(cjson::parse(sv, {}, sc).is_first());
    sb::require_true(cjson::parse(ms, {}, sc).is_first());
    sb::end_test_case();
  }
  {
    sb::test_case("parse_reuse borrows off a warm scratch through every text flavour");
    const micron::string ms = mstr();
    const cjson::strv sv{ k_doc, k_len };
    cjson::scratch sc;

    {
      auto r = cjson::parse_reuse(ms, {}, sc);
      sb::require_true(r.is_first());
      sb::require_true(r.cast<cjson::doc>().borrowed());
      sb::require(r.cast<cjson::doc>().root()["name"].str_or().len, usize(6));
    }
    {
      auto r = cjson::parse_reuse(sv, {}, sc);
      sb::require_true(r.is_first());
      sb::require_true(same(r.cast<cjson::doc>().root()["name"].str_or(), "widget"));
    }
    sb::end_test_case();
  }
  {
    sb::test_case("validate and is_valid span the same overload set");
    const micron::string ms = mstr();
    const micron::sstr<128> ss = sstr();
    const micron::vector<u8> vb = vbytes();
    const cjson::strv sv{ k_doc, k_len };
    cjson::scratch sc;

    sb::require_true(cjson::validate(k_doc, k_len) == cjson::error::ok);
    sb::require_true(cjson::validate(k_doc, k_len, {}, sc) == cjson::error::ok);
    sb::require_true(cjson::validate(sv) == cjson::error::ok);
    sb::require_true(cjson::validate(sv, {}, sc) == cjson::error::ok);
    sb::require_true(cjson::validate(ms) == cjson::error::ok);
    sb::require_true(cjson::validate(ms, {}, sc) == cjson::error::ok);
    sb::require_true(cjson::validate(ss) == cjson::error::ok);
    sb::require_true(cjson::validate(vb) == cjson::error::ok);

    sb::require_true(cjson::is_valid(k_doc, k_len));
    sb::require_true(cjson::is_valid(reinterpret_cast<const u8 *>(k_doc), k_len));
    sb::require_true(cjson::is_valid(sv));
    sb::require_true(cjson::is_valid(ms));
    sb::require_true(cjson::is_valid(ss));
    sb::require_true(cjson::is_valid(vb));

    // and a reject still rejects through every route
    micron::string bad{};
    bad.append("{\"a\":", 5);
    sb::require_true(cjson::validate(bad) != cjson::error::ok);
    sb::require_true(!cjson::is_valid(bad));
    sb::end_test_case();
  }
  {
    sb::test_case("iterate accepts every text flavour and defaults its opts");
    const micron::string ms = mstr();
    const cjson::strv sv{ k_doc, k_len };
    cjson::scratch sc;

    {
      auto r = cjson::iterate(ms, {}, sc);
      sb::require_true(r.is_first());
      sb::require_true(r.cast<cjson::view>().root()["port"].i64_or(0) == 8080);
    }
    {
      // opts-defaulted forms — iterate used to demand an explicit {}
      auto r = cjson::iterate(sv, sc);
      sb::require_true(r.is_first());
      sb::require_true(r.cast<cjson::view>().root()["on"].bool_or(false));
    }
    {
      auto r = cjson::iterate(k_doc, k_len, sc);
      sb::require_true(r.is_first());
      sb::require_true(same(r.cast<cjson::view>().root()["name"].str_raw(), "widget"));
    }
    {
      auto r = cjson::iterate(reinterpret_cast<const u8 *>(k_doc), k_len, sc);
      sb::require_true(r.is_first());
    }
    sb::end_test_case();
  }
  {
    sb::test_case("minify and minify_str reach the text flavours they never had");
    constexpr const char pretty[] = "{ \"a\" : [ 1 , 2 ] }";
    micron::string mp{};
    mp.append(pretty, sizeof(pretty) - 1);
    const cjson::strv sp{ pretty, sizeof(pretty) - 1 };

    {
      auto r = cjson::minify(mp);
      sb::require_true(r.is_first());
      sb::require(r.cast<cjson::fjson>().size(), usize(11));
    }
    {
      auto r = cjson::minify_str(sp);
      sb::require_true(r.is_first());
      sb::require(r.cast<micron::string>().size(), usize(11));
    }
    {
      auto r = cjson::minify_str(mp);
      sb::require_true(r.is_first());
      sb::require_true(same(cjson::strv{ r.cast<micron::string>().c_str(), r.cast<micron::string>().size() }, "{\"a\":[1,2]}"));
    }
    {
      u8 out[64];
      const max_t w = cjson::minify(mp, cjson::wbytes{ out, sizeof(out) });
      sb::require_true(w > 0);
      sb::require(usize(w), usize(11));
    }
    {
      u8 out[64];
      const max_t w = cjson::minify_into(pretty, sizeof(pretty) - 1, out, sizeof(out));
      sb::require(usize(w), usize(11));
    }
    sb::end_test_case();
  }
  {
    sb::test_case("insitu binds on mutable containers, never on const ones");
    micron::vector<u8> v = vbytes();
    v.reserve(k_len + cjson::padding + 8);
    auto r = cjson::parse_insitu(v);
    sb::require_true(r.is_first());
    sb::require(r.cast<cjson::doc>().root()["port"].i64_or(0), i64(8080));
    sb::end_test_case();
  }
  {
    sb::test_case("key lookup and json pointers take micron strings");
    const micron::string ms = mstr();
    auto r = cjson::parse(ms);
    sb::require_true(r.is_first());
    const cjson::doc &d = r.cast<cjson::doc>();

    micron::string kport{};
    kport.append("port", 4);
    micron::sstr<32> kname{};
    kname.try_append("name", 4);

    sb::require(d.root()[kport].i64_or(0), i64(8080));
    sb::require_true(same(d.root()[kname].str_or(), "widget"));

    micron::string ptr{};
    ptr.append("/tags/1", 7);
    sb::require_true(same(d.root().at_pointer(ptr).str_or(), "b"));
    sb::require_true(same(d.root().at_pointer("/tags/0").str_or(), "a"));      // const char* form
    sb::end_test_case();
  }
  {
    sb::test_case("val::at indexes arrays without the usize cast operator[] demands");
    auto r = cjson::parse(k_doc, k_len);
    sb::require_true(r.is_first());
    auto tags = r.cast<cjson::doc>().root()["tags"];
    sb::require_true(same(tags.at(0).str_or(), "a"));
    sb::require_true(same(tags.at(1).str_or(), "b"));
    sb::require_true(!tags.at(2));
    // at() and operator[](usize) are the same walk
    sb::require_true(tags.at(0).__raw() == tags[usize(0)].__raw());
    sb::end_test_case();
  }
  {
    sb::test_case("on-demand key lookup takes micron strings too");
    const micron::string ms = mstr();
    cjson::scratch sc;
    auto r = cjson::iterate(ms, sc);
    sb::require_true(r.is_first());
    auto root = r.cast<cjson::view>().root();

    micron::string kport{};
    kport.append("port", 4);
    sb::require_true(root[kport].i64_or(0) == 8080);
    sb::end_test_case();
  }
  {
    sb::test_case("builder composes from micron strings — kv(key, string) now compiles");
    micron::string name{};
    name.append("widget", 6);
    micron::sstr<32> key{};
    key.try_append("name", 4);

    cjson::builder b;
    b.obj().kv(key, name).kv("id", u64(7)).key(key).value(name).end();
    sb::require_true(b.err() == cjson::error::ok);
    sb::require_true(same(b.out(), R"({"name":"widget","id":7,"name":"widget"})"));

    // and the composed text parses back
    auto r = cjson::parse(b.out());
    sb::require_true(r.is_first());
    sb::require_true(same(r.cast<cjson::doc>().root()["name"].str_or(), "widget"));
    sb::end_test_case();
  }
  {
    sb::test_case("escaping still applies to string-typed keys and values");
    micron::string tricky{};
    tricky.append("a\"b\\c\nd", 7);
    cjson::builder b;
    b.obj().kv(tricky, tricky).end();
    sb::require_true(b.err() == cjson::error::ok);
    auto r = cjson::parse(b.out());
    sb::require_true(r.is_first());
    sb::require(r.cast<cjson::doc>().root()["a\"b\\c\nd"].str_or().len, usize(7));
    sb::end_test_case();
  }
  return 1;
}
