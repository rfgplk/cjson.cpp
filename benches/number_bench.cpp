//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// number-kernel micro rows: __num::read_number over synthetic tokens spanning the
// int / Clinger / Eisel-Lemire / truncated / bigint tiers, plus __itoa::write_u64 and
// __dtoa::write_f64 in isolation. tokens live in padded buffers (',' terminator, nul,
// spaces) so terminator checks see real bytes. bytes_per_op = token length.
// build via benches.duck; run pinned (the harness pins cpu 0)

#include "_bench_common.hpp"

#include "../src/cjson/cjson.hpp"

namespace
{

struct tok {
  u8 buf[192];
  usize len = 0;

  void
  set(const char *s)
  {
    usize n = 0;
    while ( s[n] ) {
      buf[n] = u8(s[n]);
      ++n;
    }
    buf[n] = u8(',');
    buf[n + 1] = 0;
    for ( usize i = n + 2; i < sizeof(buf); i++ ) buf[i] = 0x20;
    len = n;
  }
};

struct rng {
  u64 s = 0x9e3779b97f4a7c15ull;

  u64
  next()
  {
    s ^= s << 13;
    s ^= s >> 7;
    s ^= s << 17;
    return s;
  }

  u64
  below(u64 n)
  {
    return next() % n;
  }
};

void
rn_row(const char *op, const tok &t)
{
  mb::print_row(mb::bench_one(op, "cjson", t.len, t.len, [&] {
    cjson::value v{};
    const max_t r = cjson::__num::read_number(t.buf, t.len + 2, 0, v);
    mb::sink_bool(r >= 0);
    mb::sink_u64 += v.pay.u;
  }));
}

};      // namespace

int
main()
{
  mb::pin_cpu0();
  mb::print_header();

  // ------------------------------------------------------------- read_number: ints
  const char *ints[] = { "9", "42", "9999", "99999999", "999999999999", "9999999999999999", "9223372036854775807", "18446744073709551615" };
  const char *int_ops[] = { "rn/int-1", "rn/int-2", "rn/int-4", "rn/int-8", "rn/int-12", "rn/int-16", "rn/int-19", "rn/int-20" };
  for ( usize i = 0; i < 8; i++ ) {
    tok t;
    t.set(ints[i]);
    rn_row(int_ops[i], t);
  }

  // mixed-length signed ints, 1024 per op (defeats branch-predictor lock-in)
  {
    static u8 mixbuf[32768];
    static u32 offs[1024];
    rng rg;
    usize w = 0;
    for ( u32 i = 0; i < 1024; i++ ) {
      offs[i] = u32(w);
      if ( rg.below(2) ) mixbuf[w++] = u8('-');
      const u32 nd = 1 + u32(rg.below(19));
      mixbuf[w++] = u8('1' + rg.below(9));
      for ( u32 d = 1; d < nd; d++ ) mixbuf[w++] = u8('0' + rg.below(10));
      mixbuf[w++] = u8(',');
    }
    mixbuf[w] = 0;
    for ( usize i = w + 1; i < sizeof(mixbuf); i++ ) mixbuf[i] = 0x20;
    mb::print_row(mb::bench_one("rn/int-mixed1024", "cjson", w, w, [&] {
      for ( u32 i = 0; i < 1024; i++ ) {
        cjson::value v{};
        mb::sink_bool(cjson::__num::read_number(mixbuf, w + 2, offs[i], v) >= 0);
      }
    }));
  }

  // ------------------------------------------------------- read_number: fractions
  {
    const char *fr[] = { "3.14159", "0.12345678", "1.2345678901234567", "1.23456789012345678901234", "0.0000012345" };
    const char *fr_ops[] = { "rn/frac-short", "rn/frac-8", "rn/frac-17", "rn/frac-24", "rn/leadzero" };
    for ( usize i = 0; i < 5; i++ ) {
      tok t;
      t.set(fr[i]);
      rn_row(fr_ops[i], t);
    }
  }

  // ------------------------------------- read_number: the shapes the CORPUS actually has
  //
  // Every fraction row above starts from a 1-digit integer part, which is not what the
  // corpora that lose to simdjson look like. These three are taken from the measured
  // distributions (see the ins/B table in ARCHITECTURE.md's cross-library findings):
  //
  //   canada.json   98% of fractions >= 8 digits, mean 14.6, 2-digit signed int part
  //   countries.geo mean 5.7 fraction digits, only 0.8% >= 8 — SHORT fractions, and the
  //                 regime where cjson is relatively worst (2.16x simdjson ins/B)
  //   1GB.json      mean 5.2, 0% >= 8
  //
  // The short-fraction rows matter most: no 8-digit lane can fire there, so they isolate
  // the per-digit and fixed-overhead cost from the SWAR gulp entirely.
  {
    const char *fr[] = { "-65.613616999999977", "-5.716667", "-122.4194", "45.0", "-0.123456" };
    const char *fr_ops[] = { "rn/frac-canada", "rn/frac-geo6", "rn/frac-geo4", "rn/frac-geo1", "rn/frac-geo6z" };
    for ( usize i = 0; i < 5; i++ ) {
      tok t;
      t.set(fr[i]);
      rn_row(fr_ops[i], t);
    }
  }

  // mixed-width floats, 1024 per op — the fraction-side twin of rn/int-mixed1024.
  // Without this every fraction row above runs one shape in a tight loop and the branch
  // predictor learns the digit count, which flatters any per-digit change.
  {
    static u8 fmixbuf[65536];
    static u32 foffs[1024];
    rng rg;
    usize w = 0;
    for ( u32 i = 0; i < 1024; i++ ) {
      foffs[i] = u32(w);
      if ( rg.below(2) ) fmixbuf[w++] = u8('-');
      const u32 id = 1 + u32(rg.below(3));      // 1-3 digit integer part, as in geo data
      fmixbuf[w++] = u8('1' + rg.below(9));
      for ( u32 d = 1; d < id; d++ ) fmixbuf[w++] = u8('0' + rg.below(10));
      fmixbuf[w++] = u8('.');
      const u32 fd = 1 + u32(rg.below(17));      // 1-17 fraction digits
      for ( u32 d = 0; d < fd; d++ ) fmixbuf[w++] = u8('0' + rg.below(10));
      fmixbuf[w++] = u8(',');
    }
    fmixbuf[w] = 0;
    for ( usize i = w + 1; i < sizeof(fmixbuf); i++ ) fmixbuf[i] = 0x20;
    mb::print_row(mb::bench_one("rn/frac-mixed1024", "cjson", w, w, [&] {
      for ( u32 i = 0; i < 1024; i++ ) {
        cjson::value v{};
        mb::sink_bool(cjson::__num::read_number(fmixbuf, w + 2, foffs[i], v) >= 0);
      }
    }));
  }

  // exponents + the bigint halfway rescue
  {
    tok t;
    t.set("1.5e300");
    rn_row("rn/exp+300", t);
    t.set("2.2250738585072014e-308");
    rn_row("rn/exp-308", t);
    t.set("1.00000000000000011102230246251565404236316680908203125");
    rn_row("rn/bigint-halfway", t);
  }

  // ------------------------------------------------------------------------ itoa
  {
    const u64 uvals[] = { 5, 42, 9999, 99999999, 9999999999999999ull, 18446744073709551615ull };
    const char *u_ops[] = { "itoa/u1", "itoa/u2", "itoa/u4", "itoa/u8", "itoa/u16", "itoa/u20" };
    for ( usize i = 0; i < 6; i++ ) {
      const u64 v = uvals[i];
      // bytes_per_op = digit count
      usize nd = 1;
      for ( u64 x = v; x >= 10; x /= 10 ) ++nd;
      mb::print_row(mb::bench_one(u_ops[i], "cjson", nd, nd, [&] {
        u8 out[32];
        u8 *e = cjson::__itoa::write_u64(out, v);
        mb::sink_size(usize(e - out));
      }));
    }

    static u64 umix[1024];
    rng rg;
    for ( u32 i = 0; i < 1024; i++ ) {
      // random magnitude: random bit width so lengths spread 1..20 digits
      const u32 bits = 1 + u32(rg.below(64));
      umix[i] = rg.next() >> (64 - bits);
    }
    mb::print_row(mb::bench_one("itoa/mixed1024", "cjson", 1024 * 10, 1024 * 10, [&] {
      u8 out[32];
      for ( u32 i = 0; i < 1024; i++ ) {
        u8 *e = cjson::__itoa::write_u64(out, umix[i]);
        mb::sink_size(usize(e - out));
      }
    }));
  }

  // ------------------------------------------------------------------------ dtoa
  {
    const f64 dv[] = { 0.0, 1.0, 0.1, 3.14159, 1e-7, 0.0001, 1.5e300, 5e-324, 1.7976931348623157e308, 123456789012345680000.0 };
    const char *d_ops[] = { "dtoa/0.0",  "dtoa/1.0",     "dtoa/0.1",    "dtoa/pi",     "dtoa/1e-7",
                            "dtoa/1e-4", "dtoa/1.5e300", "dtoa/denorm", "dtoa/dblmax", "dtoa/21digit" };
    for ( usize i = 0; i < 10; i++ ) {
      const f64 v = dv[i];
      mb::print_row(mb::bench_one(d_ops[i], "cjson", 24, 24, [&] {
        u8 out[40];
        u8 *e = cjson::__dtoa::write_f64(out, v);
        mb::sink_size(usize(e - out));
      }));
    }

    static f64 dmix[256];
    rng rg;
    for ( u32 i = 0; i < 256; i++ ) {
      u64 bits = rg.next();
      if ( (bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull ) bits ^= 1ull << 62;      // definite finite
      dmix[i] = __builtin_bit_cast(f64, bits);
    }
    mb::print_row(mb::bench_one("dtoa/random256", "cjson", 256 * 24, 256 * 24, [&] {
      u8 out[40];
      for ( u32 i = 0; i < 256; i++ ) {
        u8 *e = cjson::__dtoa::write_f64(out, dmix[i]);
        mb::sink_size(usize(e - out));
      }
    }));
  }

  return 0;
}
