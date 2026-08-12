//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// simdjson isolation shim: micron reimplements the x86 intrinsics for its freestanding
// world, so micron headers and <immintrin.h> cannot coexist in one tu. the comparison
// benches therefore reach simdjson through this c abi, compiled by scripts/vsbuild
// with simdjson only

#include <simdjson.h>

namespace
{

// the per-kind number every contender in the extract groups returns for the same
// pointer: numbers by value, strings by length, bools 1/2, containers by element count,
// 0 on any miss or error. factored out of sj_od_extract so the n-field row below cannot
// drift from the one-shot row it is supposed to extend.
long long
od_checksum(simdjson::ondemand::value val)
{
  simdjson::ondemand::json_type t;
  if ( val.type().get(t) != simdjson::SUCCESS ) return 0;
  switch ( t ) {
  case simdjson::ondemand::json_type::number: {
    double d;
    if ( val.get_double().get(d) != simdjson::SUCCESS ) return 0;
    return (long long)d;
  }
  case simdjson::ondemand::json_type::string: {
    std::string_view sv;
    if ( val.get_string().get(sv) != simdjson::SUCCESS ) return 0;
    return (long long)sv.size();
  }
  case simdjson::ondemand::json_type::boolean: {
    bool b;
    if ( val.get_bool().get(b) != simdjson::SUCCESS ) return 0;
    return b ? 1 : 2;
  }
  case simdjson::ondemand::json_type::array: {
    simdjson::ondemand::array a;
    if ( val.get_array().get(a) != simdjson::SUCCESS ) return 0;
    size_t c = 0;
    if ( a.count_elements().get(c) != simdjson::SUCCESS ) return 0;
    return (long long)c;
  }
  case simdjson::ondemand::json_type::object: {
    simdjson::ondemand::object o;
    if ( val.get_object().get(o) != simdjson::SUCCESS ) return 0;
    size_t c = 0;
    if ( o.count_fields().get(c) != simdjson::SUCCESS ) return 0;
    return (long long)c;
  }
  default:
    return 0;
  }
}

};      // namespace

extern "C" {

void *
sj_dom_parser_new()
{
  return new simdjson::dom::parser();
}

void
sj_dom_parser_free(void *p)
{
  delete static_cast<simdjson::dom::parser *>(p);
}

// full dom parse; returns 1 on success. buf needs no padding (realloc path allowed)
int
sj_dom_parse(void *p, const char *buf, unsigned long n)
{
  simdjson::dom::element e;
  return static_cast<simdjson::dom::parser *>(p)->parse(buf, n, true).get(e) == simdjson::SUCCESS ? 1 : 0;
}

// generic rfc 6901 extract on the on-demand path — the corpus_vs `extract` row, where
// every contender is handed the same document and the same pointer. returns one
// comparable number per kind (numbers by value, strings by length, bools 1/2,
// containers by element count), 0 on any miss or error.
long long
sj_od_extract(void *p, const char *buf, unsigned long n, const char *ptr)
{
  auto *parser = static_cast<simdjson::ondemand::parser *>(p);
  simdjson::padded_string_view v(buf, n, n + simdjson::SIMDJSON_PADDING);
  auto doc = parser->iterate(v);
  // simdjson spells the whole-document pointer "" while json pointer spells it ""; its
  // at_pointer rejects the empty string on a document, so shortcut to the root type
  simdjson::ondemand::value val;
  if ( ptr[0] == '\0' ) {
    simdjson::ondemand::json_type t;
    if ( doc.type().get(t) != simdjson::SUCCESS ) return 0;
    return 1;
  }
  if ( doc.at_pointer(ptr).get(val) != simdjson::SUCCESS ) return 0;
  return od_checksum(val);
}

// N pointers against ONE iterate: the sweep row (benches/lazy_vs.cpp). simdjson pays
// stage 1 once here and then walks the value tree per pointer, which is the same deal
// cjson's iterate + N at_pointer gets, so the two are directly comparable.
//
// Repeated at_pointer on one document is legal and needs no ordering care: ondemand's
// document::at_pointer calls rewind() on entry, so each pointer restarts the CURSOR from
// the document root while the structural index built by iterate() is kept. It is the
// index that is amortized, not the walk. (A forward-only ondemand::value would raise
// OUT_OF_ORDER_ITERATION on a rewind; the document overload is the one that does not.)
long long
sj_od_nfields(void *p, const char *buf, unsigned long n, const char *const *ptrs, int count)
{
  auto *parser = static_cast<simdjson::ondemand::parser *>(p);
  simdjson::padded_string_view v(buf, n, n + simdjson::SIMDJSON_PADDING);
  auto doc = parser->iterate(v);
  long long sum = 0;
  for ( int i = 0; i < count; i++ ) {
    simdjson::ondemand::value val;
    if ( doc.at_pointer(ptrs[i]).get(val) != simdjson::SUCCESS ) continue;
    sum += od_checksum(val);
  }
  return sum;
}

void *
sj_od_parser_new()
{
  return new simdjson::ondemand::parser();
}

void
sj_od_parser_free(void *p)
{
  delete static_cast<simdjson::ondemand::parser *>(p);
}

// on-demand iterate + root type touch; buf must carry SIMDJSON_PADDING writable slack
int
sj_od_touch(void *p, const char *buf, unsigned long n)
{
  auto *parser = static_cast<simdjson::ondemand::parser *>(p);
  simdjson::padded_string_view v(buf, n, n + simdjson::SIMDJSON_PADDING);
  auto doc = parser->iterate(v);
  simdjson::ondemand::json_type t;
  return doc.type().get(t) == simdjson::SUCCESS ? 1 : 0;
}

// jwt-shaped: iterate + extract exp (int), sub (raw string len), admin (bool).
// returns exp + len + admin as a checksum, 0 on any error
long long
sj_od_claims(void *p, const char *buf, unsigned long n)
{
  auto *parser = static_cast<simdjson::ondemand::parser *>(p);
  simdjson::padded_string_view v(buf, n, n + simdjson::SIMDJSON_PADDING);
  auto doc = parser->iterate(v);
  long long exp = 0;
  std::string_view sub;
  bool admin = true;
  if ( doc["exp"].get(exp) != simdjson::SUCCESS ) return 0;
  if ( doc["sub"].get(sub) != simdjson::SUCCESS ) return 0;
  if ( doc["admin"].get(admin) != simdjson::SUCCESS ) return 0;
  return exp + (long long)sub.size() + (admin ? 1 : 0);
}

// text->text minify (no validation — simdjson::minify is stage-1 only); dst must have
// at least n bytes. returns the minified length, -1 on error
long long
sj_minify(const char *buf, unsigned long n, char *dst)
{
  size_t out = 0;
  if ( simdjson::minify(buf, n, dst, out) != simdjson::SUCCESS ) return -1;
  return (long long)out;
}

// twitter-shaped: first status id + user screen_name length
long long
sj_od_twitter(void *p, const char *buf, unsigned long n)
{
  auto *parser = static_cast<simdjson::ondemand::parser *>(p);
  simdjson::padded_string_view v(buf, n, n + simdjson::SIMDJSON_PADDING);
  auto doc = parser->iterate(v);
  unsigned long long id = 0;
  std::string_view name;
  auto first = doc["statuses"].at(0);
  if ( first["id"].get(id) != simdjson::SUCCESS ) return 0;
  if ( first["user"]["screen_name"].get(name) != simdjson::SUCCESS ) return 0;
  return (long long)(id + name.size());
}

};      // extern "C"
