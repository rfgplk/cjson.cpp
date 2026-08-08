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

namespace cj = cjson;

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

};      // namespace

int
main()
{
  mb::pin_cpu0();
  mb::print_header();

  auto data = slurp("sample/twitter.json");
  if ( data.size() == 0 ) return 0;
  const usize n = data.size();
  const cjson::bytes in{ data.cbegin(), n };

  auto rd = cjson::parse(in);
  if ( rd.is_second() ) return 0;
  const cjson::doc &d = rd.cast<cjson::doc>();
  const cjson::val statuses = d.root()["statuses"];
  const usize count = statuses.size();

  const u64 per_op = count ? count : 1;
  constexpr u64 REPS = 2000;

  auto run = [&](const char *op, const char *impl, u64 per, auto &&f) { return mb::bench_one(op, impl, n, per, f, REPS); };

  {

    mb::row g[2];
    g[0] = run("fold(sum retweet_count)", "cjson-handloop", per_op, [&] {
      i64 acc = 0;
      for ( auto s : statuses.items() ) acc += s["retweet_count"].i64_or(0);
      mb::sink_size(usize(acc));
    });
    g[1] = run("fold(sum retweet_count)", "cjson-fp", per_op, [&] {
      const i64 acc = statuses.items() | cj::pluck_c("retweet_count") | cj::fmap_c(cj::to_i64) | cj::fold_c(i64(0), cj::plus);
      mb::sink_size(usize(acc));
    });
    mb::print_group(g, 2);
  }
  {

    mb::row g[2];
    g[0] = run("filter+fold(favorited)", "cjson-handloop", per_op, [&] {
      i64 acc = 0;
      for ( auto s : statuses.items() )
        if ( s["favorited"].bool_or(false) ) acc += s["retweet_count"].i64_or(0);
      mb::sink_size(usize(acc));
    });
    g[1] = run("filter+fold(favorited)", "cjson-fp", per_op, [&] {
      const i64 acc = statuses.items() | cj::filter_c(cj::field_bool("favorited")) | cj::pluck_c("retweet_count") | cj::fmap_c(cj::to_i64)
                      | cj::fold_c(i64(0), cj::plus);
      mb::sink_size(usize(acc));
    });
    mb::print_group(g, 2);
  }
  {

    mb::row g[2];
    g[0] = run("count_if(truncated)", "cjson-handloop", per_op, [&] {
      usize c = 0;
      for ( auto s : statuses.items() )
        if ( s["truncated"].bool_or(false) ) ++c;
      mb::sink_size(c);
    });
    g[1] = run("count_if(truncated)", "cjson-fp", per_op,
               [&] { mb::sink_size(cj::count_if(cj::field_bool("truncated"), statuses.items())); });
    mb::print_group(g, 2);
  }
  {

    mb::row g[2];
    g[0] = run("max_by(retweet_count)", "cjson-handloop", per_op, [&] {
      i64 best = -1;
      for ( auto s : statuses.items() ) {
        const i64 v = s["retweet_count"].i64_or(0);
        if ( v > best ) best = v;
      }
      mb::sink_size(usize(best));
    });
    g[1] = run("max_by(retweet_count)", "cjson-fp", per_op, [&] {
      auto r = cj::max_by(cj::field_i64("retweet_count"), statuses.items());
      mb::sink_bool(r.is_first());
    });
    mb::print_group(g, 2);
  }
  {

    const cjson::val first = statuses.at(0);
    const u64 kper = first.size() ? first.size() : 1;
    mb::row g[2];
    g[0] = run("members(keylen sum)", "cjson-handloop", kper, [&] {
      usize acc = 0;
      for ( auto m : first.members() ) acc += m.key.len;
      mb::sink_size(acc);
    });
    g[1] = run("members(keylen sum)", "cjson-fp", kper, [&] {
      const usize acc = cj::fold(cj::fmap([](cjson::strv k) { return k.len; }, cj::keys(first.members())), usize(0), cj::plus);
      mb::sink_size(acc);
    });
    mb::print_group(g, 2);
  }

  {
    cjson::scratch sc;
    auto rv = cjson::iterate(in, sc);
    if ( rv.is_first() ) {
      const cjson::view v = rv.cast<cjson::view>();
      const cjson::cur ostatuses = v.root()["statuses"];
      const usize ocount = ostatuses.count();
      const u64 oper = ocount ? ocount : 1;

      mb::row g[3];
      g[0] = run("ondemand walk(sum rt)", "cjson-items", oper, [&] {
        i64 acc = 0;
        for ( auto s : ostatuses.items() ) acc += s["retweet_count"].i64_or(0);
        mb::sink_size(usize(acc));
      });
      g[1] = run("ondemand walk(sum rt)", "cjson-at-indexed", oper, [&] {
        i64 acc = 0;
        for ( usize i = 0; i < ocount; ++i ) acc += ostatuses.at(i)["retweet_count"].i64_or(0);
        mb::sink_size(usize(acc));
      });
      g[2] = run("ondemand walk(sum rt)", "cjson-fp", oper, [&] {
        const i64 acc = ostatuses.items() | cj::pluck_c("retweet_count") | cj::fmap_c(cj::to_i64) | cj::fold_c(i64(0), cj::plus);
        mb::sink_size(usize(acc));
      });
      mb::print_group(g, 3);
    }
  }
  return 0;
}
