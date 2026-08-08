//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// nlohmann/json isolation shim (installed, /usr/include/nlohmann, 3.12.0).
//
// nlohmann uses no intrinsics, so unlike simdjson/rapidjson/glaze it *could* share a tu
// with micron. It gets a shim anyway: the abi below is the same one every contender
// speaks, which keeps benches/corpus_vs.cpp free of per-library special cases, and it
// keeps ~200 KB of stl template instantiation out of the -flto link.
//
// The abi is deliberately pod-only (void*/int/long long/const char*/unsigned long) —
// no stl type ever crosses it. Compiled by scripts/vsbuild with no micron include path.

#include <nlohmann/json.hpp>

#include <string>

namespace
{

struct state {
  nlohmann::json doc;
  std::string out;
};

// one comparable number per value kind, so every contender's extract row is checking
// the same thing: numbers by value, strings by length, bools as 1/2, containers by size
long long
checksum(const nlohmann::json &v)
{
  if ( v.is_number_integer() ) return static_cast<long long>(v.get<long long>());
  if ( v.is_number_unsigned() ) return static_cast<long long>(v.get<unsigned long long>());
  if ( v.is_number_float() ) return static_cast<long long>(v.get<double>());
  if ( v.is_string() ) return static_cast<long long>(v.get_ref<const std::string &>().size());
  if ( v.is_boolean() ) return v.get<bool>() ? 1 : 2;
  if ( v.is_array() || v.is_object() ) return static_cast<long long>(v.size());
  return 0;
}

};      // namespace

extern "C" {

void *
nl_new()
{
  return new state();
}

void
nl_free(void *p)
{
  delete static_cast<state *>(p);
}

// full dom parse, result discarded; returns 1 on success
int
nl_parse(void *p, const char *buf, unsigned long n)
{
  auto *s = static_cast<state *>(p);
  s->doc = nlohmann::json::parse(buf, buf + n, nullptr, false);
  return s->doc.is_discarded() ? 0 : 1;
}

// parse and RETAIN, so nl_serialize measures serialization alone
int
nl_load(void *p, const char *buf, unsigned long n)
{
  return nl_parse(p, buf, n);
}

long long
nl_serialize(void *p)
{
  auto *s = static_cast<state *>(p);
  if ( s->doc.is_discarded() ) return -1;
  s->out = s->doc.dump();
  return static_cast<long long>(s->out.size());
}

// parse, then resolve an rfc 6901 pointer. nlohmann has no lazy path — a full dom is
// the only way in, and that is exactly what this row is meant to show.
long long
nl_extract(void *p, const char *buf, unsigned long n, const char *ptr)
{
  auto *s = static_cast<state *>(p);
  s->doc = nlohmann::json::parse(buf, buf + n, nullptr, false);
  if ( s->doc.is_discarded() ) return 0;
  try {
    const nlohmann::json::json_pointer jp{ ptr };
    if ( !s->doc.contains(jp) ) return 0;
    return checksum(s->doc.at(jp));
  } catch ( ... ) {
    return 0;
  }
}

};      // extern "C"
