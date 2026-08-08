//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

#include "../src/cjson/cjson.hpp"

#include <snowball/snowball.hpp>

#include <micron/string/strings.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

// the container seam must stay disjoint from the raw-view overload set: raw_slice
// deliberately fails byte_source, and every seam type keeps its size semantics

static_assert(!cjson::byte_source<cjson::bytes>);
static_assert(!cjson::byte_source<cjson::wbytes>);
static_assert(!cjson::byte_source<cjson::strv>);
static_assert(cjson::byte_source<micron::vector<u8>>);
static_assert(cjson::byte_source<micron::vector<u32>>);
static_assert(cjson::byte_source<micron::string>);

// punning twins agree between runtime and constant evaluation
namespace
{

constexpr u8 k_probe[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };

static_assert(cjson::__load16(k_probe) == 0x2211u);
static_assert(cjson::__load32(k_probe) == 0x44332211u);
static_assert(cjson::__load64(k_probe) == 0x8877665544332211ull);

constexpr bool
store_roundtrip() noexcept
{
  u8 buf[16]{};
  cjson::__store16(buf, 0xbeefu);
  cjson::__store32(buf + 2, 0xdeadbeefu);
  cjson::__store64(buf + 6, 0x0123456789abcdefull);
  return cjson::__load16(buf) == 0xbeefu and cjson::__load32(buf + 2) == 0xdeadbeefu and cjson::__load64(buf + 6) == 0x0123456789abcdefull;
}

static_assert(store_roundtrip());

constexpr bool
move16_forward_overlap() noexcept
{
  u8 buf[24]{};
  for ( u8 i = 0; i < 24; i++ ) buf[i] = i;
  cjson::__move16(buf + 1, buf + 5);      // dst < src, overlapping
  bool ok = true;
  for ( u8 i = 0; i < 16; i++ ) ok = ok and (buf[1 + i] == u8(5 + i));
  return ok;
}

static_assert(move16_forward_overlap());

};      // namespace

int
main()
{
  {
    sb::test_case("as_bytes preserves byte counts across element widths");
    micron::vector<u8> v8;
    for ( u8 i = 0; i < 7; i++ ) v8.push_back(i);
    micron::vector<u32> v32;
    for ( u32 i = 0; i < 5; i++ ) v32.push_back(i);
    sb::require(cjson::as_bytes(v8).size(), static_cast<usize>(7));
    sb::require(cjson::as_bytes(v32).size(), static_cast<usize>(20));
    sb::require_true(cjson::as_bytes(v8).ptr == v8.cbegin());
    sb::end_test_case();
  }
  {
    sb::test_case("as_wbytes views mutable storage in place");
    micron::vector<u8> v;
    for ( u8 i = 0; i < 4; i++ ) v.push_back(0);
    auto w = cjson::as_wbytes(v);
    w.ptr[2] = 0x7f;
    sb::require(static_cast<u32>(v[2]), static_cast<u32>(0x7f));
    sb::end_test_case();
  }
  {
    sb::test_case("error currency round-trips through the negative-max_t leaf convention");
    sb::require_true(cjson::as_error(cjson::fail(cjson::error::bad_utf8)) == cjson::error::bad_utf8);
    sb::require_true(cjson::as_error(42) == cjson::error::ok);
    sb::require_true(cjson::as_error(0) == cjson::error::ok);
    sb::end_test_case();
  }
  {
    sb::test_case("result carries both alternatives and is_first means success");
    cjson::result<u32> good{ micron::tag<u32>{}, 7u };
    cjson::result<u32> bad{ micron::tag<cjson::error>{}, cjson::error::oom };
    sb::require_true(good.is_first());
    sb::require_true(bad.is_second());
    sb::require_true(good.has_value() and bad.has_value());      // the documented trap: true for BOTH
    sb::require(good.cast<u32>(), 7u);
    sb::require_true(bad.cast<cjson::error>() == cjson::error::oom);
    sb::end_test_case();
  }
  {
    sb::test_case("runtime loads agree with the comptime shift-or twins");
    u8 buf[8] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88 };
    sb::require(cjson::__load16(buf), 0x2211u);
    sb::require(cjson::__load32(buf), 0x44332211u);
    sb::require_true(cjson::__load64(buf) == 0x8877665544332211ull);
    sb::end_test_case();
  }
  return 1;
}
