//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// rapidjson isolation shim (installed, /usr/include/rapidjson, 1.1.0).
//
// REQUIRED, not optional: rapidjson auto-enables RAPIDJSON_SSE2/SSE42 under
// -march=native (rapidjson.h:354-382) and pulls in <emmintrin.h>/<nmmintrin.h>, which
// collide with micron's reimplementation of the same intrinsics. Disabling them with
// -DRAPIDJSON_SSE2=0 would compile, but then the comparison is not flag-matched, so the
// shim keeps them on and keeps micron out of the tu instead.
//
// kParseInsituFlag is deliberately NOT used: the other contenders' parse rows are all
// copy-mode, and cjson's insitu row is labelled separately when it appears.

#include <rapidjson/document.h>
#include <rapidjson/pointer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace
{

struct state {
  rapidjson::Document doc;
  rapidjson::StringBuffer sb;
};

long long
checksum(const rapidjson::Value &v)
{
  if ( v.IsInt64() ) return v.GetInt64();
  if ( v.IsUint64() ) return static_cast<long long>(v.GetUint64());
  if ( v.IsDouble() ) return static_cast<long long>(v.GetDouble());
  if ( v.IsString() ) return static_cast<long long>(v.GetStringLength());
  if ( v.IsBool() ) return v.GetBool() ? 1 : 2;
  if ( v.IsArray() ) return static_cast<long long>(v.Size());
  if ( v.IsObject() ) return static_cast<long long>(v.MemberCount());
  return 0;
}

};      // namespace

extern "C" {

void *
rj_new()
{
  return new state();
}

void
rj_free(void *p)
{
  delete static_cast<state *>(p);
}

// A FRESH Document per call, deliberately.
//
// Reusing one Document across hundreds of parses of different documents segfaults
// inside ParseValue with a shallow stack — rapidjson's MemoryPoolAllocator is owned by
// the Document and repeated Parse calls on one instance do not reset it the way a fresh
// instance does. It is also the flag-matched thing to do: yyjson's row allocates and
// frees a document per op, and so should this one.
int
rj_parse(void *, const char *buf, unsigned long n)
{
  rapidjson::Document doc;
  doc.Parse(buf, n);
  return doc.HasParseError() ? 0 : 1;
}

// load RETAINS, so rj_serialize measures serialization alone
int
rj_load(void *p, const char *buf, unsigned long n)
{
  auto *s = static_cast<state *>(p);
  s->doc.Parse(buf, n);
  return s->doc.HasParseError() ? 0 : 1;
}

long long
rj_serialize(void *p)
{
  auto *s = static_cast<state *>(p);
  if ( s->doc.HasParseError() ) return -1;
  s->sb.Clear();
  rapidjson::Writer<rapidjson::StringBuffer> w(s->sb);
  if ( !s->doc.Accept(w) ) return -1;
  return static_cast<long long>(s->sb.GetSize());
}

long long
rj_extract(void *, const char *buf, unsigned long n, const char *ptr)
{
  rapidjson::Document doc;
  doc.Parse(buf, n);
  if ( doc.HasParseError() ) return 0;
  const rapidjson::Value *v = rapidjson::Pointer(ptr).Get(doc);
  return v ? checksum(*v) : 0;
}

};      // extern "C"
