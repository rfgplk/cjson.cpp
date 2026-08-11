#include "_ex_common.hpp"

#include <micron/maps/robin.hpp>
#include <micron/std.hpp>

namespace
{

constexpr char k_cfg[] = R"({
  "service": "billing",
  "workers": 4,
  "drift":   -3,
  "ratio":   0.25,
  "tls":     true,
  "shutdown": null,
  "limits":  { "rps": 2500, "burst": 400 },
  "peers":   [ "10.0.0.1", "10.0.0.2" ]
})";

};      // namespace

int
main()
{
  auto r = cjson::parse(k_cfg, sizeof(k_cfg) - 1);
  if ( r.is_second() ) {
    mc::echo("parse failed: ", cjson::error_name(r.cast<cjson::error>()));
    return 1;
  }
  const cjson::doc &d = r.cast<cjson::doc>();
  const cjson::val root = d.root();

  // a) one value, any kind
  ex::head("marshal");

  mc::echo("service  = ", cjson::pun(root["service"]));
  mc::echo("workers  = ", cjson::pun(root["workers"]));      // u64
  mc::echo("drift    = ", cjson::pun(root["drift"]));        // i64, sign preserved
  mc::echo("ratio    = ", cjson::pun(root["ratio"]));        // f64
  mc::echo("tls      = ", cjson::pun(root["tls"]));
  mc::echo("limits   = ", cjson::pun(root["limits"]));      // a whole subtree

  // null is a value; only a miss is valueless
  const cjson::pun nul = root["shutdown"];
  const cjson::pun miss = root["nope"];
  mc::echo("shutdown is json null: ", nul.is<cjson::jnull>() ? "yes" : "no");
  mc::echo("nope     has a value : ", miss.has_value() ? "yes" : "no");

  // b) a flat object as a map
  ex::head("to_map");

  auto mr = cjson::to_map(root);
  if ( mr.is_second() ) {
    mc::echo("to_map failed: ", cjson::error_name(mr.cast<cjson::error>()));
    return 1;
  }
  const cjson::object_map &m = mr.cast<cjson::object_map>();
  mc::echo("distinct keys = ", m.size());

  // iterate with for_each; find returns a pointer, null on a miss
  m.for_each([](const mc::string &k, const cjson::pun &v) { mc::echo("  ", k.c_str(), " -> ", v); });

  if ( const cjson::pun *w = m.find(mc::string("workers")); w != nullptr ) mc::echo("workers via find = ", w->cast<u64>());

  // c) descending into a nested value
  ex::head("nested");

  const cjson::pun *lim = m.find(mc::string("limits"));
  if ( lim != nullptr and lim->is<cjson::vref>() ) {
    const cjson::val nested = cjson::as_val(lim->cast<cjson::vref>());
    mc::echo("limits.rps   = ", nested["rps"].i64_or(0));
    mc::echo("limits.burst = ", nested["burst"].i64_or(0));
    // a subtree also serialises on its own
    const mc::string sub = cjson::write_str(nested);
    mc::echo("limits json  = ", sub);
  }

  // an array becomes a vector, in order, with mixed kinds preserved
  auto vr = cjson::to_vector(root["peers"]);
  if ( vr.is_first() ) {
    const cjson::array_vec &v = vr.cast<cjson::array_vec>();
    mc::echo("peers count = ", v.size());
    for ( usize i = 0; i < v.size(); ++i ) mc::echo("  peer[", i, "] = ", v[i]);
  }

  // d) bring your own container
  ex::head("bring your own map");

  mc::robin_map<mc::string, cjson::pun> rb(cjson::map_slots(root));
  if ( cjson::to_map_into(root, rb) == cjson::error::ok ) mc::echo("robin_map keys   = ", rb.size());

  // key_view borrows the key bytes instead of copying them. valid only while the doc is
  mc::hswiss<cjson::key_view, cjson::pun> kv{};
  if ( cjson::to_map_into(root, kv) == cjson::error::ok ) {
    mc::echo("key_view keys    = ", kv.size());
    if ( const cjson::pun *p = kv.find(cjson::key_view{ cjson::as_strv("ratio") }); p != nullptr )
      mc::echo("ratio via borrowed key = ", p->cast<f64>());
  }

  // a kind mismatch is an error, not an empty map
  mc::echo("to_map on a string -> ", cjson::error_name(cjson::to_map(root["service"]).cast<cjson::error>()));

  ex::head("round trip");

  auto j = cjson::to_json(m);
  if ( j.is_first() ) {
    const mc::string &js = j.cast<mc::string>();
    mc::echo("json = ", js);
    auto back = cjson::parse(js.c_str(), js.size());
    mc::echo("reparses    : ", back.is_first() ? "yes" : "no");
    if ( back.is_first() ) mc::echo("limits.rps survived the trip = ", back.cast<cjson::doc>().root()["limits"]["rps"].i64_or(0));
  }

  // e) lifetimes
  ex::head("lifetimes");

  // keys are owned copies, a vref is valid exactly as long as its doc is (and until the next structural edit)
  // a jraw is valid as long as the input text is
  // the on-demand path marshals containers as jraw
  auto g = cjson::get(k_cfg, "/limits");
  if ( g.is_first() and g.cast<cjson::pun>().is<cjson::jraw>() ) {
    const cjson::strv span = g.cast<cjson::pun>().cast<cjson::jraw>().text;
    ex::show("oneshot /limits = ", span);
    auto sub = cjson::parse(span.ptr, span.len);
    if ( sub.is_first() ) mc::echo("  reparsed burst = ", sub.cast<cjson::doc>().root()["burst"].i64_or(0));
  }

  return 0;
}
