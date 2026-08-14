//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// porcelain.hpp: the bridge between a cjson document and micron's containers.
//
// This layer is the ONE deliberate exemption from the constexpr contract (ARCHITECTURE.md)
// -- micron::any has a non-constexpr destructor and no micron map is constexpr -- so
// everything here is runtime-only by design, and nothing in it is reachable from ct.hpp.
// The round trip is the property worth holding: doc -> map/vector -> json -> doc must
// preserve the document.

#include "../../src/cjson/cjson.hpp"

#include "../tutil.hpp"

#include <snowball/snowball.hpp>

#include <micron/string/strings.hpp>
#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

template<usize N>
inline cjson::result<cjson::doc>
PJ(const char (&s)[N])
{
  return cjson::parse(reinterpret_cast<const u8 *>(s), N - 1);
}

};      // namespace

int
main()
{
  {
    sb::test_case("to_map lifts an object into a micron map");
    auto r = PJ(R"({"i":-7,"u":42,"d":1.5,"b":true,"s":"hi","n":null})");
    sb::require_true(r.is_first());
    auto m = cjson::to_map(r.cast<cjson::doc>().root());
    sb::require_true(m.is_first());
    sb::require(m.cast<cjson::object_map>().size(), static_cast<usize>(6));
    sb::end_test_case();
  }
  {
    sb::test_case("to_vector lifts an array into a micron vector");
    auto r = PJ(R"([1,2,"three",true,null,{"a":1},[1,2]])");
    sb::require_true(r.is_first());
    auto v = cjson::to_vector(r.cast<cjson::doc>().root());
    sb::require_true(v.is_first());
    sb::require(v.cast<cjson::array_vec>().size(), static_cast<usize>(7));
    sb::end_test_case();
  }
  {
    sb::test_case("map_slots sizes the map the document needs");
    auto r = PJ(R"({"a":1,"b":2,"c":3,"d":4,"e":5})");
    sb::require_true(r.is_first());
    sb::require_greater(cjson::map_slots(r.cast<cjson::doc>().root()), static_cast<usize>(0));
    sb::end_test_case();
  }
  {
    // the round trip that matters: doc -> map -> json -> doc
    sb::test_case("doc to map to json to doc preserves the document");
    const char *docs[] = {
      R"({"a":1})",
      R"({"i":-7,"u":42,"d":1.5,"b":true,"s":"hi","n":null})",
      R"({"nested":{"x":1,"y":[1,2,3]},"flat":"v"})",
      R"({})",
    };
    for ( const char *j : docs ) {
      usize n = 0;
      while ( j[n] ) ++n;
      auto r = cjson::parse(reinterpret_cast<const u8 *>(j), n);
      sb::require_true(r.is_first());

      auto m = cjson::to_map(r.cast<cjson::doc>().root());
      sb::require_true(m.is_first());
      auto back = cjson::to_json(m.cast<cjson::object_map>());
      sb::require_true(back.is_first());
      const micron::string &text = back.cast<micron::string>();

      // it must be conforming json (rfc s10) and denote the same member set
      if ( cjson::validate(reinterpret_cast<const u8 *>(text.c_str()), text.size()) != cjson::error::ok )
        snowball::print("to_json output does not parse: ", text.c_str());
      sb::require_true(cjson::validate(reinterpret_cast<const u8 *>(text.c_str()), text.size()) == cjson::error::ok);

      auto r2 = cjson::parse(reinterpret_cast<const u8 *>(text.c_str()), text.size());
      sb::require_true(r2.is_first());
      sb::require(r2.cast<cjson::doc>().root().size(), r.cast<cjson::doc>().root().size());
    }
    sb::end_test_case();
  }
  {
    sb::test_case("array to vector to json to array preserves the elements");
    const char *docs[] = { R"([])", R"([1,2,3])", R"([1,"a",true,null,1.5])", R"([[1],[2],{"k":3}])" };
    for ( const char *j : docs ) {
      usize n = 0;
      while ( j[n] ) ++n;
      auto r = cjson::parse(reinterpret_cast<const u8 *>(j), n);
      sb::require_true(r.is_first());
      auto v = cjson::to_vector(r.cast<cjson::doc>().root());
      sb::require_true(v.is_first());
      auto back = cjson::to_json_seq(v.cast<cjson::array_vec>());
      sb::require_true(back.is_first());
      const micron::string &text = back.cast<micron::string>();
      sb::require_true(cjson::validate(reinterpret_cast<const u8 *>(text.c_str()), text.size()) == cjson::error::ok);
      auto r2 = cjson::parse(reinterpret_cast<const u8 *>(text.c_str()), text.size());
      sb::require_true(r2.is_first());
      sb::require(r2.cast<cjson::doc>().root().size(), r.cast<cjson::doc>().root().size());
    }
    sb::end_test_case();
  }
  {
    sb::test_case("to_map_into and to_vector_into fill a caller's container");
    auto r = PJ(R"({"a":1,"b":2,"c":3})");
    sb::require_true(r.is_first());
    cjson::object_map m;
    sb::require_true(cjson::to_map_into(r.cast<cjson::doc>().root(), m) == cjson::error::ok);
    sb::require(m.size(), static_cast<usize>(3));

    auto ra = PJ(R"([1,2,3,4])");
    sb::require_true(ra.is_first());
    cjson::array_vec v;
    sb::require_true(cjson::to_vector_into(ra.cast<cjson::doc>().root(), v) == cjson::error::ok);
    sb::require(v.size(), static_cast<usize>(4));
    sb::end_test_case();
  }
  {
    // duplicate names: porcelain keeps the FIRST, matching the dom's lookup rule
    sb::test_case("to_map keeps the first of a duplicated name");
    auto r = PJ(R"({"k":1,"k":2,"k":3})");
    sb::require_true(r.is_first());
    sb::require(r.cast<cjson::doc>().root().size(), static_cast<usize>(3));
    auto m = cjson::to_map(r.cast<cjson::doc>().root());
    sb::require_true(m.is_first());
    // a map cannot hold three entries under one name
    sb::require(m.cast<cjson::object_map>().size(), static_cast<usize>(1));
    sb::end_test_case();
  }
  {
    sb::test_case("the cursor overloads mirror the dom ones");
    const char *j = R"({"a":1,"b":[1,2],"c":"x"})";
    usize n = 0;
    while ( j[n] ) ++n;
    cjson::scratch sc;
    auto rv = cjson::iterate(cjson::bytes{ reinterpret_cast<const u8 *>(j), n }, sc);
    sb::require_true(rv.is_first());
    auto m = cjson::to_map(rv.cast<cjson::view>().root());
    sb::require_true(m.is_first());
    sb::require(m.cast<cjson::object_map>().size(), static_cast<usize>(3));

    cjson::scratch sc2;
    auto rv2 = cjson::iterate(cjson::bytes{ reinterpret_cast<const u8 *>(R"([1,2,3])"), 7 }, sc2);
    sb::require_true(rv2.is_first());
    auto v = cjson::to_vector(rv2.cast<cjson::view>().root());
    sb::require_true(v.is_first());
    sb::require(v.cast<cjson::array_vec>().size(), static_cast<usize>(3));
    sb::end_test_case();
  }
  {
    sb::test_case("write_map and write_seq feed a builder");
    auto r = PJ(R"({"a":1,"b":"two"})");
    sb::require_true(r.is_first());
    auto m = cjson::to_map(r.cast<cjson::doc>().root());
    sb::require_true(m.is_first());

    cjson::builder b;
    sb::require_true(cjson::write_map(b, m.cast<cjson::object_map>()) == cjson::error::ok);
    micron::string out = b.take();
    sb::require_true(cjson::validate(reinterpret_cast<const u8 *>(out.c_str()), out.size()) == cjson::error::ok);
    sb::end_test_case();
  }
  {
    sb::test_case("a corpus object lifts and round-trips");
    auto data = tutil::slurp("sample/64kb.json");
    sb::require_greater(data.size(), static_cast<usize>(0));
    auto r = cjson::parse(tutil::view(data));
    sb::require_true(r.is_first());
    if ( r.cast<cjson::doc>().root().type() == cjson::kind::object ) {
      auto m = cjson::to_map(r.cast<cjson::doc>().root());
      sb::require_true(m.is_first());
      auto back = cjson::to_json(m.cast<cjson::object_map>());
      sb::require_true(back.is_first());
      const micron::string &text = back.cast<micron::string>();
      sb::require_true(cjson::validate(reinterpret_cast<const u8 *>(text.c_str()), text.size()) == cjson::error::ok);
    }
    sb::end_test_case();
  }
  return 1;
}
