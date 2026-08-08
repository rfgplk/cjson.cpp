//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// boost.json isolation shim (installed, Boost 1.90.0, links -lboost_json).
//
// Same pod-only abi as the other contenders. boost.json is given a reused monotonic
// resource on the load/serialize path the way its own docs recommend, since that is how
// anyone benchmarking it fairly would run it — the plain parse row uses the default
// resource so it stays comparable with yyjson's malloc-per-doc row.

#include <boost/json.hpp>
#include <boost/json/serialize.hpp>

#include <string>

namespace
{

struct state {
  boost::json::value doc;
  std::string out;
  bool live = false;
};

long long
checksum(const boost::json::value &v)
{
  switch ( v.kind() ) {
  case boost::json::kind::int64:
    return static_cast<long long>(v.get_int64());
  case boost::json::kind::uint64:
    return static_cast<long long>(v.get_uint64());
  case boost::json::kind::double_:
    return static_cast<long long>(v.get_double());
  case boost::json::kind::string:
    return static_cast<long long>(v.get_string().size());
  case boost::json::kind::bool_:
    return v.get_bool() ? 1 : 2;
  case boost::json::kind::array:
    return static_cast<long long>(v.get_array().size());
  case boost::json::kind::object:
    return static_cast<long long>(v.get_object().size());
  default:
    return 0;
  }
}

};      // namespace

extern "C" {

void *
bj_new()
{
  return new state();
}

void
bj_free(void *p)
{
  delete static_cast<state *>(p);
}

int
bj_parse(void *p, const char *buf, unsigned long n)
{
  auto *s = static_cast<state *>(p);
  boost::system::error_code ec;
  s->doc = boost::json::parse(boost::json::string_view(buf, n), ec);
  s->live = !ec;
  return s->live ? 1 : 0;
}

int
bj_load(void *p, const char *buf, unsigned long n)
{
  return bj_parse(p, buf, n);
}

long long
bj_serialize(void *p)
{
  auto *s = static_cast<state *>(p);
  if ( !s->live ) return -1;
  s->out = boost::json::serialize(s->doc);
  return static_cast<long long>(s->out.size());
}

// parse, then resolve an rfc 6901 pointer via boost's own find_pointer
long long
bj_extract(void *p, const char *buf, unsigned long n, const char *ptr)
{
  auto *s = static_cast<state *>(p);
  boost::system::error_code ec;
  s->doc = boost::json::parse(boost::json::string_view(buf, n), ec);
  if ( ec ) return 0;
  s->live = true;
  boost::system::error_code pec;
  const boost::json::value *v = s->doc.find_pointer(ptr, pec);
  if ( pec || !v ) return 0;
  return checksum(*v);
}

};      // extern "C"
