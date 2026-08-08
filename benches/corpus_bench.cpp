//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include "_bench_common.hpp"

#include "../src/cjson/cjson.hpp"

#include <micron/linux/io.hpp>
#include <micron/linux/io/ext.hpp>
#include <micron/vector.hpp>

namespace
{

micron::vector<u8>
slurp(const char *path)
{
  micron::vector<u8> out;
  micron::posix::fd_t fd = micron::posix::open_read(path);
  if ( !fd.open() ) return out;
  micron::stat_t st{};
  if ( micron::fstat(fd, st) < 0 or st.st_size <= 0 ) {
    micron::posix::close_fd(fd);
    return out;
  }
  out.reserve(static_cast<usize>(st.st_size) + cjson::padding + 1);
  u8 buf[65536];
  for ( ;; ) {
    const max_t n = micron::posix::read(fd, buf, sizeof(buf));
    if ( n <= 0 ) break;
    for ( max_t i = 0; i < n; i++ ) out.push_back(buf[i]);
  }
  micron::posix::close_fd(fd);
  return out;
}

struct corpus {
  const char *path;
  const char *label;
  const char *ptr;
};

constexpr corpus k_corpora[] = {
  { "sample/64kb.json", "64kb", "" },
  { "sample/twitter.json", "twitter", "/statuses/0/user/screen_name" },
  { "sample/1MB.json", "1MB", "" },
  { "sample/5MB.json", "5MB", "" },
  { "sample/web/github_events.json", "github_events", "/0/actor/login" },
  { "sample/web/numbers.json", "numbers", "/5000" },
  { "sample/web/instruments.json", "instruments", "/name" },
  { "sample/web/countries.geo.json", "countries.geo", "/features/0/properties/name" },
  { "sample/web/update-center.json", "update-center", "/core/version" },
  { "sample/web/twitterescaped.json", "twitterescaped", "/statuses/0/user/screen_name" },
  { "sample/web/mesh.json", "mesh", "/positions/100" },
  { "sample/web/citm_catalog.json", "citm_catalog", "/areaNames/205705994" },
  { "sample/web/mesh.pretty.json", "mesh.pretty", "/positions/100" },
  { "sample/web/canada.json", "canada", "/features/0/properties/name" },
  { "sample/web/marine_ik.json", "marine_ik", "/metadata/version" },
  { "sample/web/gsoc-2018.json", "gsoc-2018", "/0/name" },
  { "sample/web/semanticscholar-corpus.json", "semanticscholar", "/0/magId" },
  { "sample/web/api.github.com.json", "gh-openapi", "/info/version" },

  { "sample/web/1GB.json", "1GB", "/0/meta/rev" },
};

u64
reps_cap(usize n)
{
  if ( n > (512u << 20) ) return 1;
  if ( n > (16u << 20) ) return 4;
  if ( n > (4u << 20) ) return 16;
  return mb::MAX_REPS;
}

bool
streq(const char *a, const char *b)
{
  while ( *a and *a == *b ) {
    ++a;
    ++b;
  }
  return *a == *b;
}

bool
starts_with(const char *s, const char *p)
{
  while ( *p )
    if ( *s++ != *p++ ) return false;
  return true;
}

bool
want(int argc, char **argv, const char *op)
{
  bool any_op = false;
  for ( int i = 1; i < argc; i++ ) {
    if ( starts_with(argv[i], "only=") ) continue;
    any_op = true;
    if ( streq(argv[i], op) ) return true;
  }
  return !any_op;
}

bool
want_corpus(int argc, char **argv, const char *label)
{
  const char *list = nullptr;
  for ( int i = 1; i < argc; i++ )
    if ( starts_with(argv[i], "only=") ) list = argv[i] + 5;
  if ( !list ) return true;

  usize ln = 0;
  while ( label[ln] ) ++ln;
  for ( const char *p = list; *p; ) {
    const char *e = p;
    while ( *e and *e != ',' ) ++e;
    if ( usize(e - p) == ln ) {
      bool eq = true;
      for ( usize i = 0; i < ln; i++ )
        if ( p[i] != label[i] ) {
          eq = false;
          break;
        }
      if ( eq ) return true;
    }
    p = (*e == ',') ? e + 1 : e;
  }
  return false;
}

};      // namespace

int
main(int argc, char **argv)
{
  mb::pin_cpu0();
  mb::print_header();

  const bool do_parse = want(argc, argv, "parse");
  const bool do_validate = want(argc, argv, "validate");
  const bool do_extract = want(argc, argv, "extract");
  const bool do_minify = want(argc, argv, "minify");
  const bool do_write = want(argc, argv, "write");

  for ( const corpus &c : k_corpora ) {
    if ( !want_corpus(argc, argv, c.label) ) continue;
    auto data = slurp(c.path);
    if ( data.size() == 0 ) continue;
    const usize n = data.size();
    const cjson::bytes in{ data.cbegin(), n };
    const u64 cap = reps_cap(n);
    if ( cjson::validate(in) != cjson::error::ok ) continue;

    const bool huge = n > (512u << 20);

    micron::io::println("### ", c.label, "  (", n, " B)");
    if ( huge ) micron::io::println("    (huge: write/minify skipped — output buffer will not fit beside the dom)");

    if ( do_parse ) {
      mb::row g[2];
      {
        cjson::scratch sc;
        g[0] = mb::bench_one("parse", "cjson", n, n, [&] { mb::sink_bool(cjson::parse(in, {}, sc).is_first()); }, cap);
      }
      {
        cjson::scratch sc;
        g[1] = mb::bench_one("parse", "cjson-reuse", n, n, [&] { mb::sink_bool(cjson::parse_reuse(in, {}, sc).is_first()); }, cap);
      }
      mb::print_group(g, 2);
    }

    if ( do_validate ) {
      cjson::scratch sc;
      mb::row solo = mb::bench_one("validate", "cjson", n, n, [&] { mb::sink_bool(cjson::validate(in, {}, sc) == cjson::error::ok); }, cap);
      mb::print_group(&solo, 1);
    }

    if ( do_extract && c.ptr[0] != '\0' ) {
      cjson::scratch sc;
      mb::row g[2];
      g[0] = mb::bench_one(
          "extract", "cjson-ondemand", n, n,
          [&] {
            auto rv = cjson::iterate(in, sc);
            if ( rv.is_first() ) mb::sink_bool(static_cast<bool>(rv.cast<cjson::view>().root().at_pointer(c.ptr)));
          },
          cap);

      g[1] = mb::bench_one(
          "extract", "cjson-index-only", n, n,
          [&] {
            auto rv = cjson::iterate(in, sc);
            mb::sink_bool(rv.is_first());
          },
          cap);
      mb::print_group(g, 2);
    }

    if ( do_minify && !huge ) {
      micron::vector<u8> out;
      out.reserve(n + cjson::padding);
      cjson::scratch sc;
      mb::row solo = mb::bench_one(
          "minify", "cjson", n, n, [&] { mb::sink_size(usize(cjson::minify(in, cjson::wbytes{ out.begin(), n }, {}, sc))); }, cap);
      mb::print_group(&solo, 1);
    }

    if ( do_write && !huge ) {
      auto r = cjson::parse(in, cjson::opts{ .with_write_bound = true });
      if ( r.is_first() ) {
        const cjson::doc &d = r.cast<cjson::doc>();
        const usize wcap = cjson::write_bound(d);
        micron::vector<u8> out;
        out.reserve(wcap + 8);
        mb::row g[2];
        g[0] = mb::bench_one(
            "write/minify", "cjson", n, n, [&] { mb::sink_size(usize(cjson::write_into(d, cjson::wbytes{ out.begin(), wcap }))); }, cap);
        g[1] = mb::bench_one(
            "write/pretty2", "cjson", n, n,
            [&] {
              cjson::fjson o = cjson::write(d, cjson::style{ .indent = 2 });
              mb::sink_size(o.size());
            },
            cap);
        mb::print_group(g, 2);
      }
    }
  }
  return 0;
}
