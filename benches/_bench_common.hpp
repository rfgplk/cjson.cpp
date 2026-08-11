

#pragma once

#include "../../micron/external/bbench/bench.hpp"

#include <micron/io/console.hpp>
#include <micron/io/stdout.hpp>
#include <micron/linux/sys/sched.hpp>
#include <micron/std.hpp>

namespace mb
{

constexpr u32 K_MEASUREMENTS = 7;
constexpr u64 WARMUP_REPS = 4;
constexpr u64 TARGET_BYTES_PER_MEAS = 1ULL << 22;
constexpr u64 MIN_REPS = 32;
constexpr u64 MAX_REPS = 1ULL << 18;

using mem_events = bbench::event_group<bbench::hardware_cycles, bbench::hardware_instructions, bbench::branches, bbench::branch_misses>;

// row/header column layout (end-columns). Single source of truth for print_header(),
// print_row(), and the separator line — widths sized against benches/results/*.txt:
// max impl "simdjson-ondemand" (17 ch), max cyc/op "87968231.78" (11 ch, 26MB corpus),
// max size(B) 208923441 (sample/random.json, 9 ch).
//
// GB/s sits right after impl: it is the headline number every other json library
// publishes, and it is the one figure comparable across machines. cyc/op stays the A/B
// currency (see scripts/ab) because it is the one that does not move with clock.
//
// The impl column is 24 wide, up from 22: corpus_vs runs ten contenders and the old
// width glued "simdjson-ondemand" into cyc/op in the stored baselines.
constexpr u32 COL_SIZE = 10;
constexpr u32 COL_GAP = 13;
constexpr u32 COL_OP = 43;
constexpr u32 COL_IMPL = 67;
constexpr u32 COL_GBPS = 79;
constexpr u32 COL_CYC = 95;
constexpr u32 COL_INS = 111;
constexpr u32 COL_IPC = 123;
constexpr u32 COL_BMISS = 135;

struct row {
  const char *op;
  const char *impl;
  u64 size;
  f64 cyc_per_op;
  f64 ins_per_op;
  f64 ipc;
  f64 bmiss_rate;
  f64 gbps;
};

// prefix-match on "cjson" — shared by print_row's bold highlighting and print_group's
// cjson-vs-competitor partitioning.
[[gnu::always_inline]] inline bool
is_cjson_impl(const char *impl) noexcept
{
  constexpr char prefix[] = "cjson";
  for ( u32 i = 0; i < 5; i++ )
    if ( impl[i] != prefix[i] ) return false;
  return true;
}

[[gnu::always_inline]] inline bool
is_exact_cjson(const char *impl) noexcept
{
  return is_cjson_impl(impl) && impl[5] == '\0';
}

[[gnu::always_inline]] inline bool
leads(const row &a, const row *b) noexcept
{
  if ( !b ) return true;
  if ( a.gbps != b->gbps ) return a.gbps > b->gbps;
  return a.cyc_per_op < b->cyc_per_op;
}

struct fmt2 {
  u64 whole;
  u32 frac_x100;
};

[[gnu::always_inline]] inline fmt2
to_fmt2(f64 v)
{
  if ( v < 0 ) v = 0;
  u64 scaled = static_cast<u64>(v * 100.0 + 0.5);
  return { scaled / 100, static_cast<u32>(scaled % 100) };
}

struct line {
  char buf[256];
  u32 pos;

  constexpr line() noexcept : pos(0) { }

  void
  s(const char *p) noexcept
  {
    while ( *p ) buf[pos++] = *p++;
  }

  void
  pad_to(u32 end_col, u32 written) noexcept
  {
    const u32 want = end_col >= written ? end_col - written : 0;
    if ( want < pos )
      buf[pos++] = ' ';
    else
      while ( pos < want ) buf[pos++] = ' ';
  }

  void
  dashes_to(u32 end_col) noexcept
  {
    while ( pos < end_col ) buf[pos++] = '-';
  }

  void
  u_at(u64 v, u32 end_col) noexcept
  {
    char tmp[24];
    u32 n = 0;
    if ( v == 0 )
      tmp[n++] = '0';
    else {
      u64 vv = v;
      while ( vv ) {
        tmp[n++] = '0' + (vv % 10);
        vv /= 10;
      }
    }
    pad_to(end_col, n);
    while ( n ) buf[pos++] = tmp[--n];
  }

  void
  f2_at(fmt2 f, u32 end_col) noexcept
  {
    char tmp[24];
    u32 n = 0;
    u64 w = f.whole;
    if ( w == 0 )
      tmp[n++] = '0';
    else
      while ( w ) {
        tmp[n++] = '0' + (w % 10);
        w /= 10;
      }
    pad_to(end_col, n + 3);
    while ( n ) buf[pos++] = tmp[--n];
    buf[pos++] = '.';
    buf[pos++] = '0' + static_cast<char>(f.frac_x100 / 10);
    buf[pos++] = '0' + static_cast<char>(f.frac_x100 % 10);
  }

  void
  f2(fmt2 f) noexcept
  {
    char tmp[24];
    u32 n = 0;
    u64 w = f.whole;
    if ( w == 0 )
      tmp[n++] = '0';
    else
      while ( w ) {
        tmp[n++] = '0' + (w % 10);
        w /= 10;
      }
    while ( n ) buf[pos++] = tmp[--n];
    buf[pos++] = '.';
    buf[pos++] = '0' + static_cast<char>(f.frac_x100 / 10);
    buf[pos++] = '0' + static_cast<char>(f.frac_x100 % 10);
  }

  void
  fN(f64 v) noexcept
  {
    if ( v < 0 ) v = 0;
    u32 dec = 2;
    if ( v > 0.0 )
      for ( f64 t = v; t < 0.1 && dec < 6; t *= 10.0 ) ++dec;

    u64 unit = 1;
    for ( u32 i = 0; i < dec; i++ ) unit *= 10;
    const u64 scaled = static_cast<u64>(v * static_cast<f64>(unit) + 0.5);

    char tmp[24];
    u32 n = 0;
    u64 w = scaled / unit;
    if ( w == 0 )
      tmp[n++] = '0';
    else
      while ( w ) {
        tmp[n++] = '0' + (w % 10);
        w /= 10;
      }
    while ( n ) buf[pos++] = tmp[--n];

    char frac[8];
    u64 fr = scaled % unit;
    for ( u32 i = 0; i < dec; i++ ) {
      frac[i] = '0' + static_cast<char>(fr % 10);
      fr /= 10;
    }
    buf[pos++] = '.';
    for ( u32 i = dec; i > 0; i-- ) buf[pos++] = frac[i - 1];
  }

  void
  s_at(const char *p, u32 end_col) noexcept
  {
    u32 n = 0;
    while ( p[n] ) ++n;
    pad_to(end_col, n);
    while ( *p ) buf[pos++] = *p++;
  }

  void
  s_lj_at(const char *p, u32 end_col) noexcept
  {
    while ( *p ) buf[pos++] = *p++;
    while ( pos < end_col ) buf[pos++] = ' ';
  }

  const char *
  str() noexcept
  {
    buf[pos] = '\0';
    return buf;
  }
};

[[gnu::cold]] inline void
print_header()
{
  line h;
  h.s_at("size(B)", COL_SIZE);
  h.pad_to(COL_GAP, 0);
  h.s_lj_at("op", COL_OP);
  h.s_lj_at("impl", COL_IMPL);
  h.s_at("GB/s", COL_GBPS);
  h.s_at("cyc/op", COL_CYC);
  h.s_at("ins/op", COL_INS);
  h.s_at("IPC", COL_IPC);
  h.s_at("bmiss%", COL_BMISS);
  micron::io::println(h.str());
  line sep;
  sep.dashes_to(COL_BMISS);
  micron::io::println(sep.str());
}

[[gnu::cold]] inline void
print_row(const row &r)
{
  fmt2 gbs = to_fmt2(r.gbps);
  fmt2 cpo = to_fmt2(r.cyc_per_op);
  fmt2 ins = to_fmt2(r.ins_per_op);
  fmt2 ipc = to_fmt2(r.ipc);
  fmt2 bm = to_fmt2(r.bmiss_rate * 100.0);
  line ln;
  ln.u_at(r.size, COL_SIZE);
  ln.pad_to(COL_GAP, 0);
  ln.s_lj_at(r.op, COL_OP);
  ln.s_lj_at(r.impl, COL_IMPL);
  ln.f2_at(gbs, COL_GBPS);
  ln.f2_at(cpo, COL_CYC);
  ln.f2_at(ins, COL_INS);
  ln.f2_at(ipc, COL_IPC);
  ln.f2_at(bm, COL_BMISS);
  if ( is_cjson_impl(r.impl) && micron::posix::isatty(micron::stdout_fileno) )
    micron::io::println("\033[1m", ln.str(), "\033[0m");
  else
    micron::io::println(ln.str());
}

[[gnu::cold]] inline void
print_group(const row *rows, u32 n)
{
  for ( u32 i = 0; i < n; i++ ) print_row(rows[i]);

  const row *best_cjson = nullptr;
  const row *best_other = nullptr;
  for ( u32 i = 0; i < n; i++ ) {
    const row &r = rows[i];
    if ( is_cjson_impl(r.impl) ) {
      if ( leads(r, best_cjson) ) best_cjson = &r;
    } else {
      if ( leads(r, best_other) ) best_other = &r;
    }
  }

  if ( best_cjson && best_other ) {
    line ln;
    ln.s("  -> cjson");
    if ( !is_exact_cjson(best_cjson->impl) ) {
      ln.s(" (best: ");
      ln.s(best_cjson->impl);
      ln.s(")");
    }
    ln.s(" is ");
    if ( best_other->gbps > 0.0 )
      ln.fN(best_cjson->gbps / best_other->gbps);
    else
      ln.s("n/a");
    ln.s("x GB/s, ");
    if ( best_cjson->cyc_per_op > 0.0 )
      ln.fN(best_other->cyc_per_op / best_cjson->cyc_per_op);
    else
      ln.s("n/a");
    ln.s("x cyc/op vs best competitor (");
    ln.s(best_other->impl);
    ln.s(") -- ");
    ln.fN(best_cjson->gbps);
    ln.s(" vs ");
    ln.fN(best_other->gbps);
    ln.s(" GB/s");
    micron::io::println(ln.str());
  }
  micron::io::println("");
}

inline volatile u64 sink_u64 = 0;

[[gnu::always_inline]] inline void
clobber(const void *p) noexcept
{
  asm volatile("" : : "r"(p) : "memory");
}

[[gnu::always_inline]] inline void
sink_bool(bool b) noexcept
{
  sink_u64 += static_cast<u64>(b);
}

[[gnu::always_inline]] inline void
sink_size(usize v) noexcept
{
  sink_u64 += static_cast<u64>(v);
}

[[gnu::always_inline]] inline void
sink_ptr(const void *p) noexcept
{
  sink_u64 += reinterpret_cast<u64>(p);
}

inline f64
median_f64(f64 *xs, u32 n) noexcept
{
  for ( u32 i = 1; i < n; i++ ) {
    f64 key = xs[i];
    u32 j = i;
    while ( j > 0 && xs[j - 1] > key ) {
      xs[j] = xs[j - 1];
      --j;
    }
    xs[j] = key;
  }
  return xs[n / 2];
}

struct sample {
  u64 cyc;
  u64 inst;
  u64 br;
  u64 bm;
  f64 ns;
};

// wall clock is taken around the SAME loop as the counters rather than in a second
// pass: the clock reads sit outside the loop, so they cost nothing per rep, and cyc/op
// and GB/s then describe one identical run instead of two that drifted apart.
template<typename Fn>
[[gnu::noinline]] sample
measure_once(Fn &&fn, u64 reps) noexcept
{
  mem_events evs{ bbench::quiet{} };
  evs.open();
  bbench::time_clock_mono ck{};
  ck.begin();
  evs.begin();
  for ( u64 i = 0; i < reps; i++ ) fn();
  evs.end();
  ck.end();
  return { static_cast<u64>(evs.get<bbench::hardware_cycles>().retrieve()),
           static_cast<u64>(evs.get<bbench::hardware_instructions>().retrieve()), static_cast<u64>(evs.get<bbench::branches>().retrieve()),
           static_cast<u64>(evs.get<bbench::branch_misses>().retrieve()), ck.template elapsed<bbench::time_resolution::ns>() };
}

template<typename Fn>
row
bench_one(const char *op, const char *impl, u64 size, u64 bytes_per_op, Fn &&fn, u64 max_reps_override = MAX_REPS) noexcept
{
  for ( u64 i = 0; i < WARMUP_REPS; i++ ) fn();

  u64 reps = TARGET_BYTES_PER_MEAS / (bytes_per_op == 0 ? 1 : bytes_per_op);
  if ( reps < MIN_REPS ) reps = MIN_REPS;
  if ( reps > max_reps_override ) reps = max_reps_override;

  f64 cpo_samples[K_MEASUREMENTS];
  f64 ins_samples[K_MEASUREMENTS];
  f64 ipc_samples[K_MEASUREMENTS];
  f64 bm_samples[K_MEASUREMENTS];
  f64 gbps_samples[K_MEASUREMENTS];

  for ( u32 m = 0; m < K_MEASUREMENTS; m++ ) {
    sample s = measure_once(fn, reps);
    cpo_samples[m] = static_cast<f64>(s.cyc) / static_cast<f64>(reps);
    ins_samples[m] = static_cast<f64>(s.inst) / static_cast<f64>(reps);
    ipc_samples[m] = s.cyc > 0 ? static_cast<f64>(s.inst) / static_cast<f64>(s.cyc) : 0.0;
    bm_samples[m] = s.br > 0 ? static_cast<f64>(s.bm) / static_cast<f64>(s.br) : 0.0;
    // bytes per nanosecond IS GB/s (1 B/ns = 1e9 B/s). bytes_per_op is whatever the
    // caller counts per op, so a non-byte bench reads this as units/ns.
    const f64 ns_per_op = s.ns / static_cast<f64>(reps);
    gbps_samples[m] = ns_per_op > 0.0 ? static_cast<f64>(bytes_per_op) / ns_per_op : 0.0;
  }
  return row{ op,
              impl,
              size,
              median_f64(cpo_samples, K_MEASUREMENTS),
              median_f64(ins_samples, K_MEASUREMENTS),
              median_f64(ipc_samples, K_MEASUREMENTS),
              median_f64(bm_samples, K_MEASUREMENTS),
              median_f64(gbps_samples, K_MEASUREMENTS) };
}

inline void
pin_cpu0()
{
  micron::posix::cpu_set_t set;
  set.cpu_zero();
  set.cpu_set(0);
  micron::posix::sched_setaffinity(0, sizeof(set), set);
}

template<typename Fn>
f64
time_one_ns(Fn &&fn, u64 reps) noexcept
{
  f64 samples[K_MEASUREMENTS];
  for ( u32 m = 0; m < K_MEASUREMENTS; m++ ) {
    bbench::time_clock_mono ck{};
    ck.begin();
    for ( u64 i = 0; i < reps; i++ ) fn();
    ck.end();
    samples[m] = ck.template elapsed<bbench::time_resolution::ns>() / static_cast<f64>(reps);
  }
  return median_f64(samples, K_MEASUREMENTS);
}

inline u64
mbps(u64 bytes_per_op, f64 ns_per_op) noexcept
{
  if ( ns_per_op <= 0.0 ) return 0;
  return static_cast<u64>((static_cast<f64>(bytes_per_op) / ns_per_op) * 1000.0);
}

struct lat {
  u64 min_ns;
  u64 p50_ns;
  u64 p90_ns;
  u64 p99_ns;
  u64 max_ns;
};

inline constexpr u32 LAT_SAMPLES = 2000;
inline f64 lat_buf[LAT_SAMPLES];

template<typename Fn>
lat
latency_one(Fn &&fn, u32 samples = LAT_SAMPLES) noexcept
{
  if ( samples > LAT_SAMPLES ) samples = LAT_SAMPLES;
  for ( u32 i = 0; i < 16; i++ ) fn();
  for ( u32 i = 0; i < samples; i++ ) {
    bbench::time_clock_mono ck{};
    ck.begin();
    fn();
    ck.end();
    lat_buf[i] = ck.template elapsed<bbench::time_resolution::ns>();
  }

  for ( u32 i = 1; i < samples; i++ ) {
    const f64 key = lat_buf[i];
    u32 j = i;
    while ( j > 0 && lat_buf[j - 1] > key ) {
      lat_buf[j] = lat_buf[j - 1];
      --j;
    }
    lat_buf[j] = key;
  }
  auto at = [&](f64 q) -> u64 {
    u32 idx = (u32)(q * (f64)(samples - 1));
    return (u64)lat_buf[idx];
  };
  return lat{ (u64)lat_buf[0], at(0.50), at(0.90), at(0.99), (u64)lat_buf[samples - 1] };
}

struct soak_result {
  f64 mbps_decile[10];
  f64 mbps_median;
  f64 drift_pct;
  u64 total_reps;
};

inline constexpr u32 SOAK_MAX_SLICES = 4096;
inline f64 soak_slice_mbps[SOAK_MAX_SLICES];

template<typename Fn>
soak_result
soak_one(Fn &&fn, u64 bytes_per_op, u64 target_ms = 5000) noexcept
{
  for ( u64 i = 0; i < WARMUP_REPS; i++ ) fn();

  bbench::time_clock_mono cal{};
  cal.begin();
  fn();
  cal.end();
  f64 ns_call = cal.template elapsed<bbench::time_resolution::ns>();
  if ( ns_call < 1.0 ) ns_call = 1.0;
  u64 slice_reps = static_cast<u64>(20.0e6 / ns_call);
  if ( slice_reps < 1 ) slice_reps = 1;

  u32 slices = 0;
  u64 total_reps = 0;
  f64 elapsed_ns = 0.0;
  const f64 target_ns = static_cast<f64>(target_ms) * 1.0e6;
  while ( elapsed_ns < target_ns && slices < SOAK_MAX_SLICES ) {
    bbench::time_clock_mono ck{};
    ck.begin();
    for ( u64 i = 0; i < slice_reps; i++ ) fn();
    ck.end();
    const f64 ns = ck.template elapsed<bbench::time_resolution::ns>();
    soak_slice_mbps[slices++] = ns > 0.0 ? (static_cast<f64>(bytes_per_op * slice_reps) / ns) * 1000.0 : 0.0;
    total_reps += slice_reps;
    elapsed_ns += ns;
  }

  soak_result r{};
  r.total_reps = total_reps;
  const u32 per = slices >= 10 ? slices / 10 : 1;
  for ( u32 d = 0; d < 10; d++ ) {
    const u32 lo = d * per;
    u32 hi = (d == 9) ? slices : (d + 1) * per;
    if ( lo >= slices ) {
      r.mbps_decile[d] = 0.0;
      continue;
    }
    if ( hi > slices ) hi = slices;
    f64 sum = 0.0;
    for ( u32 i = lo; i < hi; i++ ) sum += soak_slice_mbps[i];
    r.mbps_decile[d] = sum / static_cast<f64>(hi - lo);
  }
  r.mbps_median = median_f64(soak_slice_mbps, slices ? slices : 1);
  r.drift_pct = r.mbps_decile[0] > 0.0 ? (r.mbps_decile[0] - r.mbps_decile[9]) / r.mbps_decile[0] * 100.0 : 0.0;
  return r;
}

[[gnu::cold]] inline void
print_soak_row(const char *op, const char *impl, const soak_result &r)
{
  line ln;
  ln.s_lj_at(op, 28);
  ln.s_lj_at(impl, 44);
  ln.u_at(static_cast<u64>(r.mbps_median), 54);
  ln.u_at(static_cast<u64>(r.mbps_decile[0]), 64);
  ln.u_at(static_cast<u64>(r.mbps_decile[9]), 74);
  ln.f2_at(to_fmt2(r.drift_pct < 0 ? -r.drift_pct : r.drift_pct), 84);
  ln.s(r.drift_pct < 0 ? " (rising)" : "");
  micron::io::println(ln.str());
}

[[gnu::cold]] inline void
print_soak_header()
{
  line h;
  h.s_lj_at("soak op", 28);
  h.s_lj_at("impl", 44);
  h.s_at("MB/s", 54);
  h.s_at("d0", 64);
  h.s_at("d9", 74);
  h.s_at("drift%", 84);
  micron::io::println(h.str());
  micron::io::println("------------------------------------------------------------------------------------");
}

[[gnu::cold]] inline void
print_preamble(const char *title)
{
  micron::io::println("=== ", title, " ===");
  micron::io::println("warmup ", WARMUP_REPS, " reps; ", K_MEASUREMENTS, " median samples per cell");
  micron::io::println("perf events: cycles + instructions + branches + branch-misses");
  micron::io::println("");
}

[[gnu::cold]] inline void
print_epilogue()
{
  micron::io::println("");
  micron::io::println("=== done ===");
  micron::io::println("(anti-DCE sink: ", sink_u64, ")");
}

}      // namespace mb
