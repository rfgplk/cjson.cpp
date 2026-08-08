// 06_build_write.cpp
// Producing json: cjson::builder, cjson::write, cjson::minify.
//
// See also:
//   examples/02_dom.cpp     — the document these writers consume
//   examples/05_oneshot.cpp — compact/pretty/reformat, the one-call versions
//
// Two ways to produce json. builder composes text directly from your values and never
// builds a document. write() serializes a document you already have. minify strips
// whitespace from text without materializing anything at all.

#include "_ex_common.hpp"

#include <micron/std.hpp>
#include <micron/string/strings.hpp>
#include <micron/vector.hpp>

int
main()
{
  // builder
  ex::head("builder");

  cjson::builder b;
  b.obj()
      .kv("id", u64(12345))
      .kv("name", "widget")
      .kv("price", 19.99)
      .kv("active", true)
      .key("tags")
      .arr()
      .value("a")
      .value("b")
      .end()
      .key("meta")
      .obj()
      .kv("rev", i64(-3))
      .end()
      .key("nothing")
      .null()
      .end();

  if ( b.err() != cjson::error::ok )
    mc::echo("builder error: ", cjson::error_name(b.err()));
  else
    ex::show("built: ", b.out());

  // integer width is explicit

  // micron strings work as both keys and values
  {
    micron::string key{};
    key.append("owner", 5);
    micron::string val{};
    val.append("ada", 3);

    cjson::builder sb;
    sb.obj().kv(key, val).end();
    ex::show("from micron strings: ", sb.out());
  }

  // escaping is automatic, in keys and values alike
  {
    micron::string tricky{};
    tricky.append("a\"b\\c\nd", 7);
    cjson::builder eb;
    eb.obj().kv("raw", tricky).end();
    ex::show("escaped: ", eb.out());
  }

  ex::head("sticky error");

  cjson::builder bad;
  bad.obj().key("k").end();      // a key with no value: unbalanced
  mc::echo("err  = ", cjson::error_name(bad.err()));
  mc::echo("out  = ", bad.out().len, " bytes (empty: the document was never valid)");

  // an unterminated builder is also empty, not truncated
  cjson::builder open;
  open.obj().kv("a", i64(1));
  mc::echo("unclosed out = ", open.out().len, " bytes");

  ex::head("buffer reuse");

  micron::string recycled{};
  for ( u32 i = 0; i < 3; i++ ) {
    cjson::builder rb{ micron::move(recycled) };
    rb.obj().kv("seq", u64(i)).end();
    mc::echo("  ", rb.out().len, " bytes");
    recycled = rb.take();
  }
  mc::echo("buffer capacity carried across all three: ", recycled.max_size() > 0 ? "yes" : "no");

  // write
  ex::head("write");

  constexpr const char k_src[] = R"({"a":[1,2,3],"b":{"c":"x"},"d":true})";
  auto r = cjson::parse(k_src, sizeof(k_src) - 1, cjson::opts{ .with_write_bound = true });
  if ( r.is_second() ) return 1;
  const cjson::doc &d = r.cast<cjson::doc>();

  // owning output
  cjson::fjson flat = cjson::write(d);
  mc::echo("minified: ", flat.size(), " bytes");

  cjson::fjson nice = cjson::write(d, cjson::style{ .indent = 2 });
  mc::echo("pretty:   ", nice.size(), " bytes");

  const usize cap = cjson::write_bound(d);
  micron::vector<u8> out;
  out.reserve(cap + 8);
  const max_t w = cjson::write_into(d, cjson::wbytes{ out.begin(), cap });
  mc::echo("write_into wrote ", usize(w), " of ", cap, " reserved bytes");

  u8 tiny[4];
  const max_t fail = cjson::write_into(d, cjson::wbytes{ tiny, sizeof(tiny) });
  mc::echo("into a 4-byte buffer -> ", cjson::error_name(cjson::as_error(fail)));

  // NOTE: non-finite doubles serialize as null, never Infinity or NaN (js parity)

  // minify
  ex::head("minify");

  constexpr const char k_pretty[] = "{ \"a\" : [ 1 , 2 ] , \"b\" : true }";
  auto m = cjson::minify_str(k_pretty, sizeof(k_pretty) - 1);
  if ( m.is_first() ) {
    const micron::string &s = m.cast<micron::string>();
    mc::echo(sizeof(k_pretty) - 1, " bytes -> ", s.size(), " bytes");
    mc::echo(s.c_str());
  }

  // into a caller buffer instead, sized by minify_bound
  {
    u8 mbuf[64];
    const max_t mw = cjson::minify(k_pretty, sizeof(k_pretty) - 1, cjson::wbytes{ mbuf, sizeof(mbuf) });
    mc::echo("minify into a buffer: ", usize(mw), " bytes");
  }

  // and it rejects invalid input rather than producing junk
  mc::echo("minify(\"{oops\") -> ", cjson::minify_str("{oops", 5).is_second() ? "rejected" : "accepted");

  mc::echo("");
  return 0;
}
