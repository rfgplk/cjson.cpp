//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// the constexpr contract, enforced: building this file IS most of the test — every
// static_assert below runs the full stage-1 sweep and grammar fsm inside constant
// evaluation. main() adds the runtime-equality half of the twin seam

#include "../src/cjson/cjson.hpp"

#include <snowball/snowball.hpp>

#include <micron/types.hpp>

namespace
{

template<usize N>
consteval cjson::error
V(const char (&s)[N])
{
  return cjson::validate(s, N - 1);
}

template<usize N>
cjson::error
VR(const char (&s)[N])
{
  return cjson::validate(s, N - 1);
}

// accepted
static_assert(V(R"({})") == cjson::error::ok);
static_assert(V(R"([])") == cjson::error::ok);
static_assert(V(R"(null)") == cjson::error::ok);
static_assert(V(R"(true)") == cjson::error::ok);
static_assert(V(R"(false)") == cjson::error::ok);
static_assert(V(R"("")") == cjson::error::ok);
static_assert(V(R"(0)") == cjson::error::ok);
static_assert(V(R"(-0)") == cjson::error::ok);
static_assert(V(R"(123)") == cjson::error::ok);
static_assert(V(R"(-1.5e+300)") == cjson::error::ok);
static_assert(V(R"(  {"a":1,"b":[true,false,null],"c":"x","d":{"e":[]}}  )") == cjson::error::ok);
static_assert(V(R"("escapes \" \\ \/ \b \f \n \r \t A")") == cjson::error::ok);
static_assert(V(R"("pair 😀 ok")") == cjson::error::ok);
static_assert(V("[1,2,3,[4,5,[6]],{\"k\":\"v\"}]") == cjson::error::ok);

// rejected, with the right diagnosis
static_assert(V(R"()") == cjson::error::empty_input);
static_assert(V(R"(   )") == cjson::error::empty_input);
static_assert(V(R"({)") == cjson::error::bad_syntax);
static_assert(V(R"(})") == cjson::error::bad_syntax);
static_assert(V(R"([1,])") == cjson::error::bad_syntax);
static_assert(V(R"({"a":})") == cjson::error::bad_syntax);
static_assert(V(R"({"a" 1})") == cjson::error::bad_syntax);
static_assert(V(R"({1:2})") == cjson::error::bad_syntax);
static_assert(V(R"([}])") == cjson::error::bad_syntax);
static_assert(V(R"({]})") == cjson::error::bad_syntax);
static_assert(V(R"(tru)") == cjson::error::bad_syntax);
static_assert(V(R"(truex)") == cjson::error::bad_syntax);
static_assert(V(R"(nul)") == cjson::error::bad_syntax);
static_assert(V(R"(01)") == cjson::error::bad_number);
static_assert(V(R"(+1)") == cjson::error::bad_syntax);
static_assert(V(R"(.5)") == cjson::error::bad_syntax);
static_assert(V(R"(1.)") == cjson::error::bad_number);
static_assert(V(R"(1e)") == cjson::error::bad_number);
static_assert(V(R"(1e+)") == cjson::error::bad_number);
static_assert(V(R"(-)") == cjson::error::bad_number);
static_assert(V(R"("unclosed)") == cjson::error::bad_string);
static_assert(V("\"tab\tinside\"") == cjson::error::bad_string);
static_assert(V(R"("bad \q escape")") == cjson::error::bad_escape);
static_assert(V(R"("bad \u00g0")") == cjson::error::bad_escape);
static_assert(V(R"("lone \ud800 high")") == cjson::error::bad_surrogate);
static_assert(V(R"("lone \udc00 low")") == cjson::error::bad_surrogate);
static_assert(V(R"("reversed \udc00\ud800")") == cjson::error::bad_surrogate);
static_assert(V(R"(1 2)") == cjson::error::trailing_garbage);
static_assert(V(R"({} extra)") == cjson::error::trailing_garbage);
static_assert(V(R"(\)") == cjson::error::bad_syntax);

// utf-8 rejects — this is the fused-checker seam: comptime runs the scalar decoder,
// runtime the simd lookup4; both must diagnose bad_utf8 (runtime mirrors below)
static_assert(V("\"\xc3\x28\"") == cjson::error::bad_utf8);
static_assert(V("\"\x80\"") == cjson::error::bad_utf8);
static_assert(V("\"\xed\xa0\x80\"") == cjson::error::bad_utf8);
static_assert(V("\"\xf4\x90\x80\x80\"") == cjson::error::bad_utf8);
static_assert(V("\"\xc0\xaf\"") == cjson::error::bad_utf8);
static_assert(V("\"trunc \xe2\x82") == cjson::error::bad_string);      // unclosed outranks utf-8

// options fold correctly under constant evaluation
static_assert(cjson::validate("1 2", 3, cjson::opts{ .stop_when_done = true }) == cjson::error::ok);

// depth guard fires inside constant evaluation
consteval cjson::error
deep(u32 n)
{
  u8 *buf = new u8[2 * n];
  for ( u32 i = 0; i < n; ++i ) buf[i] = u8('[');
  for ( u32 i = 0; i < n; ++i ) buf[n + i] = u8(']');
  const cjson::error e = cjson::validate(buf, 2 * n);
  delete[] buf;
  return e;
}

static_assert(deep(64) == cjson::error::ok);
// the innermost empty pair takes the []-shortcut and never pushes a scope (simdjson
// semantics), so limit+1 nests still pass; limit+2 is the first rejected depth
static_assert(deep(cjson::depth_limit + 1) == cjson::error::ok);
static_assert(deep(cjson::depth_limit + 2) == cjson::error::depth_exceeded);

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// the full dom pipeline inside constant evaluation: copy -> pad -> stage 1 -> arena
// build -> navigate -> extract, with every allocation transient

// comptime drives the constexpr max_t core directly: micron::option (and therefore the
// result<doc> wrappers) is runtime-only by design
template<usize N>
consteval i64
ct_i64(const char (&s)[N], const char *key)
{
  u8 *tmp = new u8[N - 1];
  for ( usize i = 0; i + 1 < N; ++i ) tmp[i] = u8(s[i]);
  i64 out = -777;
  {
    cjson::scratch sc{};
    cjson::doc d{};
    if ( cjson::__parse_into(d, cjson::bytes{ tmp, N - 1 }, {}, sc) > 0 ) {
      usize kl = 0;
      while ( key[kl] ) ++kl;
      out = d.root()[cjson::strv{ key, kl }].i64_or(-888);
    }
  }
  delete[] tmp;
  return out;
}

template<usize N>
consteval f64
ct_f64_idx(const char (&s)[N], usize idx)
{
  u8 *tmp = new u8[N - 1];
  for ( usize i = 0; i + 1 < N; ++i ) tmp[i] = u8(s[i]);
  f64 out = -1;
  {
    cjson::scratch sc{};
    cjson::doc d{};
    if ( cjson::__parse_into(d, cjson::bytes{ tmp, N - 1 }, {}, sc) > 0 ) out = d.root()[idx].f64_or(-2);
  }
  delete[] tmp;
  return out;
}

static_assert(ct_i64(R"({"port":8080,"nested":{"x":1}})", "port") == 8080);
static_assert(ct_i64(R"({"a":-42})", "a") == -42);
static_assert(ct_i64(R"({"a":1})", "missing") == -888);
static_assert(ct_f64_idx(R"([1.5,2.5e10,0.125])", 1) == 2.5e10);
static_assert(ct_f64_idx(R"([1e23])", 0) == 1e23);

// the reuse twin: the scratch owns the pool and the slab, the doc borrows both. under
// constant evaluation a borrowed doc that still ran delete[] in release() would be a
// double delete — which is a hard non-constant expression, so these static_asserts are
// the compile-time proof that the __borrowed gate holds. two parses on one scratch also
// prove the retained buffers survive being reused
template<usize N>
consteval i64
ct_i64_reuse(const char (&s)[N], const char *key)
{
  u8 *tmp = new u8[N - 1];
  for ( usize i = 0; i + 1 < N; ++i ) tmp[i] = u8(s[i]);
  i64 out = -777;
  {
    cjson::scratch sc{};
    usize kl = 0;
    while ( key[kl] ) ++kl;
    for ( u32 pass = 0; pass < 2; ++pass ) {
      cjson::doc d{};
      if ( cjson::__parse_reuse_into(d, cjson::bytes{ tmp, N - 1 }, {}, sc) > 0 ) {
        if ( !d.borrowed() ) return -999;
        out = d.root()[cjson::strv{ key, kl }].i64_or(-888);
      }
    }
  }
  delete[] tmp;
  return out;
}

static_assert(ct_i64_reuse(R"({"port":8080,"nested":{"x":1}})", "port") == 8080);
static_assert(ct_i64_reuse(R"({"a":-42})", "a") == -42);
static_assert(ct_i64_reuse(R"({"a":1})", "missing") == -888);

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// the ct baking layer: parse/minify/write at compile time into .rodata; an ox-shaped
// config extracted through the baked cursor

constexpr cjson::ct::str k_cfg{
  R"({"listen":{"port":8080,"backlog":512},"tls":true,"hosts":["a.example","b.example"],"limits":{"body":1048576,"ratio":0.25},"name":"café"})"
};

static_assert(cjson::ct::validate<k_cfg>());
static_assert(!cjson::ct::validate<cjson::ct::str{ "{broken" }>());

constexpr auto k_tree = cjson::ct::parse<k_cfg>();

static_assert(k_tree.root()["listen"]["port"].i64_or(0) == 8080);
static_assert(k_tree.root()["listen"]["backlog"].i64_or(0) == 512);
static_assert(k_tree.root()["tls"].bool_or(false));
static_assert(k_tree.root()["hosts"].size() == 2);
static_assert(k_tree.root()["hosts"][usize(0)].str_is("a.example"));
static_assert(k_tree.root()["limits"]["body"].u64_or(0) == 1048576);
static_assert(k_tree.root()["limits"]["ratio"].f64_or(0) == 0.25);
static_assert(k_tree.root()["name"].str_len() == 5);      // caf + 2-byte é, decoded from é
static_assert(k_tree.root()["name"].str_at(3) == 0xc3 and k_tree.root()["name"].str_at(4) == 0xa9);
static_assert(!k_tree.root()["absent"]);

constexpr auto k_min = cjson::ct::minify<cjson::ct::str{ R"( { "s" : [ 1 , 2 ] } )" }>();

static_assert(k_min.size() == 11);
static_assert(k_min[0] == u8('{') and k_min[10] == u8('}'));

constexpr auto k_txt = cjson::ct::write<k_tree>();

static_assert(k_txt.size() > 100 and k_txt[0] == u8('{'));

};      // namespace

int
main()
{
  {
    sb::test_case("runtime verdicts equal the comptime verdicts for the same sources");
    sb::require_true(VR(R"({"a":1,"b":[true,false,null]})") == cjson::error::ok);
    sb::require_true(VR(R"("pair 😀 ok")") == cjson::error::ok);
    sb::require_true(VR(R"([1,])") == cjson::error::bad_syntax);
    sb::require_true(VR(R"(01)") == cjson::error::bad_number);
    sb::require_true(VR(R"("lone \ud800 high")") == cjson::error::bad_surrogate);
    sb::require_true(VR(R"("unclosed)") == cjson::error::bad_string);
    sb::require_true(VR("\"\xc3\x28\"") == cjson::error::bad_utf8);
    sb::require_true(VR("\"\x80\"") == cjson::error::bad_utf8);
    sb::require_true(VR("\"\xed\xa0\x80\"") == cjson::error::bad_utf8);
    sb::require_true(VR("\"\xf4\x90\x80\x80\"") == cjson::error::bad_utf8);
    sb::require_true(VR("\"\xc0\xaf\"") == cjson::error::bad_utf8);
    sb::require_true(VR("\"trunc \xe2\x82") == cjson::error::bad_string);
    sb::end_test_case();
  }
  return 1;
}
