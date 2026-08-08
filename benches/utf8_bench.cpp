//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// utf-8 validation in isolation (the stage-1 second-pass cost today, the fused-path
// cost after), over ascii-dominant and multibyte-dense inputs, plus the utf8 share of
// whole-document validate (on/off delta on twitter).
// build via benches.duck; run pinned (the harness pins cpu 0)

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
  out.reserve(static_cast<usize>(st.st_size) + 1);
  u8 buf[65536];
  for ( ;; ) {
    const max_t n = micron::posix::read(fd, buf, sizeof(buf));
    if ( n <= 0 ) break;
    for ( max_t i = 0; i < n; i++ ) out.push_back(buf[i]);
  }
  micron::posix::close_fd(fd);
  return out;
}

constexpr usize synth_n = 1u << 20;

void
val_row(const char *tag, const u8 *p, usize n)
{
  mb::print_row(mb::bench_one("utf8/validate", tag, n, n, [&] { mb::sink_bool(cjson::__utf8::validate_scalar(p, n)); }));
}

};      // namespace

int
main()
{
  mb::pin_cpu0();
  mb::print_header();

  {
    auto lf = slurp("sample/large-file.json");
    if ( lf.size() != 0 ) val_row("corpus-largefile", lf.cbegin(), lf.size());
  }
  auto tw = slurp("sample/twitter.json");
  if ( tw.size() != 0 ) val_row("corpus-twitter", tw.cbegin(), tw.size());

  // synthetic 1MB bodies
  static u8 ascii[synth_n];
  for ( usize i = 0; i < synth_n; i++ ) ascii[i] = u8('a' + (i % 26));
  val_row("synth-ascii", ascii, synth_n);

  // ~60% multibyte: ascii word, 2-byte, 3-byte, 4-byte round robin
  static u8 mixed[synth_n];
  {
    usize w = 0;
    while ( w + 16 <= synth_n ) {
      mixed[w++] = u8('x');
      mixed[w++] = u8(' ');
      mixed[w++] = 0xC2;
      mixed[w++] = 0xA2;      // U+00A2
      mixed[w++] = 0xE2;
      mixed[w++] = 0x82;
      mixed[w++] = 0xAC;      // U+20AC
      mixed[w++] = 0xF0;
      mixed[w++] = 0x9F;
      mixed[w++] = 0x98;
      mixed[w++] = 0x80;      // U+1F600
      mixed[w++] = u8('y');
      mixed[w++] = u8('z');
      mixed[w++] = 0xE4;
      mixed[w++] = 0xB8;
      mixed[w++] = 0xAD;      // U+4E2D
    }
    while ( w < synth_n ) mixed[w++] = u8(' ');
  }
  val_row("synth-mixed", mixed, synth_n);

  // dense cjk: pure 3-byte sequences
  static u8 cjk[synth_n];
  {
    usize w = 0;
    while ( w + 3 <= synth_n ) {
      cjk[w++] = 0xE4;
      cjk[w++] = 0xB8;
      cjk[w++] = u8(0x80 + (w % 48));
    }
    while ( w < synth_n ) cjk[w++] = u8(' ');
  }
  val_row("synth-cjk", cjk, synth_n);

  // utf8 share of whole-document validate: on vs off, twitter
  if ( tw.size() != 0 ) {
    const cjson::bytes in{ tw.cbegin(), tw.size() };
    mb::print_row(mb::bench_one("validate/utf8-on", "cjson", tw.size(), tw.size(),
                                [&] { mb::sink_bool(cjson::validate(in, {}) == cjson::error::ok); }));
    mb::print_row(mb::bench_one("validate/utf8-off", "cjson", tw.size(), tw.size(),
                                [&] { mb::sink_bool(cjson::validate(in, cjson::opts{ .skip_utf8 = true }) == cjson::error::ok); }));
  }

  return 0;
}
