//  Copyright (c) 2025- David Lucius Severus
//
//  Distributed under the Boost Software License, Version 1.0.
//  See accompanying file LICENSE or copy at
//  http://www.boost.org/LICENSE_1_0.txt

// rfc 8259 s2 (grammar + the ws set), s3 (values, lowercase literal names), s4 (objects),
// s5 (arrays), read as a specification and turned into assertions. every case runs
// through all six strict entry points and they must agree -- see rfc_cases.hpp.
//
// also carries the two regression tables for defects this suite was written to catch:
// the nul hole (F1, patched in tables.hpp) and the on-demand grammar boundary (F2, made
// opt-in via opts::check_grammar).

#include "rfc_cases.hpp"

#include <snowball/snowball.hpp>

#include <micron/types.hpp>
#include <micron/vector.hpp>

namespace
{

using rfc::K;
using rfc::KD;
using rfc::verdict;

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// s2: JSON-text = ws value ws

const rfc::kase k_text[] = {
  K("{}", verdict::accept, "s2 JSON-text = ws value ws"),
  K(" {}", verdict::accept, "s2 leading ws"),
  K("{} ", verdict::accept, "s2 trailing ws"),
  K("\t\n\r {} \r\n\t", verdict::accept, "s2 ws is all four bytes, both sides"),
  K("\t\n\r ", verdict::reject, "s2 ws alone is not a value"),
  K("", verdict::reject, "s2 a JSON-text contains one value"),
};

// s2: ws = %x20 / %x09 / %x0A / %x0D -- and NOTHING else. each of these is a byte or
// sequence some implementation somewhere treats as space; all must reject.
const rfc::kase k_notws[] = {
  K("\x0b{}", verdict::reject, "s2 0x0b vertical tab is not ws"),
  K("{}\x0b", verdict::reject, "s2 0x0b after the root is not ws"),
  K("\x0c{}", verdict::reject, "s2 0x0c form feed is not ws (also an op-classify false positive)"),
  K("{}\x0c", verdict::reject, "s2 0x0c after the root is not ws"),
  K("\x1a{}", verdict::reject, "s2 0x1a is not ws (the other op-classify false positive)"),
  K("\xc2\xa0{}", verdict::reject, "s2 u+00a0 nbsp is not ws"),
  K("\xe2\x80\xa8{}", verdict::reject, "s2 u+2028 line separator is not ws (s12 calls it out)"),
  K("\xe2\x80\xa9{}", verdict::reject, "s2 u+2029 paragraph separator is not ws"),
  K("\xe3\x80\x80{}", verdict::reject, "s2 u+3000 ideographic space is not ws"),
  K("\x0c", verdict::reject, "s2 0x0c alone is not a value"),
};

// s8.1: "implementations that parse JSON texts MAY ignore the presence of a byte order
// mark rather than treating it as an error." cjson does not ignore it -- discretionary.
const rfc::kase k_bom[] = {
  KD("\xef\xbb\xbf{}", verdict::reject, "s8.1 bom is a MAY; cjson rejects it"),
  KD("\xef\xbb\xbf[1]", verdict::reject, "s8.1 bom before an array"),
  K("{\"\xef\xbb\xbf\":1}", verdict::accept, "s8.1 u+feff INSIDE a string is ordinary content"),
};

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// s3: values. "The literal names MUST be lowercase. No other literal names are allowed."

const rfc::kase k_values[] = {
  K("null", verdict::accept, "s3 null"),
  K("true", verdict::accept, "s3 true"),
  K("false", verdict::accept, "s3 false"),
  K("\"Hello world!\"", verdict::accept, "s13 a string is a JSON-text"),
  K("42", verdict::accept, "s13 a number is a JSON-text"),
  K("[]", verdict::accept, "s5 empty array"),
  K("{}", verdict::accept, "s4 empty object"),

  K("True", verdict::reject, "s3 literal names MUST be lowercase"),
  K("TRUE", verdict::reject, "s3 literal names MUST be lowercase"),
  K("False", verdict::reject, "s3 literal names MUST be lowercase"),
  K("FALSE", verdict::reject, "s3 literal names MUST be lowercase"),
  K("Null", verdict::reject, "s3 literal names MUST be lowercase"),
  K("NULL", verdict::reject, "s3 literal names MUST be lowercase"),
  K("None", verdict::reject, "s3 no other literal names are allowed"),
  K("nil", verdict::reject, "s3 no other literal names are allowed"),
  K("undefined", verdict::reject, "s3 no other literal names are allowed"),

  K("tru", verdict::reject, "s3 truncated literal"),
  K("nul", verdict::reject, "s3 truncated literal"),
  K("fals", verdict::reject, "s3 truncated literal"),
  K("t", verdict::reject, "s3 truncated literal"),
  K("truex", verdict::reject, "s3 literal with a trailing byte"),
  K("nulll", verdict::reject, "s3 literal with a trailing byte"),
  K("falsey", verdict::reject, "s3 literal with a trailing byte"),
  K("[truex]", verdict::reject, "s3 literal run-on inside an array"),
};

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// s4: object = begin-object [ member *( value-separator member ) ] end-object
//     member = string name-separator value

const rfc::kase k_objects[] = {
  K("{\"a\":1}", verdict::accept, "s4 one member"),
  K("{\"a\":1,\"b\":2}", verdict::accept, "s4 two members"),
  K("{ \"a\" : 1 }", verdict::accept, "s4 ws around the separators"),
  K("{\"\":1}", verdict::accept, "s4 the empty string is a legal name"),
  K("{\"a\":{\"b\":{\"c\":[]}}}", verdict::accept, "s4 nesting"),

  K("{,}", verdict::reject, "s4 a lone separator is not a member"),
  K("{\"a\"}", verdict::reject, "s4 member requires name-separator value"),
  K("{\"a\":}", verdict::reject, "s4 member requires a value"),
  K("{:1}", verdict::reject, "s4 member requires a name"),
  K("{\"a\":1,}", verdict::reject, "s4 no trailing value-separator"),
  K("{,\"a\":1}", verdict::reject, "s4 no leading value-separator"),
  K("{\"a\" 1}", verdict::reject, "s4 name-separator is not optional"),
  K("{\"a\"::1}", verdict::reject, "s4 one name-separator per member"),
  K("{\"a\":1,,\"b\":2}", verdict::reject, "s4 one value-separator between members"),
  K("{'a':1}", verdict::reject, "s4 a name is a string; s7 strings use quotation marks"),
  K("{a:1}", verdict::reject, "s4 an unquoted name is not a string"),
  K("{1:2}", verdict::reject, "s4 a name MUST be a string"),
  K("{\"a\":1}}", verdict::reject, "s2 trailing end-object"),
  K("{\"a\":1", verdict::reject, "s4 unterminated object"),
  K("{", verdict::reject, "s4 unterminated object"),
  K("}", verdict::reject, "s4 a lone end-object is not a value"),
  K("{\"a\":1]", verdict::reject, "s4/s5 mismatched closer"),
};

// s4: "The names within an object SHOULD be unique." Behaviour on duplicates is left
// open, so cjson's choice (keep all, first match wins) is discretionary.
const rfc::kase k_dupes[] = {
  KD("{\"k\":1,\"k\":2}", verdict::accept, "s4 duplicate names are a SHOULD; cjson accepts"),
  KD("{\"k\":1,\"k\":2,\"k\":3}", verdict::accept, "s4 duplicate names, three ways"),
};

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// s5: array = begin-array [ value *( value-separator value ) ] end-array

const rfc::kase k_arrays[] = {
  K("[1]", verdict::accept, "s5 one element"),
  K("[1,2,3]", verdict::accept, "s5 several elements"),
  K("[ 1 , 2 ]", verdict::accept, "s5 ws around the separators"),
  K("[null,true,\"s\",1,{},[]]", verdict::accept, "s5 no requirement that values share a type"),
  K("[[[[]]]]", verdict::accept, "s5 nesting"),

  K("[,]", verdict::reject, "s5 a lone separator is not a value"),
  K("[1,]", verdict::reject, "s5 no trailing value-separator"),
  K("[,1]", verdict::reject, "s5 no leading value-separator"),
  K("[1,,2]", verdict::reject, "s5 one value-separator between values"),
  K("[1 2]", verdict::reject, "s5 values are comma separated"),
  K("[1]]", verdict::reject, "s2 trailing end-array"),
  K("[1", verdict::reject, "s5 unterminated array"),
  K("[", verdict::reject, "s5 unterminated array"),
  K("]", verdict::reject, "s5 a lone end-array is not a value"),
  K("[1}", verdict::reject, "s4/s5 mismatched closer"),
  K("[[]", verdict::reject, "s5 unbalanced"),
};

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// s2: exactly one value per JSON-text. anything after it is not part of the text.

const rfc::kase k_trailing[] = {
  K("{}{}", verdict::reject, "s2 two roots"),
  K("{} extra", verdict::reject, "s2 trailing garbage"),
  K("1 2", verdict::reject, "s2 two roots"),
  K("[1][2]", verdict::reject, "s2 two roots"),
  K("nulltrue", verdict::reject, "s2 two roots, no separator"),
  K("\"a\"\"b\"", verdict::reject, "s2 two roots, no separator"),
};

// %%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%%
// F1 regression: 0x00 is not ws, so `ws value ws` cannot contain one outside a string.
//
// Before the tables.hpp patch every case in the first group was ACCEPTED and silently
// truncated -- `1\0garbage` parsed as `1`, consumed() == 9. Two mechanisms composed:
// is_num_end admitted 0x00 as a token terminator, and stage 1 indexes only the first byte
// of a run of non-ws/non-op bytes, so neither the nul nor anything after it in that run
// ever got an index entry for the trailing check to see.
//
// The second group already rejected (those nuls follow an op byte or a quote, so they do
// get indexed) and is here to prove the fix did not over-reach.

const rfc::kase k_nul[] = {
  K("1\0", verdict::reject, "F1 s2 0x00 is not ws: bare number then nul"),
  K("1\0garbage", verdict::reject, "F1 s2 nul then trailing bytes -- used to parse as `1`"),
  K("true\0junk", verdict::reject, "F1 s2 literal then nul then trailing bytes"),
  K("null\0", verdict::reject, "F1 s2 literal then nul"),
  K("false\0x", verdict::reject, "F1 s2 literal then nul then a byte"),
  K("[1\0]", verdict::reject, "F1 s5 interior nul -- used to parse as [1]"),
  K("[1\0,2]", verdict::reject, "F1 s5 interior nul before a separator"),
  K("{\"a\":1\0}", verdict::reject, "F1 s4 interior nul -- used to parse as {\"a\":1}"),
  K("{\"a\":1\0,\"b\":2}", verdict::reject, "F1 s4 interior nul before a separator"),
  K("[1,2\0]", verdict::reject, "F1 s5 interior nul on the last element"),
  K("12345\0", verdict::reject, "F1 s2 multi-digit number then nul"),
  K("-1.5e3\0", verdict::reject, "F1 s2 full number grammar then nul"),

  K("{}\0", verdict::reject, "F1 control: nul after end-object was already rejected"),
  K("[]\0", verdict::reject, "F1 control: nul after end-array"),
  K("\"s\"\0", verdict::reject, "F1 control: nul after a string"),
  K("\0{}", verdict::reject, "F1 control: nul in value position"),
  K("\0", verdict::reject, "F1 control: nul alone"),
  K("[\0]", verdict::reject, "F1 control: nul as an array element"),
  K("{\"a\"\0:1}", verdict::reject, "F1 control: nul where a name-separator belongs"),
};

// nuls are legal INSIDE a string, but only escaped (s7: the control characters U+0000
// through U+001F MUST be escaped)
const rfc::kase k_nul_in_string[] = {
  K("\"\\u0000\"", verdict::accept, "s7 \\u0000 is a legal escape"),
  K("[\"a\\u0000b\"]", verdict::accept, "s7 escaped nul mid-string"),
  K("\"\0\"", verdict::reject, "s7 a RAW nul inside a string MUST be escaped"),
  K("\"a\0b\"", verdict::reject, "s7 a raw nul mid-string MUST be escaped"),
};

};      // namespace

int
main()
{
  {
    sb::test_case("s2: JSON-text is one value, optionally surrounded by ws");
    rfc::expect_all(k_text);
    rfc::expect_all(k_trailing);
    sb::end_test_case();
  }
  {
    sb::test_case("s2: the ws set is exactly 0x20 0x09 0x0a 0x0d and nothing else");
    rfc::expect_all(k_notws);
    sb::end_test_case();
  }
  {
    sb::test_case("s8.1: the byte order mark is a MAY; cjson's choice is pinned");
    rfc::expect_all(k_bom);
    sb::end_test_case();
  }
  {
    sb::test_case("s3: values, and literal names MUST be lowercase");
    rfc::expect_all(k_values);
    sb::end_test_case();
  }
  {
    sb::test_case("s4: objects");
    rfc::expect_all(k_objects);
    rfc::expect_all(k_dupes);
    sb::end_test_case();
  }
  {
    sb::test_case("s5: arrays");
    rfc::expect_all(k_arrays);
    sb::end_test_case();
  }
  {
    sb::test_case("F1: 0x00 outside a string is never part of a JSON-text");
    rfc::expect_all(k_nul);
    rfc::expect_all(k_nul_in_string);
    sb::end_test_case();
  }
  {
    // the nul hole truncated silently, so `consumed()` is the sharp end of it: a caller
    // using stop_when_done to walk ndjson would have skipped the rest of the record.
    sb::test_case("F1: no accepted document ever stops short at an interior nul");
    const char doc[] = "1\0garbage";
    auto r = cjson::parse(reinterpret_cast<const u8 *>(doc), sizeof(doc) - 1);
    sb::require_true(r.is_second());
    // even the lenient mode must not silently truncate here: stop_when_done permits
    // trailing bytes after a COMPLETE root, and `1\0` is not a complete root
    auto rs = cjson::parse(reinterpret_cast<const u8 *>(doc), sizeof(doc) - 1, cjson::opts{ .stop_when_done = true });
    sb::require_true(rs.is_second());
    sb::end_test_case();
  }
  {
    // rfc s9: "A JSON parser MAY accept non-JSON forms or extensions." iterate() skips
    // the stage-2 grammar fsm on purpose -- that is what an on-demand cursor buys. The
    // boundary is pinned on BOTH sides so it can never drift unnoticed.
    sb::test_case("F2: iterate is lenient by default and strict under check_grammar");
    const char *malformed[] = { "[1,,2]", "{,}", "[1 2]", "{\"a\" 1}", "[1,2", "{\"a\":1,}", "[,]" };
    for ( const char *m : malformed ) {
      usize n = 0;
      while ( m[n] ) ++n;
      const u8 *p = reinterpret_cast<const u8 *>(m);

      // strict entry points reject
      sb::require_true(cjson::validate(p, n) != cjson::error::ok);

      // default iterate accepts: stage-1 invariants only
      cjson::scratch sc;
      sb::require_true(cjson::iterate(cjson::bytes{ p, n }, sc).is_first());

      // opt-in makes it agree with validate, error code and all
      cjson::scratch sc2;
      auto rg = cjson::iterate(cjson::bytes{ p, n }, cjson::opts{ .check_grammar = true }, sc2);
      sb::require_true(rg.is_second());
      sb::require_true(rg.cast<cjson::error>() == cjson::validate(p, n));
    }
    sb::end_test_case();
  }
  {
    // check_grammar must not change the verdict for well-formed input, nor the values read
    sb::test_case("F2: check_grammar is a no-op on well-formed documents");
    const char *good[] = { "{\"a\":1,\"b\":[1,2,3]}", "[]", "42", "\"s\"", "{\"n\":{\"m\":true}}" };
    for ( const char *g : good ) {
      usize n = 0;
      while ( g[n] ) ++n;
      const u8 *p = reinterpret_cast<const u8 *>(g);
      cjson::scratch a, b;
      auto ra = cjson::iterate(cjson::bytes{ p, n }, a);
      auto rb = cjson::iterate(cjson::bytes{ p, n }, cjson::opts{ .check_grammar = true }, b);
      sb::require_true(ra.is_first() and rb.is_first());
      sb::require_true(ra.cast<cjson::view>().root().type() == rb.cast<cjson::view>().root().type());
    }
    sb::end_test_case();
  }
  {
    // stage-1 invariants still hold in the lenient mode: iterate is lenient about the
    // GRAMMAR, never about utf-8, string termination or control characters
    sb::test_case("F2: iterate stays strict about stage-1 invariants");
    const char *hard[] = {
      "[\"unterminated",         // no closing quote
      "[\"\x01\"]",              // raw control char in a string
      "[\"\xff\xfe\"]",          // invalid utf-8
      "[\"\xed\xa0\x80\"]",      // surrogate encoded in utf-8
      "[\"\xc0\x80\"]",          // overlong
    };
    for ( const char *h : hard ) {
      usize n = 0;
      while ( h[n] ) ++n;
      cjson::scratch sc;
      auto r = cjson::iterate(cjson::bytes{ reinterpret_cast<const u8 *>(h), n }, sc);
      sb::require_true(r.is_second());
    }
    sb::end_test_case();
  }
  {
    sb::test_case("the discretionary cases are counted, and COMPLIANCE.md accounts for them");
    // s8.1 bom (2) + s4 duplicate names (2). if this number moves, COMPLIANCE.md is stale.
    const usize disc = rfc::discretionary_count(k_bom) + rfc::discretionary_count(k_dupes);
    sb::require(disc, static_cast<usize>(4));
    sb::end_test_case();
  }
  return 1;
}
