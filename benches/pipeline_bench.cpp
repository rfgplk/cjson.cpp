//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// the ox request lifecycle in miniature: iterate a request body, extract fields, compose
// a response with the builder — scratch and builder buffers hot vs cold. sizes span the
// api-body range

#include "_bench_common.hpp"

#include "../src/cjson/cjson.hpp"

#include <micron/vector.hpp>

namespace
{

// synthesize an api-ish body of roughly n bytes
micron::vector<u8>
make_body(usize target)
{
  micron::vector<u8> v;
  auto app = [&](const char *s) {
    for ( usize i = 0; s[i]; i++ ) v.push_back(u8(s[i]));
  };
  app("{\"user\":\"someone@example.com\",\"active\":true,\"score\":41.5,\"items\":[");
  bool first = true;
  u32 id = 0;
  while ( v.size() + 96 < target ) {
    if ( !first ) v.push_back(u8(','));
    first = false;
    app("{\"id\":");
    char num[16];
    u8 *e = cjson::__itoa::write_u64(reinterpret_cast<u8 *>(num), id++);
    for ( char *c = num; c < reinterpret_cast<char *>(e); c++ ) v.push_back(u8(*c));
    app(",\"tag\":\"item-tag\",\"w\":0.25}");
  }
  app("],\"exp\":1735689600}");
  return v;
}

};      // namespace

int
main()
{
  mb::pin_cpu0();
  mb::print_header();

  const usize sizes[] = { 256, 1024, 4096, 16384 };

  for ( const usize target : sizes ) {
    auto body = make_body(target);
    const usize n = body.size();

    // hot: per-shard scratch + recycled builder buffer (the steady-state server loop)
    {
      cjson::scratch sc;
      micron::string reuse{};
      reuse.reserve(1024);
      mb::print_row(mb::bench_one("req->resp/hot", "cjson", n, n, [&] {
        auto rv = cjson::iterate(cjson::bytes{ body.cbegin(), n }, {}, sc);
        auto root = rv.cast<cjson::view>().root();
        const i64 exp = root["exp"].i64_or(0);
        const auto user = root["user"].str_raw();
        const usize items = root["items"].count();
        cjson::builder b(micron::move(reuse));
        b.obj().kv("ok", true).kv("exp", exp).kv("items", u64(items)).key("user").value(user).end();
        auto out = b.out();
        mb::sink_size(out.len + usize(exp));
        reuse = b.take();
      }));
    }

    // cold: everything allocated per request
    mb::print_row(mb::bench_one("req->resp/cold", "cjson", n, n, [&] {
      cjson::scratch sc;
      auto rv = cjson::iterate(cjson::bytes{ body.cbegin(), n }, {}, sc);
      auto root = rv.cast<cjson::view>().root();
      const i64 exp = root["exp"].i64_or(0);
      const auto user = root["user"].str_raw();
      cjson::builder b;
      b.obj().kv("ok", true).kv("exp", exp).key("user").value(user).end();
      auto out = b.out();
      mb::sink_size(out.len + usize(exp));
    }));

    // full dom for comparison
    {
      cjson::scratch sc;
      mb::print_row(mb::bench_one("req->resp/dom", "cjson", n, n, [&] {
        auto rd = cjson::parse(cjson::bytes{ body.cbegin(), n }, {}, sc);
        const cjson::doc &d = rd.cast<cjson::doc>();
        const i64 exp = d.root()["exp"].i64_or(0);
        cjson::builder b;
        b.obj().kv("ok", true).kv("exp", exp).end();
        mb::sink_size(b.out().len + usize(exp));
      }));
    }
  }
  return 0;
}
