//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// freestanding compile+link gate (-k): including the umbrella and touching the codec
// surface must need no libc and no stl. building this file IS the test; the binary is
// a no-op. not a snowball tu — declared in build.duck, not tests.duck

#include "../src/cjson/cjson.hpp"

namespace
{

constexpr u8 k_doc[] = { '{', '}' };

constexpr bool
gate() noexcept
{
  // exercise the constexpr surface so nothing is elided
  bool ok = cjson::char_class[u8('{')] == (cjson::c_strsafe | cjson::c_struct);
  ok = ok and cjson::is_open(k_doc[0]) and !cjson::is_open(k_doc[1]);
  ok = ok and cjson::as_error(cjson::fail(cjson::error::oom)) == cjson::error::oom;
  u8 buf[8]{};
  cjson::__store64(buf, 0x2020202020202000ull);      // the text-pool pad shape: nul then spaces
  ok = ok and buf[0] == 0 and !cjson::is_num_end(buf[0]);
  ok = ok and cjson::is_space(buf[1]) and cjson::is_num_end(buf[1]);
  return ok;
}

static_assert(gate());

};      // namespace

int
main()
{
  // exit 1 == pass under the duck test contract; as a -k build target the code is unused
  volatile bool ok = gate();
  return ok ? 1 : 2;
}
