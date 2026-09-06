// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Just enough HTTP/1.1 to serve one request per connection: parse the
// request line, query string, and the request headers nshmqtt actually
// looks at (Accept, Content-Type, Content-Length, Transfer-Encoding,
// X-Mqtt-Api-Key, X-Mqtt-Qos, X-Mqtt-Retain) -- everything else is
// ignored. No keep-alive, no chunked transfer, no
// pipelining -- every response carries "Connection: close" and the socket
// is closed right after. This is deliberately not a general-purpose HTTP
// implementation; NGINX is the normal front end and already speaks full
// HTTP/1.1 to the outside world (see README).

#include <string>
#include <utility>
#include <vector>

namespace nshmqtt
{

enum class ReadResult
{
    Ok,
    TooLarge,
    ConnectionClosed,
    Timeout,
    IoError
};

// Reads from `fd` until an empty line (end of headers) is seen or
// `max_bytes` is exceeded. The returned buffer may contain some or all of
// the request body too, if it arrived in the same underlying recv() calls
// as the header block -- see parse_http_request()'s `leftover_body` output,
// which is how a caller recovers those bytes. A receive timeout is applied
// so a slow/stalled client cannot tie up a worker thread indefinitely.
ReadResult read_http_head(int fd, std::size_t max_bytes, int timeout_seconds, std::string &out);

// Reads exactly `need_bytes` more bytes from `fd`, appending them to
// `body`. Used after parse_http_request() to read whatever of the body
// didn't already arrive in read_http_head()'s buffer. Same timeout
// semantics as read_http_head().
ReadResult read_http_body(int fd, std::size_t need_bytes, int timeout_seconds, std::string &body);

struct HttpRequest
{
    std::string method;
    std::string path; // decoded, no query string
    std::vector<std::pair<std::string, std::string>> query;
    std::string accept;        // raw Accept header value, empty if absent
    std::string content_type;  // raw Content-Type header value, empty if absent
    long content_length = -1;  // -1 = no Content-Length header present
    bool chunked = false;      // Transfer-Encoding: chunked was present -- not supported, caller should reject
    std::string auth_token;    // raw X-Mqtt-Api-Key header value, empty if absent -- see config.h's http_auth_tokens
    std::string qos_header;    // raw X-Mqtt-Qos header value, empty if absent -- see server.cpp's handle_event()
    std::string retain_header; // raw X-Mqtt-Retain header value, empty if absent -- see handle_event()
    std::string body;          // filled in by the caller once the full body has been read
};

// Parses the request line, query string, and the headers listed above out
// of the raw bytes returned by read_http_head(). Returns false if the
// request line is malformed (wrong number of tokens, missing leading '/',
// bad percent-encoding, not an HTTP/1.x request line) or Content-Length is
// present but not a valid non-negative integer. `leftover_body` receives
// whatever bytes followed the header-terminating blank line in `raw` --
// empty if none arrived yet, and possibly already the complete body for a
// small request.
bool parse_http_request(const std::string &raw, HttpRequest &req, std::string &leftover_body);

// Looks up `key` in a parsed request's query parameters; returns false if
// not present.
bool find_query_param(const HttpRequest &req, const std::string &key, std::string &value);

// The response representations nshmqtt can produce. Prometheus is never
// negotiated -- it's set directly by the /metrics handler regardless of
// Accept, since a Prometheus scraper expects exactly that format.
enum class ResponseFormat
{
    Json,
    Text,
    Prometheus
};

// True if `accept_header` explicitly asks for JSON (case-insensitive
// substring match on "application/json"). Used to pick between a minimal
// text body and a JSON body for endpoints (like /health) whose default is
// plain text -- most health-check clients never send an Accept header at
// all and only look at the status code.
bool accept_wants_json(const std::string &accept_header);

struct HttpResponse
{
    int status = 200;
    ResponseFormat format = ResponseFormat::Json;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
};

// Serializes a response to raw bytes ready to write() to the socket.
// Content-Length and Connection are added automatically; `headers` should
// contain only the extra response headers. When `include_body` is true,
// Content-Type (from resp.format) is added too and the body bytes are
// written; when false (used for HEAD), there is no body to describe, so
// Content-Type is left off, while Content-Length still reflects
// resp.body's real size.
std::string build_http_response(const HttpResponse &resp, bool include_body = true);

// Convenience for the small error body used by every non-200 response, in
// whichever format the caller picked: {"error":"..."} for Json, or a
// single "error=..." line for Text.
HttpResponse make_error_response(int status, const std::string &message, ResponseFormat format);

} // namespace nshmqtt
