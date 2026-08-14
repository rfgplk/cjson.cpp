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
//
// That paragraph described an intention, not the code, until this was fixed: no
// monotonic_resource existed anywhere in the file, so boost.json ran its slowest
// supported configuration on both rows while the comment asserted the opposite. It now
// does what it says.
//
//   * bj_parse / bj_extract  — default resource, node-at-a-time malloc. These are the
//     parse-per-op rows and they stay comparable with yyjson's malloc-per-doc row.
//   * bj_load / bj_serialize — a monotonic arena, rebuilt per corpus (so it cannot grow
//     across the 18 documents) and retained across the timed serialize reps. The tree is
//     then arena-contiguous, which is the point of the recommendation: serialize walks it
//     without chasing scattered nodes. Output goes into a retained std::string through
//     boost::json::serializer's streaming api instead of a fresh string per rep.

#include <boost/json.hpp>
#include <boost/json/monotonic_resource.hpp>
#include <boost/json/serialize.hpp>
#include <boost/json/serializer.hpp>

#include <memory>
#include <string>

namespace
{

// the load/serialize arena. mr is declared BEFORE doc so doc is destroyed first, and the
// whole object is rebuilt per corpus — that rebuild is how the arena is reclaimed, since a
// monotonic_resource frees nothing until it dies, by design.
struct load_arena {
  boost::json::monotonic_resource mr;
  boost::json::value doc;

  // parens, not braces: brace-init on a json::value selects the value_ref list ctor and
  // would try to build a value *holding* the storage_ptr rather than one using it
  load_arena() : mr(), doc(boost::json::storage_ptr(&mr)) { }
};

struct state {
  boost::json::value doc;              // parse / extract rows — default resource
  std::unique_ptr<load_arena> la;      // load / serialize rows — retained arena
  boost::json::serializer sr;          // retained across reps; reset() re-points it
  std::string out;                     // retained across reps; clear() keeps the capacity
  bool live = false;                   // doc (parse/extract) is valid
  bool load_live = false;              // la->doc (load/serialize) is valid
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

// load RETAINS, so bj_serialize measures serialization alone. A fresh arena per corpus:
// the previous document's blocks go back to the allocator here instead of accumulating
// across all 18 documents.
int
bj_load(void *p, const char *buf, unsigned long n)
{
  auto *s = static_cast<state *>(p);
  s->la = std::make_unique<load_arena>();
  boost::system::error_code ec;
  // storage matches on both sides, so this is an ownership transfer, not a deep copy back
  // into the default resource — which is what an assignment across differing storage does
  s->la->doc = boost::json::parse(boost::json::string_view(buf, n), ec, boost::json::storage_ptr(&s->la->mr));
  s->load_live = !ec;
  return s->load_live ? 1 : 0;
}

long long
bj_serialize(void *p)
{
  auto *s = static_cast<state *>(p);
  if ( !s->load_live || !s->la ) return -1;
  s->out.clear();      // keeps the capacity; only the first rep pays for growth
  s->sr.reset(&s->la->doc);
  char chunk[16384];
  while ( !s->sr.done() ) {
    const boost::json::string_view sv = s->sr.read(chunk, sizeof(chunk));
    s->out.append(sv.data(), sv.size());
  }
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
