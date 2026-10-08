//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include "../../src/cjson/fixed.hpp"
#include <micron/sec/seccomp.hpp>
#include <micron/linux/process/fork.hpp>
#include <snowball/snowball.hpp>

namespace
{
template<usize Bytes = 512, usize Nodes = 64> struct storage {
  u32 idx[Bytes + 10]{};
  u8 pool[Bytes + cjson::padding]{};
  cjson::value vals[Nodes]{};
  cjson::source_range ranges[Nodes]{};
  constexpr cjson::fixed_scratch view()
  { return {{idx, Bytes + 10}, {pool, Bytes + cjson::padding}, {vals, Nodes}, {ranges, Nodes}}; }
};
constexpr cjson::bytes as_bytes(const char *p)
{ return {reinterpret_cast<const u8 *>(p), micron::strlen(p)}; }

constexpr bool constant_contract()
{
  storage<32, 8> sc;
  cjson::doc doc;
  const u8 text[]{'[', '0', ',', '"', 'x', '"', ']'};
  if ( cjson::parse_into(doc, {text, 7}, sc.view()) != 3 ) return false;
  if ( cjson::get_len(*doc.root().at(1).__raw()) != 1 or sc.ranges[0].end != 7 ) return false;
  u8 out[4]{}; const u8 newline[]{10};
  return cjson::quote_into({newline, 1}, {out, 4}) == 4 and out[1] == '\\' and out[2] == 'n';
}
static_assert(constant_contract());
}

int
main()
{
  sb::test_case("fixed parse retains exact token ranges and decoded UTF-16 without allocation");
  storage<> sc; cjson::doc doc;
  const char *input = R"( {"n":18446744073709551615,"s":"\ud83d\ude00\u0000","a":[false,{},-1.20e+02]} )";
  cjson::opts opts{}; opts.numbers_as_raw = true;
  sb::require_true(cjson::parse_into(doc, as_bytes(input), sc.view(), opts) == 10 && doc.borrowed());
  sb::require_true(doc.root()["n"].raw_str().len == 20 && doc.root()["s"].str_or().len == 5);
  sb::require_true(sc.ranges[0].begin == 1 && input[sc.ranges[0].end - 1] == '}');
  for ( usize i = 0; i < doc.size(); ++i ) {
    auto range = sc.ranges[i];
    sb::require_true(range.begin < range.end && range.end <= micron::strlen(input));
    cjson::scratch dynamic;
    auto parsed = cjson::parse(cjson::bytes{reinterpret_cast<const u8 *>(input + range.begin), range.end - range.begin}, opts, dynamic);
    sb::require_true(parsed.is_first() && parsed.cast<cjson::doc>().root().type() == cjson::get_kind(sc.vals[i]));
  }
  sb::end_test_case();

  sb::test_case("node, index, pool, depth and output limits fail without partial publication");
  const char *array = "[1,2,3]";
  for ( unsigned mode = 0; mode < 5; ++mode ) {
    sb::require_true(cjson::parse_into(doc, as_bytes(array), sc.view(), opts) == 4);
    auto short_sc = sc.view();
    if ( mode == 0 ) short_sc.indexes.len = 16;
    if ( mode == 1 ) short_sc.pool.len = 70;
    if ( mode == 2 ) short_sc.values.len = 3;
    if ( mode == 3 ) short_sc.ranges.len = 3;
    sb::require_true(cjson::parse_into(doc, as_bytes(array), short_sc, opts, mode == 4 ? 0 : 32) < 0 && !doc.alive());
  }
  u8 output[32]; for ( auto &b : output ) b = 0xa5;
  const u8 invalid[]{0xc0, 0x80}, control[]{0, 10, '"', '\\'};
  sb::require_true(cjson::quote_into({invalid, 2}, {output, 32}) == cjson::fail(cjson::error::bad_utf8));
  sb::require_true(cjson::quote_into({control, 4}, {output, 13}) == cjson::fail(cjson::error::short_output));
  for ( auto b : output ) sb::require_true(b == 0xa5);
  sb::require_true(cjson::quote_into({control, 4}, {output, 14}) == 14 && output[14] == 0xa5);
  sb::require_true(cjson::parse_into(doc, {output, 14}, sc.view()) == 1 && doc.root().str_or().len == 4);
  sb::end_test_case();

  sb::test_case("fixed parser rejects complete invalid grammar and has reusable failure state");
  for ( const char *bad : {"[1,]", "{\"a\":1,}", "01", "-", "1e+", "[true false]", "true null", "\"\\ud800\"",
                           "\"\\udc00\"", "{\"a\":}", "\"\xff\"", "   "} ) {
    sb::require_true(cjson::parse_into(doc, as_bytes(bad), sc.view(), opts) < 0 && !doc.alive());
    sb::require_true(cjson::parse_into(doc, as_bytes("[0]"), sc.view(), opts) == 2);
  }
  sb::require_true(cjson::parse_into(doc, as_bytes("[[[0]]]"), sc.view(), opts, 2) == cjson::fail(cjson::error::depth_exceeded));
  sb::end_test_case();

  sb::test_case("large escaped strings need one value slot and parse/write survive denied allocation syscalls");
  const int pid = micron::try_fork(); sb::require_true(pid >= 0);
  if ( !pid ) {
    micron::sec::seccomp::filter_builder<64> filter;
    filter.require_native_arch().deny_errno(SYS_mmap, micron::error::out_of_memory)
      .deny_errno(SYS_brk, micron::error::out_of_memory).default_allow();
    sb::require_true(micron::sec::seccomp::load(filter) == 0);
    storage<512, 1> tiny;
    u8 source[386]{}; source[0] = '"'; source[385] = '"';
    for ( usize i = 1; i < 385; i += 6 ) { source[i] = '\\'; source[i+1] = 'u'; source[i+2] = '0';
      source[i+3] = '0'; source[i+4] = '6'; source[i+5] = '1'; }
    cjson::doc value;
    sb::require_true(cjson::parse_into(value, {source, 386}, tiny.view()) == 1 && value.root().str_or().len == 64);
    u8 serialized[1024]{};
    sb::require_true(cjson::write_into(value, {serialized, 1024}) == 66);
    value.release();
    sb::require_true(tiny.pool[1] == 'a');
    micron::sys_group_exit(1);
  }
  int result = 0;
  sb::require_true(micron::waitpid(pid, &result, 0) == pid && micron::wifexited(result) && micron::wexitstatus(result) == 1);
  sb::end_test_case();
  sb::test_case("fixed writer closes grammar, preserves exact integers and never publishes failed output");
  const char *expected = R"({"a":[18446744073709551615,true,null,"x\n"]})";
  for ( usize cap = 0; cap < 96; ++cap ) {
    u8 output[97]; for ( auto &b : output ) b = 0xa5;
    cjson::fixed_writer<2> writer({output, cap});
    writer.object(); writer.key(as_bytes("a")); writer.array(); writer.number(~u64(0));
    writer.boolean(true); writer.null(); writer.string(as_bytes("x\n")); writer.end(); writer.end();
    const auto n = writer.finish();
    if ( cap < micron::strlen(expected) ) sb::require_true(n < 0);
    else sb::require_true(n == max_t(micron::strlen(expected)) && micron::memcmp<u8>(output, reinterpret_cast<const u8 *>(expected), usize(n)) == 0);
    for ( usize i = cap; i < 97; ++i ) sb::require_true(output[i] == 0xa5);
  }
  {
    u8 output[32]; cjson::fixed_writer<1> writer({output, 32});
    writer.object(); writer.key(as_bytes("a")); writer.end();
    sb::require_true(writer.finish() == cjson::fail(cjson::error::bad_syntax));
    writer.number(1); sb::require_true(writer.finish() < 0);
  }
  {
    u8 output[32]; cjson::fixed_writer<1> writer({output, 32});
    writer.array(); writer.array(); sb::require_true(writer.finish() == cjson::fail(cjson::error::depth_exceeded));
  }
  sb::end_test_case();
  return 1;
}
