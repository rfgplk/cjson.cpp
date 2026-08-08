//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// shared printing helpers for the examples. cjson hands strings back as strv
// ({ptr,len} into someone else's buffer, never nul-terminated), so printing one means
// copying it — that is a property of the library worth seeing in the examples rather
// than hiding behind a formatter.
#pragma once

#include "../src/cjson/cjson.hpp"

#include <micron/io/echo.hpp>
#include <micron/string/strings.hpp>
#include <micron/types.hpp>

namespace ex
{

inline micron::string
str(cjson::strv v)
{
  micron::string s{};
  s.append(v.ptr, v.len);
  return s;
}

inline void
show(const char *label, cjson::strv v)
{
  micron::io::echo(label, str(v).c_str());
}

inline void
head(const char *title)
{
  // micron::io::echo("^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^");
  micron::io::echo("%%%%%%%% ", title, " %%%%%%%%");
}

};      // namespace ex
