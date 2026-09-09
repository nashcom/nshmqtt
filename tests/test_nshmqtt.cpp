// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

// Standalone unit-test binary for nshmqtt's pure-function pieces (HTTP
// parsing, config parsing, JSON helpers, state persistence, Prometheus name
// normalization). Deliberately not assert()-based: a failed check is
// recorded and printed, and the run continues so one broken case doesn't
// hide the rest. Exit code is 0 only if every check passed.
//
// MQTT connectivity (publish/subscribe/reconnect against a real broker) and
// full end-to-end HTTP behavior are covered separately by
// tests/integration_test.sh against a running daemon and a real Mosquitto
// broker -- this binary only exercises code that doesn't need either.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "../src/config.h"
#include "../src/event_placeholders.h"
#include "../src/http.h"
#include "../src/json_util.h"
#include "../src/metrics.h"
#include "../src/mqtt_topic.h"
#include "../src/state.h"
#include "../src/text_util.h"
#include "../src/webhook_json.h"

namespace
{

int g_pass = 0;
int g_fail = 0;

void check(bool condition, const std::string &description)
{
    if (condition)
    {
        ++g_pass;
    }
    else
    {
        ++g_fail;
        std::fprintf(stderr, "FAIL: %s\n", description.c_str());
    }
}

void check_eq(const std::string &actual, const std::string &expected, const std::string &description)
{
    if (actual == expected)
    {
        ++g_pass;
    }
    else
    {
        ++g_fail;
        std::fprintf(stderr, "FAIL: %s\n  expected: %s\n  actual:   %s\n", description.c_str(), expected.c_str(),
                     actual.c_str());
    }
}

void check_double_eq(double actual, double expected, const std::string &description)
{
    check(actual > expected - 1e-9 && actual < expected + 1e-9, description);
}

// ---------------------------------------------------------------------
// text_util: format_double / parse_double
// ---------------------------------------------------------------------

void test_format_double_natural_precision()
{
    check_eq(nshmqtt::format_double(17.3), "17.3", "format_double drops trailing zero padding");
    check_eq(nshmqtt::format_double(0), "0", "format_double of zero");
    check_eq(nshmqtt::format_double(-4.5), "-4.5", "format_double of a negative value");
}

void test_parse_double_roundtrip()
{
    double v = 0.0;
    check(nshmqtt::parse_double("17.3", v), "parse_double accepts a plain number");
    check_double_eq(v, 17.3, "parse_double value correct");

    check(nshmqtt::parse_double("  22.7  ", v), "parse_double trims surrounding whitespace");
    check_double_eq(v, 22.7, "parse_double trimmed value correct");

    check(nshmqtt::parse_double("-5", v), "parse_double accepts a negative integer");
    check(!nshmqtt::parse_double("17.3garbage", v), "parse_double rejects trailing garbage");
    check(!nshmqtt::parse_double("", v), "parse_double rejects an empty string");
    check(!nshmqtt::parse_double("not-a-number", v), "parse_double rejects non-numeric text");
}

void test_parse_int_basic()
{
    long v = 0;
    check(nshmqtt::parse_int("2", v), "parse_int accepts a plain integer");
    check(v == 2, "parse_int value correct");

    check(nshmqtt::parse_int("  1  ", v), "parse_int trims surrounding whitespace");
    check(v == 1, "parse_int trimmed value correct");

    check(nshmqtt::parse_int("-3", v), "parse_int accepts a negative integer");
    check(!nshmqtt::parse_int("2garbage", v), "parse_int rejects trailing garbage");
    check(!nshmqtt::parse_int("2.5", v), "parse_int rejects a fractional number");
    check(!nshmqtt::parse_int("", v), "parse_int rejects an empty string");
    check(!nshmqtt::parse_int("not-a-number", v), "parse_int rejects non-numeric text");
}

void test_parse_bool_basic()
{
    bool v = false;
    check(nshmqtt::parse_bool("true", v) && v, "parse_bool accepts 'true'");
    check(nshmqtt::parse_bool("1", v) && v, "parse_bool accepts '1'");
    check(nshmqtt::parse_bool("yes", v) && v, "parse_bool accepts 'yes'");
    check(nshmqtt::parse_bool("On", v) && v, "parse_bool accepts 'On' case-insensitively");

    check(nshmqtt::parse_bool("false", v) && !v, "parse_bool accepts 'false'");
    check(nshmqtt::parse_bool("0", v) && !v, "parse_bool accepts '0'");
    check(nshmqtt::parse_bool("no", v) && !v, "parse_bool accepts 'no'");
    check(nshmqtt::parse_bool("Off", v) && !v, "parse_bool accepts 'Off' case-insensitively");

    check(!nshmqtt::parse_bool("", v), "parse_bool rejects an empty string");
    check(!nshmqtt::parse_bool("maybe", v), "parse_bool rejects an unrecognized word");
}

void test_constant_time_equals()
{
    check(nshmqtt::constant_time_equals("secret-token", "secret-token"), "identical strings compare equal");
    check(!nshmqtt::constant_time_equals("secret-token", "wrong-token"), "different strings compare unequal");
    check(!nshmqtt::constant_time_equals("short", "much-longer-string"), "different lengths compare unequal");
    check(!nshmqtt::constant_time_equals("", "nonempty"), "empty vs non-empty compares unequal");
    check(nshmqtt::constant_time_equals("", ""), "two empty strings compare equal");
}

void test_json_escape()
{
    check_eq(nshmqtt::json_escape("hello"), "hello", "json_escape leaves plain text unchanged");
    check_eq(nshmqtt::json_escape("a\"b"), "a\\\"b", "json_escape escapes a quote");
    check_eq(nshmqtt::json_escape("a\\b"), "a\\\\b", "json_escape escapes a backslash");
    check_eq(nshmqtt::json_escape("a\nb"), "a\\nb", "json_escape escapes a newline");
}

// ---------------------------------------------------------------------
// json_util: extract_json_number_field / parse_flat_number_object
// ---------------------------------------------------------------------

void test_extract_json_number_field()
{
    double v = 0.0;
    check(nshmqtt::extract_json_number_field(R"({"value": 17.3})", "value", v), "extracts a simple numeric field");
    check_double_eq(v, 17.3, "extracted value correct");

    check(nshmqtt::extract_json_number_field(R"({"value":-4})", "value", v), "extracts a negative integer");
    check_double_eq(v, -4.0, "extracted negative value correct");

    check(nshmqtt::extract_json_number_field(R"({"note":"hot","value":22.7})", "value", v),
          "finds the field regardless of key order");
    check_double_eq(v, 22.7, "value correct with a preceding string field");

    check(!nshmqtt::extract_json_number_field(R"({"other": 1})", "value", v), "missing field is reported as absent");
    check(!nshmqtt::extract_json_number_field(R"({"value": "hot"})", "value", v),
          "non-numeric field value is rejected");
    check(!nshmqtt::extract_json_number_field("not json at all", "value", v), "non-object input is rejected");
    check(!nshmqtt::extract_json_number_field(R"({"value": 1)", "value", v), "unterminated object is rejected");
}

void test_parse_flat_number_object()
{
    std::map<std::string, double> out;
    check(nshmqtt::parse_flat_number_object(R"({"server1/load": 17.3, "room/temperature": 22.4})", out),
          "parses a flat name->number object");
    check(out.size() == 2, "both entries present");
    check_double_eq(out["server1/load"], 17.3, "first entry value correct");
    check_double_eq(out["room/temperature"], 22.4, "second entry value correct");

    std::map<std::string, double> empty_out;
    check(nshmqtt::parse_flat_number_object("{}", empty_out), "parses an empty object");
    check(empty_out.empty(), "empty object yields no entries");

    std::map<std::string, double> bad_out;
    check(!nshmqtt::parse_flat_number_object(R"({"a": "not a number"})", bad_out), "rejects a non-numeric value");
    check(!nshmqtt::parse_flat_number_object("[1,2,3]", bad_out), "rejects a top-level array");
    check(!nshmqtt::parse_flat_number_object("", bad_out), "rejects empty input");
}

// Looks up `name` in a flatten_json_object() result; returns false if
// absent, so tests can assert both presence (with value) and absence in
// one helper.
bool find_leaf(const std::vector<std::pair<std::string, double>> &leaves, const std::string &name, double &out)
{
    for (const auto &leaf : leaves)
    {
        if (leaf.first == name)
        {
            out = leaf.second;
            return true;
        }
    }
    return false;
}

bool has_leaf(const std::vector<std::pair<std::string, double>> &leaves, const std::string &name)
{
    double unused = 0.0;
    return find_leaf(leaves, name, unused);
}

void test_flatten_json_object_numbers()
{
    std::vector<std::pair<std::string, double>> leaves;
    check(nshmqtt::flatten_json_object(R"({"batteryPercent": 91, "batteryVoltage": 4.12})", leaves),
          "parses a flat object of numbers");
    check(leaves.size() == 2, "both leaves present");
    double v = 0.0;
    check(find_leaf(leaves, "batteryPercent", v) && v == 91.0, "integer leaf value correct");
    check(find_leaf(leaves, "batteryVoltage", v), "float leaf present");
    check_double_eq(v, 4.12, "float leaf value correct");
}

void test_flatten_json_object_booleans()
{
    std::vector<std::pair<std::string, double>> leaves;
    check(nshmqtt::flatten_json_object(R"({"matrixPower": false, "lowBattery": true})", leaves),
          "parses boolean leaves");
    double v = 0.0;
    check(find_leaf(leaves, "matrixPower", v) && v == 0.0, "false becomes 0");
    check(find_leaf(leaves, "lowBattery", v) && v == 1.0, "true becomes 1");
}

void test_flatten_json_object_nested()
{
    std::vector<std::pair<std::string, double>> leaves;
    check(nshmqtt::flatten_json_object(R"({"wifi": {"enabled": true, "attempts": 0, "connects": 1}})", leaves),
          "parses a nested object");
    check(leaves.size() == 3, "all three nested leaves present, none for 'wifi' itself");
    double v = 0.0;
    check(find_leaf(leaves, "wifi_enabled", v) && v == 1.0, "nested key joined with '_', bool converted");
    check(find_leaf(leaves, "wifi_attempts", v) && v == 0.0, "nested integer leaf correct");
    check(find_leaf(leaves, "wifi_connects", v) && v == 1.0, "second nested integer leaf correct");
    check(!has_leaf(leaves, "wifi"), "the intermediate object itself never becomes a leaf");
}

void test_flatten_json_object_deeply_nested()
{
    std::vector<std::pair<std::string, double>> leaves;
    check(nshmqtt::flatten_json_object(R"({"a": {"b": {"c": 5}}})", leaves), "parses two levels of nesting");
    check(leaves.size() == 1, "exactly one leaf");
    double v = 0.0;
    check(find_leaf(leaves, "a_b_c", v) && v == 5.0, "each level joined with '_' in order");
}

void test_flatten_json_object_ignores_strings_nulls_arrays()
{
    std::vector<std::pair<std::string, double>> leaves;
    check(nshmqtt::flatten_json_object(R"({"currentApp": "Weather", "extra": null, "indicators": [1,2,3], "fps": 42})",
                                       leaves),
          "a string/null/array alongside a number is not itself a parse error");
    check(leaves.size() == 1, "only the numeric leaf survives");
    double v = 0.0;
    check(find_leaf(leaves, "fps", v) && v == 42.0, "the one numeric leaf is still correct");
    check(!has_leaf(leaves, "currentApp"), "string leaf ignored");
    check(!has_leaf(leaves, "extra"), "null leaf ignored");
    check(!has_leaf(leaves, "indicators"), "array leaf ignored");
}

void test_flatten_json_object_malformed()
{
    std::vector<std::pair<std::string, double>> leaves;
    check(!nshmqtt::flatten_json_object("{not json", leaves), "rejects unterminated/malformed input");
    check(!nshmqtt::flatten_json_object("[1,2,3]", leaves),
          "rejects a top-level array, same as parse_flat_number_object");
    check(!nshmqtt::flatten_json_object("", leaves), "rejects empty input");
    check(!nshmqtt::flatten_json_object(R"({"a": undefined})", leaves),
          "rejects a value that isn't a recognized JSON literal shape at all");

    std::vector<std::pair<std::string, double>> empty_leaves;
    check(nshmqtt::flatten_json_object("{}", empty_leaves), "an empty object is valid, not malformed");
    check(empty_leaves.empty(), "an empty object yields no leaves");
}

// ---------------------------------------------------------------------
// http.h: request-line/header/body parsing
// ---------------------------------------------------------------------

void test_parse_http_request_basic()
{
    std::string raw = "GET /event/server1/status?value=completed HTTP/1.1\r\nHost: x\r\n\r\n";
    nshmqtt::HttpRequest req;
    std::string leftover;
    check(nshmqtt::parse_http_request(raw, req, leftover), "parses a basic GET request");
    check_eq(req.method, "GET", "method parsed");
    check_eq(req.path, "/event/server1/status", "path decoded and query string stripped");
    std::string value;
    check(nshmqtt::find_query_param(req, "value", value), "query parameter found");
    check_eq(value, "completed", "query parameter value correct");
}

void test_parse_http_request_with_body_headers()
{
    std::string raw = "POST /event/x HTTP/1.1\r\nContent-Type: application/json\r\nContent-Length: 13\r\n\r\n"
                      "{\"a\":1}extra";
    nshmqtt::HttpRequest req;
    std::string leftover;
    check(nshmqtt::parse_http_request(raw, req, leftover), "parses a POST request with body headers");
    check_eq(req.content_type, "application/json", "Content-Type captured");
    check(req.content_length == 13, "Content-Length captured");
    check_eq(leftover, "{\"a\":1}extra", "bytes after the header terminator are returned as leftover_body");
}

void test_parse_http_request_chunked_flagged()
{
    std::string raw = "POST /event/x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
    nshmqtt::HttpRequest req;
    std::string leftover;
    check(nshmqtt::parse_http_request(raw, req, leftover),
          "a chunked request still parses (rejection is the caller's job)");
    check(req.chunked, "chunked flag set from Transfer-Encoding header");
}

void test_parse_http_request_malformed()
{
    nshmqtt::HttpRequest req;
    std::string leftover;
    check(!nshmqtt::parse_http_request("not a request at all", req, leftover), "garbage input rejected");
    check(!nshmqtt::parse_http_request("get /x HTTP/1.1\r\n\r\n", req, leftover), "lowercase method rejected");
    check(!nshmqtt::parse_http_request("GET x HTTP/1.1\r\n\r\n", req, leftover), "path without leading '/' rejected");
    check(!nshmqtt::parse_http_request("GET /x HTTP/0.9\r\n\r\n", req, leftover), "unsupported HTTP version rejected");
    check(!nshmqtt::parse_http_request("GET /x HTTP/1.1\r\nContent-Length: abc\r\n\r\n", req, leftover),
          "non-numeric Content-Length rejected");
}

void test_parse_http_request_captures_api_key()
{
    std::string raw = "POST /event/x HTTP/1.1\r\nX-Mqtt-Api-Key: sekrit\r\n\r\n";
    nshmqtt::HttpRequest req;
    std::string leftover;
    check(nshmqtt::parse_http_request(raw, req, leftover), "parses a request carrying X-Mqtt-Api-Key");
    check_eq(req.auth_token, "sekrit", "X-Mqtt-Api-Key value captured");

    std::string raw_absent = "POST /event/x HTTP/1.1\r\n\r\n";
    nshmqtt::HttpRequest req2;
    nshmqtt::parse_http_request(raw_absent, req2, leftover);
    check(req2.auth_token.empty(), "auth_token empty when the header is absent");

    // HTTP header names are case-insensitive (RFC 7230) -- any casing a
    // client sends must be recognized the same way.
    std::string raw_mixed_case = "POST /event/x HTTP/1.1\r\nx-MQTT-api-KEY: sekrit\r\n\r\n";
    nshmqtt::HttpRequest req3;
    nshmqtt::parse_http_request(raw_mixed_case, req3, leftover);
    check_eq(req3.auth_token, "sekrit", "X-Mqtt-Api-Key recognized regardless of header name casing");
}

void test_parse_http_request_captures_qos_retain_headers()
{
    std::string raw = "POST /event/x HTTP/1.1\r\nX-Mqtt-Qos: 2\r\nX-Mqtt-Retain: true\r\n\r\n";
    nshmqtt::HttpRequest req;
    std::string leftover;
    check(nshmqtt::parse_http_request(raw, req, leftover), "parses a request carrying X-Mqtt-Qos/X-Mqtt-Retain");
    check_eq(req.qos_header, "2", "X-Mqtt-Qos value captured");
    check_eq(req.retain_header, "true", "X-Mqtt-Retain value captured");

    std::string raw_absent = "POST /event/x HTTP/1.1\r\n\r\n";
    nshmqtt::HttpRequest req2;
    nshmqtt::parse_http_request(raw_absent, req2, leftover);
    check(req2.qos_header.empty(), "qos_header empty when the header is absent");
    check(req2.retain_header.empty(), "retain_header empty when the header is absent");
}

void test_accept_wants_json()
{
    check(nshmqtt::accept_wants_json("application/json"), "exact match");
    check(nshmqtt::accept_wants_json("text/plain, application/json"), "substring match among several");
    check(!nshmqtt::accept_wants_json("text/plain"), "text/plain alone does not want json");
    check(!nshmqtt::accept_wants_json(""), "empty Accept does not want json");
}

// ---------------------------------------------------------------------
// config.h
// ---------------------------------------------------------------------

void test_config_defaults()
{
    nshmqtt::Config cfg;
    check_eq(cfg.socket_path, "/run/nshmqtt/nshmqtt.sock", "default socket path");
    check(cfg.tcp_port == 0, "TCP listener disabled by default");
    check(cfg.simple_get, "simple_get enabled by default");
    check(cfg.mqtt_qos == 1, "default MQTT QoS is 1");
    check(cfg.subscribe_enabled == false, "subscribe disabled by default");
    check(cfg.state_enabled, "state persistence enabled by default");
    check(cfg.http_auth_tokens.empty(), "http_auth_tokens empty (auth disabled) by default");
    check(cfg.mqtt_pool_size == 1, "mqtt_pool_size defaults to 1");
    check(cfg.prometheus_mqtt_json_topics.empty(), "prometheus_mqtt_json_topics empty by default");
    check_eq(cfg.prometheus_prefix, "nshmqtt_", "service metric prefix defaults to nshmqtt_");
    check_eq(cfg.prometheus_mqtt_state_prefix, "mqtt_",
             "state/content metric prefix defaults to mqtt_, deliberately different from the service prefix");
}

void test_config_load_and_env_override()
{
    std::string path = "/tmp/nshmqtt_test_config.conf";
    {
        std::ofstream out(path);
        out << "# comment\n\nsocket=/tmp/x.sock\nmqtt_host=broker.example\nmqtt_port=8883\n"
               "subscribe_enabled=true\nsubscribe_topics=a/#, b/c , \nmqtt_qos=2\n"
               "http_auth_tokens=tok-a, tok-b\nprometheus_mqtt_json_topics=awtrix/state/device, sensor/foo/state\n"
               "unknown_key=ignored\n";
    }

    nshmqtt::Config cfg;
    std::string err;
    std::vector<std::string> warnings;
    check(nshmqtt::load_config(path, cfg, err, &warnings), "loads a well-formed config file");
    check_eq(cfg.socket_path, "/tmp/x.sock", "socket overridden from file");
    check_eq(cfg.mqtt_host, "broker.example", "mqtt_host overridden from file");
    check(cfg.mqtt_port == 8883, "mqtt_port overridden from file");
    check(cfg.mqtt_qos == 2, "mqtt_qos overridden from file");
    check(cfg.subscribe_topics.size() == 2, "subscribe_topics split on commas, blanks dropped");
    check(cfg.http_auth_tokens.size() == 2, "http_auth_tokens split on commas");
    check(cfg.prometheus_mqtt_json_topics.size() == 2, "prometheus_mqtt_json_topics split on commas");
    check_eq(cfg.prometheus_mqtt_json_topics[0], "awtrix/state/device", "first JSON topic parsed exactly");
    check_eq(cfg.prometheus_mqtt_json_topics[1], "sensor/foo/state", "second JSON topic parsed exactly");
    check(warnings.size() == 1, "unknown key produces exactly one warning");

    setenv("NSHMQTT_MQTT_HOST", "env-broker.example", 1);
    std::string env_err;
    check(nshmqtt::apply_env_overrides(cfg, env_err), "applies environment overrides");
    check_eq(cfg.mqtt_host, "env-broker.example", "environment variable takes precedence over the config file");
    unsetenv("NSHMQTT_MQTT_HOST");

    std::remove(path.c_str());
}

void test_config_rejects_bad_qos()
{
    std::string path = "/tmp/nshmqtt_test_bad_qos.conf";
    {
        std::ofstream out(path);
        out << "mqtt_qos=7\n";
    }
    nshmqtt::Config cfg;
    std::string err;
    check(!nshmqtt::load_config(path, cfg, err), "out-of-range mqtt_qos is rejected");
    std::remove(path.c_str());
}

void test_config_missing_file_is_error()
{
    nshmqtt::Config cfg;
    std::string err;
    check(!nshmqtt::load_config("/tmp/nshmqtt_does_not_exist.conf", cfg, err), "missing config file reported as error");
}

// ---------------------------------------------------------------------
// state.h: StateStore
// ---------------------------------------------------------------------

void test_state_store_basic()
{
    nshmqtt::StateStore store;
    double v = 0.0;
    check(!store.get("x", v), "unset name is not found");

    store.set("server1/load", 17.3);
    check(store.get("server1/load", v), "set name is found");
    check_double_eq(v, 17.3, "stored value correct");
    check(store.size() == 1, "size reflects one entry");

    store.set("server1/load", 20.0); // replace
    store.get("server1/load", v);
    check_double_eq(v, 20.0, "set() replaces an existing value");

    check(store.remove("server1/load"), "remove() reports the entry existed");
    check(!store.remove("server1/load"), "remove() on an already-removed name reports false");
    check(!store.get("server1/load", v), "value gone after remove()");
}

void test_state_store_persistence_roundtrip()
{
    std::string path = "/tmp/nshmqtt_test_state.json";
    std::remove(path.c_str());
    std::remove((path + ".tmp").c_str());

    nshmqtt::StateStore store;
    store.set("server1/load", 17.3);
    store.set("room/temperature", 22.4);

    std::string err;
    check(store.save(path, err), "save() succeeds");
    check(std::ifstream(path).good(), "state file exists after save()");
    check(!std::ifstream(path + ".tmp").good(), "temp file is not left behind after a successful save");

    nshmqtt::StateStore loaded;
    check(loaded.load(path, err), "load() succeeds");
    check(loaded.size() == 2, "loaded store has both entries");
    double v = 0.0;
    check(loaded.get("server1/load", v) && v > 17.29 && v < 17.31, "loaded value correct");

    std::remove(path.c_str());
}

void test_state_store_load_missing_file_not_an_error()
{
    nshmqtt::StateStore store;
    std::string err;
    check(store.load("/tmp/nshmqtt_state_never_existed.json", err), "loading a missing state file is not an error");
    check(store.size() == 0, "store stays empty when there was nothing to load");
}

// ---------------------------------------------------------------------
// metrics.h: request/response counters and Prometheus name normalization
// ---------------------------------------------------------------------

void test_metrics_record_request()
{
    nshmqtt::Metrics m;
    m.record_request("/event/x/y");
    m.record_request("/metric/x");
    m.record_request("/health");
    m.record_request("/metrics");
    m.record_request("/unknown");
    check(m.event_requests_total.load() == 1, "event request counted");
    check(m.metric_requests_total.load() == 1, "metric request counted");
    check(m.health_requests_total.load() == 1, "health request counted");
    check(m.metrics_requests_total.load() == 1, "metrics request counted");
    check(m.other_requests_total.load() == 1, "unknown path counted as other");
}

void test_metrics_record_response()
{
    nshmqtt::Metrics m;
    m.record_response(200);
    m.record_response(404);
    m.record_response(500);
    check(m.responses_2xx.load() == 1, "2xx counted");
    check(m.responses_4xx.load() == 1, "4xx counted");
    check(m.responses_5xx.load() == 1, "5xx counted");
}

void test_normalize_metric_name()
{
    check_eq(nshmqtt::normalize_metric_name("nshmqtt_", "server1/system/cpu/load"), "nshmqtt_server1_system_cpu_load",
             "slashes normalized to underscores");
    check_eq(nshmqtt::normalize_metric_name("nshmqtt_", "1abc"), "nshmqtt__1abc",
             "leading digit gets an underscore prefix");
    check_eq(nshmqtt::normalize_metric_name("nshmqtt_", "a-b c"), "nshmqtt_a_b_c",
             "non-alphanumeric bytes become underscores");
}

void test_normalize_metric_name_topic_prefix()
{
    check_eq(nshmqtt::normalize_metric_name("nshmqtt_", "cpu/load", "site1/"), "nshmqtt_site1_cpu_load",
             "topic_prefix is prepended before normalization and goes through the same substitution");
    check_eq(nshmqtt::normalize_metric_name("nshmqtt_", "cpu/load"), "nshmqtt_cpu_load",
             "omitted topic_prefix defaults to empty -- today's behavior unchanged");
    check_eq(nshmqtt::normalize_metric_name("nshmqtt_", "cpu/load", ""), "nshmqtt_cpu_load",
             "explicit empty topic_prefix behaves the same as omitting it");
}

void test_normalize_all_collision_detection()
{
    std::vector<std::pair<std::string, double>> items = {
        {"room.temp", 20.0},
        {"room_temp", 21.0}, // normalizes to the same Prometheus name as room.temp
        {"kitchen/temp", 19.5},
    };
    nshmqtt::NormalizedMetrics result = nshmqtt::normalize_all("nshmqtt_", items);
    check(result.metrics.size() == 2, "one colliding entry is dropped from the output");
    check(result.collisions.size() == 1, "exactly one collision reported");
    check_eq(result.collisions[0], "room_temp", "the alphabetically-later name is the one reported as colliding");
}

void test_normalize_all_topic_prefix()
{
    std::vector<std::pair<std::string, double>> items = {{"cpu/load", 1.5}};
    nshmqtt::NormalizedMetrics result = nshmqtt::normalize_all("nshmqtt_", items, "site1/");
    check(result.metrics.size() == 1, "single entry passes through");
    check_eq(result.metrics[0].first, "nshmqtt_site1_cpu_load",
             "normalize_all threads topic_prefix through to each entry");
}

void test_topic_matches_filter_exact_and_wildcards()
{
    check(nshmqtt::topic_matches_filter("sensors/kitchen/temperature", "sensors/kitchen/temperature"), "exact match");
    check(!nshmqtt::topic_matches_filter("sensors/kitchen/temperature", "sensors/kitchen/humidity"),
          "different leaf does not match");

    check(nshmqtt::topic_matches_filter("sensors/kitchen/temperature", "sensors/+/temperature"),
          "'+' matches exactly one level");
    check(!nshmqtt::topic_matches_filter("sensors/kitchen/room/temperature", "sensors/+/temperature"),
          "'+' does not match more than one level");
    check(!nshmqtt::topic_matches_filter("sensors/temperature", "sensors/+/temperature"),
          "'+' does not match zero levels");

    check(nshmqtt::topic_matches_filter("sensors/kitchen/temperature", "sensors/#"), "'#' matches remaining levels");
    check(nshmqtt::topic_matches_filter("sensors", "sensors/#"), "'#' matches zero remaining levels too");
    check(nshmqtt::topic_matches_filter("anything/at/all", "#"), "bare '#' matches every ordinary topic");

    check(!nshmqtt::topic_matches_filter("$SYS/broker/uptime", "#"),
          "a leading '$' topic does not match a bare wildcard filter");
    check(!nshmqtt::topic_matches_filter("$SYS/broker/uptime", "+/broker/uptime"),
          "a leading '$' topic does not match a leading '+' filter");
    check(nshmqtt::topic_matches_filter("$SYS/broker/uptime", "$SYS/#"),
          "an explicit $SYS/# filter still matches $SYS topics normally");

    check(!nshmqtt::topic_matches_filter("sensors/kitchen", "sensors/kitchen/temperature"),
          "filter longer than topic does not match");
    check(!nshmqtt::topic_matches_filter("sensors/kitchen/temperature/extra", "sensors/kitchen/temperature"),
          "topic longer than filter (no trailing '#') does not match");
}

void test_topic_matches_any()
{
    std::vector<std::string> filters = {"metrics/#", "events/status"};
    check(nshmqtt::topic_matches_any("metrics/server1/load", filters), "matches the first filter in the list");
    check(nshmqtt::topic_matches_any("events/status", filters), "matches the second filter in the list");
    check(!nshmqtt::topic_matches_any("other/topic", filters), "matches neither filter");
    check(!nshmqtt::topic_matches_any("anything", {}), "an empty filter list matches nothing");
}

void test_build_webhook_json_basic()
{
    std::string out = nshmqtt::build_webhook_json("sensors/kitchen/temperature", "21.4", false, 1700000000);
    check_eq(out,
             "{\"topic\":\"sensors/kitchen/temperature\",\"payload\":\"21.4\",\"retain\":false,"
             "\"timestamp\":1700000000}",
             "renders the expected JSON envelope");

    std::string retained_out = nshmqtt::build_webhook_json("x", "y", true, 0);
    check(retained_out.find("\"retain\":true") != std::string::npos, "retain:true rendered when retained");

    std::string escaped_out = nshmqtt::build_webhook_json("x", "a\"b", false, 0);
    check(escaped_out.find("\"payload\":\"a\\\"b\"") != std::string::npos,
          "payload is JSON-escaped, so an arbitrary MQTT payload can't break the envelope");
}

void test_substitute_event_placeholders_hex_lengths()
{
    std::string out8 = nshmqtt::substitute_event_placeholders("NSHMQTT_RANDOM_HEX8");
    check(out8.size() == 8, "NSHMQTT_RANDOM_HEX8 produces exactly 8 hex characters");
    check(out8.find_first_not_of("0123456789abcdef") == std::string::npos, "HEX8 output is all lowercase hex digits");

    check(nshmqtt::substitute_event_placeholders("NSHMQTT_RANDOM_HEX16").size() == 16,
          "NSHMQTT_RANDOM_HEX16 produces exactly 16 hex characters");
    check(nshmqtt::substitute_event_placeholders("NSHMQTT_RANDOM_HEX32").size() == 32,
          "NSHMQTT_RANDOM_HEX32 produces exactly 32 hex characters");
    check(nshmqtt::substitute_event_placeholders("NSHMQTT_RANDOM_HEX64").size() == 64,
          "NSHMQTT_RANDOM_HEX64 produces exactly 64 hex characters");
}

void test_substitute_event_placeholders_uuid()
{
    std::string out = nshmqtt::substitute_event_placeholders("NSHMQTT_RANDOM_UUID");
    check(out.size() == 36, "NSHMQTT_RANDOM_UUID produces a 36-character string");
    check(out.size() == 36 && out[8] == '-' && out[13] == '-' && out[18] == '-' && out[23] == '-',
          "UUID dashes land at the canonical 8-4-4-4-12 positions");
    check(out.size() == 36 && out[14] == '4', "UUID version nibble is 4 (UUIDv4)");
    check(out.size() == 36 && (out[19] == '8' || out[19] == '9' || out[19] == 'a' || out[19] == 'b'),
          "UUID variant nibble is one of 8/9/a/b (RFC 4122 variant)");
}

void test_substitute_event_placeholders_timestamps()
{
    std::string out_s = nshmqtt::substitute_event_placeholders("NSHMQTT_TIMESTAMP_S");
    check(!out_s.empty() && out_s.find_first_not_of("0123456789") == std::string::npos,
          "NSHMQTT_TIMESTAMP_S is replaced with a plain decimal integer");
    check(out_s.size() >= 10, "NSHMQTT_TIMESTAMP_S looks like a real epoch-seconds value (>= 10 digits)");

    std::string out_ms = nshmqtt::substitute_event_placeholders("NSHMQTT_TIMESTAMP_MS");
    check(out_ms.size() > out_s.size(), "NSHMQTT_TIMESTAMP_MS has more digits than NSHMQTT_TIMESTAMP_S");

    std::string out_us = nshmqtt::substitute_event_placeholders("NSHMQTT_TIMESTAMP_US");
    check(out_us.size() > out_ms.size(), "NSHMQTT_TIMESTAMP_US has more digits than NSHMQTT_TIMESTAMP_MS");

    std::string out_ns = nshmqtt::substitute_event_placeholders("NSHMQTT_TIMESTAMP_NS");
    check(out_ns.size() > out_us.size(), "NSHMQTT_TIMESTAMP_NS has more digits than NSHMQTT_TIMESTAMP_US");
}

void test_substitute_event_placeholders_datetime()
{
    std::string dt = nshmqtt::substitute_event_placeholders("NSHMQTT_DATETIME_UTC");
    check(dt.size() == 20 && dt[4] == '-' && dt[7] == '-' && dt[10] == 'T' && dt[13] == ':' && dt[16] == ':' &&
              dt[19] == 'Z',
          "NSHMQTT_DATETIME_UTC matches YYYY-MM-DDTHH:MM:SSZ");

    std::string date = nshmqtt::substitute_event_placeholders("NSHMQTT_DATE_UTC");
    check(date.size() == 10 && date[4] == '-' && date[7] == '-', "NSHMQTT_DATE_UTC matches YYYY-MM-DD");

    std::string time = nshmqtt::substitute_event_placeholders("NSHMQTT_TIME_UTC");
    check(time.size() == 8 && time[2] == ':' && time[5] == ':', "NSHMQTT_TIME_UTC matches HH:MM:SS");
}

void test_substitute_event_placeholders_reuse_and_passthrough()
{
    std::string out = nshmqtt::substitute_event_placeholders("NSHMQTT_RANDOM_HEX8-NSHMQTT_RANDOM_HEX8");
    check(out.size() == 17, "two HEX8 tokens in one payload both get replaced (correct total length)");
    check(out.size() == 17 && out.substr(0, 8) != out.substr(9, 8),
          "two occurrences of the same token get two independently generated values, not one reused");

    check_eq(nshmqtt::substitute_event_placeholders("{\"status\":\"completed\"}"), "{\"status\":\"completed\"}",
             "text with no recognized tokens is left completely unchanged");

    std::string mixed = nshmqtt::substitute_event_placeholders("id=NSHMQTT_RANDOM_HEX8;done");
    check(mixed.size() == 16 && mixed.substr(0, 3) == "id=" && mixed.substr(11) == ";done",
          "a token embedded in surrounding text is replaced in place, surrounding text untouched");
}

void test_render_prometheus_service_metrics_basic()
{
    nshmqtt::Metrics m;
    m.record_request("/metric/x");
    m.record_response(200);
    std::string out = nshmqtt::render_prometheus_service_metrics("nshmqtt_", m, "0.1.0", 12.5, true, 1, 1, 0, 1, 0, 0);
    check(out.find("nshmqtt_mqtt_connected 1") != std::string::npos, "connection state rendered");
    check(out.find("nshmqtt_mqtt_connections_active 1") != std::string::npos, "active connection count rendered");
    check(out.find("nshmqtt_state_entries 1") != std::string::npos, "state entry count rendered");
    check(out.find("nshmqtt_webhook_queue_depth 0") != std::string::npos, "webhook queue depth rendered");
    check(out.find("nshmqtt_http_requests_total{category=\"metric\"} 1") != std::string::npos,
          "request counter rendered");
    check(out.find("server1_load") == std::string::npos,
          "service metrics never include a per-topic state value series");
}

void test_render_prometheus_state_metrics_basic()
{
    std::vector<std::pair<std::string, double>> normalized = {{"nshmqtt_server1_load", 17.3}};
    std::string out = nshmqtt::render_prometheus_state_metrics(normalized);
    check(out.find("nshmqtt_server1_load 17.3") != std::string::npos, "state value rendered as its own series");
    check(out.find("mqtt_connected") == std::string::npos, "state metrics never include service-level series");
}

void test_render_health_json_basic()
{
    std::string out = nshmqtt::render_health_json("0.1.0", 5.0, false, 0, 2, 3);
    check(out.find("\"status\":\"ok\"") != std::string::npos, "health JSON reports ok status");
    check(out.find("\"mqtt_connected\":false") != std::string::npos, "health JSON reports mqtt connection state");
    check(out.find("\"mqtt_pool_size\":2") != std::string::npos, "health JSON reports configured pool size");
    check(out.find("\"state_entries\":3") != std::string::npos, "health JSON reports state entry count");
}

} // namespace

int main()
{
    test_format_double_natural_precision();
    test_parse_double_roundtrip();
    test_parse_int_basic();
    test_parse_bool_basic();
    test_constant_time_equals();
    test_json_escape();

    test_extract_json_number_field();
    test_parse_flat_number_object();
    test_flatten_json_object_numbers();
    test_flatten_json_object_booleans();
    test_flatten_json_object_nested();
    test_flatten_json_object_deeply_nested();
    test_flatten_json_object_ignores_strings_nulls_arrays();
    test_flatten_json_object_malformed();

    test_parse_http_request_basic();
    test_parse_http_request_with_body_headers();
    test_parse_http_request_chunked_flagged();
    test_parse_http_request_malformed();
    test_parse_http_request_captures_api_key();
    test_parse_http_request_captures_qos_retain_headers();
    test_accept_wants_json();

    test_config_defaults();
    test_config_load_and_env_override();
    test_config_rejects_bad_qos();
    test_config_missing_file_is_error();

    test_state_store_basic();
    test_state_store_persistence_roundtrip();
    test_state_store_load_missing_file_not_an_error();

    test_metrics_record_request();
    test_metrics_record_response();
    test_normalize_metric_name();
    test_normalize_metric_name_topic_prefix();
    test_normalize_all_collision_detection();
    test_normalize_all_topic_prefix();
    test_topic_matches_filter_exact_and_wildcards();
    test_topic_matches_any();
    test_build_webhook_json_basic();
    test_substitute_event_placeholders_hex_lengths();
    test_substitute_event_placeholders_uuid();
    test_substitute_event_placeholders_timestamps();
    test_substitute_event_placeholders_datetime();
    test_substitute_event_placeholders_reuse_and_passthrough();
    test_render_prometheus_service_metrics_basic();
    test_render_prometheus_state_metrics_basic();
    test_render_health_json_basic();

    // NSHMQTT_TEST_FORCE_FAIL=1: deliberately fails one check, for
    // nothing except verifying that a real test failure actually
    // propagates all the way out -- through this binary's own exit
    // code, test-container.sh's `docker run`, and the CI step that runs
    // it -- rather than trusting that chain by reasoning alone. Off
    // (unset) in every normal run; nothing in this repo ever sets it.
    // Uses the same parse_bool() every other on/off setting in this
    // project uses -- getenv() != nullptr alone would also trigger on
    // NSHMQTT_TEST_FORCE_FAIL="" (set but empty) or "0", neither of
    // which anyone setting this would actually mean as "yes, fail."
    const char *force_fail_env = std::getenv("NSHMQTT_TEST_FORCE_FAIL");
    bool force_fail = false;
    if (force_fail_env != nullptr)
    {
        nshmqtt::parse_bool(force_fail_env, force_fail);
    }
    if (force_fail)
    {
        check(false, "deliberate failure via NSHMQTT_TEST_FORCE_FAIL (not a real bug -- see this check's own comment)");
    }

    std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
