nshmqtt — Lightweight HTTP-to-MQTT gateway and MQTT-to-Prometheus/webhook bridge

A small C++ service that lets applications without an MQTT library or a persistent MQTT connection publish MQTT
events and state using plain HTTP requests. It can also subscribe to MQTT topics, keep track of current values,
expose them as Prometheus metrics, and forward matching messages to an HTTP(S) webhook as JSON.

```text
HTTP request -> nshmqtt -> MQTT publish
MQTT message -> nshmqtt -> current-state store -> Prometheus
MQTT message -> nshmqtt -> HTTP(S) webhook (JSON)
```

`nshmqtt` is not an MQTT broker -- it is glue between HTTP-capable applications and MQTT infrastructure.

## Contents

- [Architecture](#architecture)
- [Getting data in and out (start here)](#getting-data-in-and-out-start-here)
- [Design philosophy](#design-philosophy)
- [Dependencies](#dependencies)
- [Compilation](#compilation)
- [Configuration](#configuration)
- [HTTP API](#http-api)
  - [Events](#events)
  - [Current state / metrics](#current-state--metrics)
  - [Deleting state](#deleting-state)
  - [Simple URL API](#simple-url-api)
- [Authentication](#authentication)
- [Headers](#headers)
- [MQTT connectivity](#mqtt-connectivity)
- [MQTT subscriptions](#mqtt-subscriptions)
- [Webhook forwarding](#webhook-forwarding)
- [State persistence](#state-persistence)
- [Prometheus support](#prometheus-support)
- [Health endpoint](#health-endpoint)
- [curl examples](#curl-examples)
- [MQTT examples](#mqtt-examples)
- [NGINX example](#nginx-example)
- [TLS](#tls)
- [Mosquitto test commands](#mosquitto-test-commands)
- [Concurrency model](#concurrency-model)
- [Installation](#installation)
- [Docker](#docker)
- [Security considerations](#security-considerations)
- [Testing](#testing)
- [Non-goals](#non-goals)

## Architecture

```text
                         HTTPS
                           |
                           v
                         NGINX
               TLS / Auth / ACL / Logging
                           |
                    HTTP over AF_UNIX
                           |
                           v
                /run/nshmqtt/nshmqtt.sock
                           |
                           v
                        nshmqtt
                       /       \
                      /         \
                   MQTT       metrics
                     |
                     v
                 MQTT broker
```

The UNIX domain socket is the primary HTTP interface -- there is no requirement for nshmqtt to expose a TCP port. An
optional TCP listener (`tcp_port=`, off by default) exists for containers, network namespaces, and testing; it has
no TLS of its own.

The diagram above shows nshmqtt as an MQTT *publisher* (the write path from HTTP), but it's a *subscriber* too, in
two independent ways: [MQTT subscriptions](#mqtt-subscriptions) (into the current-state store) and
[Webhook forwarding](#webhook-forwarding) (onward to an HTTP(S) endpoint) -- either, both, or neither can be
enabled, watching the same or different topics.

`nshmqtt` implements application semantics (events, current state, Prometheus mapping). NGINX implements network
exposure and transport security (HTTPS, client auth, ACLs, rate limiting, access logging). This split is deliberate
-- see [Security considerations](#security-considerations).

## Getting data in and out (start here)

This section assumes no prior MQTT knowledge and is the map for everything else in this document -- each option
below links to the section with the full detail.

### MQTT in plain terms

- **Broker**: the MQTT server everything connects to (in this project's compose stack, the `mosquitto` container).
  `nshmqtt` is not a broker itself -- it's a client of one, the same as everything else that talks MQTT to it.
- **Topic**: a named channel, written like a path (`sensors/kitchen/temperature`). Nothing needs to be created
  ahead of time -- publishing to a topic that's never been used before just starts it.
- **Publish**: send a value to a topic.
- **Subscribe**: ask the broker to deliver values published to a topic, or a wildcard pattern of topics (`#` means
  "every topic").
- **Retain**: a publish can be marked "retained" -- the broker remembers only the most recent retained value per
  topic and hands it to any new subscriber immediately, even one that connects long after the publish happened. An
  un-retained publish is only ever seen by whoever is already subscribed at that exact moment.
- **QoS**: how hard the broker/client try to guarantee delivery (`0` = no guarantee, `1` = at least once, may
  duplicate, `2` = exactly once, slowest). The default (`1`) is fine for most use cases.

### Getting a value IN -- three ways

| Way                 | You run                               | Reaches current state?  | Retained?        |
| ------------------- | ------------------------------------- | ----------------------- | ---------------- |
| HTTP event          | `curl -X POST .../event/<topic>`      | Numeric only, see below | Off by default   |
| HTTP metric write   | `curl -X PUT .../metric/<name>`       | Always                  | Always           |
| Native MQTT publish | `mosquitto_pub -t <topic> -m <value>` | If subscribed + numeric | Your `-r` choice |

1. **HTTP event** -- something that happened, e.g. "a backup finished":
   ```bash
   curl -X POST 'http://localhost:8081/event/domino/server1/backup' -d '{"status":"completed"}'
   ```
   No MQTT knowledge needed to use this one. See [Events](#events).

2. **HTTP metric write** -- a value that IS something right now, e.g. current load:
   ```bash
   curl -X PUT 'http://localhost:8081/metric/server1/load' -d '17.3'
   ```
   Also no MQTT knowledge needed. See [Current state / metrics](#current-state--metrics).

3. **Native MQTT publish** -- for a sensor, device, or service that already speaks MQTT directly; no HTTP call at
   all, nshmqtt just happens to be listening:
   ```bash
   mosquitto_pub -h localhost -p 1883 -t 'sensors/kitchen/temperature' -m '21.4'
   ```
   Requires this (already on by default in `docker-compose.yml`):
   ```ini
   subscribe_enabled=true
   subscribe_topics=#
   ```
   See [MQTT subscriptions](#mqtt-subscriptions) for the full explanation, and [MQTT examples](#mqtt-examples) for
   more commands.

### Getting a value OUT -- four ways

| Way                         | You run                    | Shows                                              |
| --------------------------- | -------------------------- | -------------------------------------------------- |
| Prometheus, service metrics | `curl .../metrics`         | Request counters, MQTT status -- never content     |
| Prometheus, content metrics | `curl .../metrics-state`   | One series per value, from either path above       |
| Native MQTT subscribe       | `mosquitto_sub -t <topic>` | Live values, retained one immediately on subscribe |
| Health                      | `curl .../health`          | up/down + MQTT connection status, not values       |

1. **Prometheus service metrics** -- is nshmqtt itself healthy, is it connected to the broker:
   ```bash
   curl http://localhost:9100/metrics
   ```
2. **Prometheus content metrics** -- the actual current values, whichever way they got in:
   ```bash
   curl http://localhost:9100/metrics-state
   ```
   See [Prometheus support](#prometheus-support).
3. **Native MQTT subscribe** -- watch values directly on the broker, no Prometheus/HTTP involved:
   ```bash
   mosquitto_sub -h localhost -p 1883 -t '#' -v
   ```
   See [MQTT examples](#mqtt-examples) and [Mosquitto test commands](#mosquitto-test-commands).
4. **Health** -- is the service up at all:
   ```bash
   curl http://localhost:8081/health
   ```
   See [Health endpoint](#health-endpoint).

### A third thing: pushing MQTT onward to a webhook

Distinct from both tables above -- not a client pulling a value out of nshmqtt, but nshmqtt itself pushing a
message onward to an HTTP(S) endpoint you configure, as JSON, the moment a matching MQTT message arrives:

```ini
webhook_enabled=true
webhook_url=https://example.com/hook
webhook_topics=sensors/#
```

Independent of everything above -- its own topic list, on or off on its own. See
[Webhook forwarding](#webhook-forwarding) for the JSON shape, auth, and TLS options.

## Design philosophy

Simple things MUST be simple. Structured things MAY use JSON.

Publishing a plain value like `23.4` never requires constructing JSON. At the same time, a client producing
structured data can submit arbitrary JSON and nshmqtt will not interpret or rewrite it -- an event's HTTP body is
published to MQTT byte-for-byte unchanged. JSON is an application payload format, not something the gateway
imposes.

Two operations look similar but mean different things, and nshmqtt keeps them distinct throughout:

```text
Event:            a thing that happened, transient, never retained
Current state:    a value that IS something right now, may be retained
```

## Dependencies

- A C++17 compiler (`g++`)
- [Eclipse Paho MQTT C](https://github.com/eclipse-paho/paho.mqtt.c) (`libpaho-mqtt3c`, library + development
  headers) -- the synchronous `MQTTClient` API, not `MQTTAsync`
- [libcurl](https://curl.se/libcurl/) (library + development headers) -- used only by
  [Webhook forwarding](#webhook-forwarding)'s outbound HTTP(S) POSTs, via `HttpClient` (`src/httpclient.h/cpp`), a
  small RAII wrapper around curl's "easy" API
- POSIX threads (`pthread`)
- Linux (uses `signalfd`, `accept4`; not portable to other platforms by design)

No HTTP framework, no JSON library, no Prometheus client library -- nshmqtt implements just enough HTTP/1.1
parsing/serialization, JSON input/output, and Prometheus text exposition format for its own endpoints (its own
outbound webhook requests are the one place libcurl does the HTTP work instead). See [NOTICE](NOTICE) for Eclipse
Paho MQTT C's own license attribution.

## Compilation

```bash
make          # builds ./nshmqtt
make test     # builds and runs tests/test_nshmqtt (unit tests)
sudo make install   # binary + starter config only, see Installation below
```

On Debian/Ubuntu: `apt install g++ make libpaho-mqtt-dev libcurl4-openssl-dev`. On Alpine:
`apk add g++ make paho-mqtt-c-dev curl-dev` (see [Docker](#docker) for why the Alpine build looks a little
different from a plain `make`).

## Configuration

Simple `key=value` format: blank lines and `#` comments ignored, unknown keys logged as a warning (not fatal),
every key overridable by an `NSHMQTT_<KEY>` environment variable, environment takes precedence over the file. See
[`etc/nshmqtt.conf.example`](etc/nshmqtt.conf.example) for the full annotated list. `nshmqtt --help` prints the
same list with its config-key/env-var/description columns.

The MQTT broker location is never assumed to be `127.0.0.1` by nshmqtt's own design (even though that's a common
deployment) -- set `mqtt_host` explicitly for anything else: another container, another host on the LAN, or a
remote broker over the internet. For a remote broker that requires TLS, keep nshmqtt's own Paho build TLS-free (see
[Dependencies](#dependencies)) and put an NGINX `stream` proxy in front instead:

```text
nshmqtt --plain MQTT--> 127.0.0.1:<local port> --NGINX stream--> MQTT/TLS --> remote broker:8883
```

## HTTP API

### Events

```http
POST /event/<topic>
```

The request body is published to MQTT on `<topic>` unchanged -- JSON, plain text, or any other content. By default
events are published at `mqtt_qos` (the configured default) with the MQTT retain flag off.

```http
POST /event/domino/server1/backup
Content-Type: application/json

{"status": "completed", "duration": 127}
```

publishes topic `domino/server1/backup`, payload `{"status": "completed", "duration": 127}` (byte-for-byte).

**`qos` and `retain` override the publish for that one request**, on both `POST` and the simple `GET` form below --
as either a query parameter or a header, whichever is more convenient for the caller:

|        | Query parameter | Header                |
| ------ | --------------- | --------------------- |
| QoS    | `?qos=2`        | `X-Mqtt-Qos: 2`       |
| Retain | `?retain=true`  | `X-Mqtt-Retain: true` |

Since a `POST` body is the payload itself (published byte-for-byte), neither can ever be a body field -- that would
mean interpreting part of the payload as control data, which nshmqtt deliberately never does (see
[Design philosophy](#design-philosophy)). If a request somehow sends both forms for the same one, **the header
wins**. Query parameter form:

```http
POST /event/domino/server1/backup?qos=2&retain=true
Content-Type: application/json

{"status": "completed", "duration": 127}
```

Header form, identical effect:

```http
POST /event/domino/server1/backup
Content-Type: application/json
X-Mqtt-Qos: 2
X-Mqtt-Retain: true

{"status": "completed", "duration": 127}
```

`qos` accepts `0`, `1`, or `2`; anything else is `400`. `retain` accepts `true`/`false`, `1`/`0`, `yes`/`no`, or
`on`/`off` (case-insensitive); anything else is `400`. Both are optional and independent -- set either, both, or
neither. The response echoes back what was actually used:

```json
{"topic":"domino/server1/backup","payload_bytes":37,"qos":2,"retain":true,"published":true}
```

This is deliberately **not** available on `PUT /metric` -- see [Current state / metrics](#current-state--metrics)
for why state writes keep retain fixed at `true` rather than letting a caller turn it off by accident.

A publish failure (broker unreachable, publish queue full, or no acknowledgement within
`mqtt_publish_timeout_seconds`) is a real HTTP error for an event -- `502`/`503`/`504` respectively -- since MQTT
is the only channel an event has.

**An event does not show up in `GET /metrics-state`, on its own.** `POST /event` only publishes to MQTT -- it
never touches the current-state store directly, unlike `PUT /metric` below. Publishing the `domino/server1/backup`
example above and then checking `/metrics-state` will not show a `domino_server1_backup` series, even though the
publish itself succeeded (`"published":true` in the response). This can look surprising with `subscribe_enabled=true`
and `subscribe_topics=#` (the `docker-compose.yml` default), since nshmqtt's own subscription sees every event it
publishes come right back over the broker -- but the JSON body `{"status":"completed","duration":127}` isn't a bare
number, so [MQTT subscriptions](#mqtt-subscriptions)' own rule applies: a matched message whose payload isn't a bare
number is simply ignored for state purposes. The reverse is also true and worth knowing before it surprises you the
other way: an event whose body *is* a bare number, published to a topic that matches `subscribe_topics`, **will**
end up in `/metrics-state` -- from the subscription's point of view, nshmqtt's own event publish is indistinguishable
from any other matching MQTT message on the broker. If you want an event's topic to never affect state regardless of
its payload, keep `subscribe_topics` narrower than the topics you use for events, or use a separate topic namespace
for each (e.g. `events/...` vs. the topics you publish metrics to).

### Current state / metrics

```http
PUT /metric/<name>
```

Sets the current value of `<name>`. Accepts a plain numeric body:

```http
PUT /metric/server1/load
Content-Type: text/plain

17.3
```

or a small JSON object with a `value` field:

```http
PUT /metric/server1/load
Content-Type: application/json

{"value": 17.3}
```

(The spec deliberately keeps this minimal -- no metadata fields beyond `value` in the initial implementation.)
Either form:

- updates the in-memory current-state store (visible in `/metrics`)
- persists it to `state_file`, if `state_enabled` (default: on)
- publishes it to MQTT on `<name>`, **with the retain flag set**

A metric write always answers `200` if the value parsed and the local state update succeeded, **even if the MQTT
publish itself failed** -- state has its own persistence and doesn't depend on the broker being reachable right
now. The response body reports whether the MQTT side actually succeeded:

```json
{"name":"server1/load","value":17.3,"mqtt_published":true}
```

This is the one place nshmqtt's behavior deliberately differs between the two operation types: an event has no
channel other than MQTT, so its publish failing is a real error; a metric write already has a local result
independent of MQTT, so it isn't.

### Deleting state

```http
DELETE /metric/<name>
```

Removes `<name>` from the in-memory store, from `state_file`, and publishes an empty retained message to `<name>`
on the broker -- the standard MQTT convention for "no retained message here anymore," so a new subscriber doesn't
see a value nshmqtt itself now considers gone. Answers `404` if `<name>` was never set (distinguishing "no update,
keep the previous value" from "this state no longer exists").

### Simple URL API

For clients too primitive to construct a proper request body (old applications, simple webhook tools, `curl`
one-liners, monitoring scripts):

```http
GET /event/<topic>?value=<value>
GET /metric/<name>?value=<value>
```

equivalent to the `POST`/`PUT` forms above, with `value` (plain text only -- no JSON form here) as the body. This
is a side-effecting `GET`, which is not normal HTTP semantics, so it's controlled by `simple_get=` (default:
`true`) and can be turned off. `GET /event/<topic>` also accepts `qos`/`retain`, same as `POST /event` (see
[Events](#events)), as either query parameters (`?qos=2&retain=true`) or headers (`X-Mqtt-Qos`/`X-Mqtt-Retain`) --
e.g. `GET /event/server1/status?value=completed&qos=2&retain=true`.

## Authentication

Two independent, stackable layers -- neither substitutes for the other:

- **NGINX** checks the standard `Authorization` header (mTLS, basic auth, `auth_request` against something else,
  whatever fits your environment -- see [NGINX example](#nginx-example)).
- **nshmqtt itself** checks a deliberately different header, `X-Mqtt-Api-Key`, against `http_auth_tokens` (a
  comma-separated list of valid tokens, each caller can get its own):

```ini
http_auth_tokens=some-long-random-token,another-token-for-a-different-caller
```

Using two different headers means both checks can run at once without one interfering with the other -- NGINX can
consume/require `Authorization` however it wants while nshmqtt independently validates its own `X-Mqtt-Api-Key`, and
neither has to know about the other's scheme. Empty/unset (default) disables nshmqtt's own check entirely --
nothing changes from today's behavior.

**All of nshmqtt's own custom headers share the `X-Mqtt-` prefix and the same casing style**: `X-Mqtt-Api-Key`
(this section), and `X-Mqtt-Qos`/`X-Mqtt-Retain` (see [Events](#events)) -- there are no others. Header names are
case-insensitive per the HTTP spec (RFC 7230) and nshmqtt matches them that way internally, so `x-mqtt-api-key` or
`X-MQTT-API-KEY` work identically to `X-Mqtt-Api-Key` on the wire; the mixed-case form shown throughout this
document is just the conventional style (matching `Content-Type`, `Authorization`, and how most other services
document their own custom headers), not a requirement.

The reason nshmqtt has its own check at all, not just NGINX's: nshmqtt's optional `tcp_port` listener, if a
deployment enables it, can be reached directly with no NGINX in the path -- NGINX-only auth protects nothing there.
The `docker-compose.yml` stack in this project does enable it (nshmqtt's `/metrics` and `/metrics-state` are routed
over TCP `9100` rather than the UNIX socket, see [Docker](#docker)) -- not published to the host directly, but
reachable from any other container on the compose network, not just NGINX, which is exactly the scenario nshmqtt's
own check is for. Token comparison is constant-time (`text_util.h`'s `constant_time_equals()`) so response timing
can't be used to guess a valid token byte-by-byte.

When configured, every payload-bearing endpoint requires a valid token: `/event/*`, `/metric/*`, and
`/metrics-state`. `/health` and the service-only `/metrics` are exempt on purpose -- neither carries a caller's own
data, and both need to stay reachable without a token (a container health check or Prometheus's own scrape of
service metrics has no way to supply one). A missing or invalid token gets `401`.

**The token always goes in the `X-Mqtt-Api-Key` header -- never in the JSON body, never as a query parameter.** This
isn't just a style preference: an event's request body is published to MQTT byte-for-byte unchanged (see
[Design philosophy](#design-philosophy), "preserve payloads"). A token embedded in the body would get published
straight onto the broker along with it -- visible to every subscriber on that topic, retained in the broker's own
logs, potentially forwarded onward by a bridge. A query parameter fares little better: it routinely ends up in
NGINX/proxy access logs and shell history (`curl` command lines, `?value=...` in the simple URL API). A header is
the one place a credential can travel with the request without also becoming part of the data the request is
about.

```bash
curl -X POST --unix-socket /run/nshmqtt/nshmqtt.sock \
  -H 'X-Mqtt-Api-Key: some-long-random-token' \
  -H 'Content-Type: application/json' \
  -d '{"status":"completed","duration":127}' \
  'http://localhost/event/domino/server1/backup'
```

## Headers

Every header nshmqtt recognizes, in one place -- everything else in a request is ignored, since this is
deliberately not a general-purpose HTTP server (see [Architecture](#architecture)).

**Custom, request headers nshmqtt defines** -- all share the `X-Mqtt-` prefix, matched case-insensitively (see
[Authentication](#authentication)):

| Header           | Endpoints                                 | Required?              | Also available as    |
| ---------------- | ----------------------------------------- | ---------------------- | -------------------- |
| `X-Mqtt-Api-Key` | `/event/*`, `/metric/*`, `/metrics-state` | If tokens set          | Nothing, header-only |
| `X-Mqtt-Qos`     | `/event/*` (`POST` + simple `GET`)        | No, default `mqtt_qos` | `?qos=`              |
| `X-Mqtt-Retain`  | `/event/*` (`POST` + simple `GET`)        | No, default off        | `?retain=`           |

`X-Mqtt-Qos`/`X-Mqtt-Retain` win over their query-parameter equivalent if a request somehow sends both -- see
[Events](#events). `X-Mqtt-Api-Key` has no query-parameter form at all: unlike `qos`/`retain`, a token is a secret,
and a query parameter routinely ends up in access logs and shell history -- see
[Authentication](#authentication)'s own note for the full reasoning, which also covers why it can never go in the
JSON body either.

**Standard request headers nshmqtt reads:**

| Header                       | Used for                                                                        |
| ---------------------------- | ------------------------------------------------------------------------------- |
| `Accept`                     | JSON vs. plain-text body, currently only `GET /health`                          |
| `Content-Type`               | JSON vs. plain body, `PUT /metric` only -- `POST /event` always publishes as-is |
| `Content-Length`             | Request body size; required on any request carrying one                         |
| `Transfer-Encoding: chunked` | Not supported -- rejected outright                                              |

**Standard response headers nshmqtt sends:**

| Header              | When                       | Notes                                                             |
| ------------------- | -------------------------- | ----------------------------------------------------------------- |
| `Content-Type`      | Every response with a body | JSON, plain text, or Prometheus's own exposition-format value     |
| `Content-Length`    | Always                     | Real body size, even on a `HEAD` response with no body            |
| `Connection: close` | Always                     | No keep-alive, no pipelining -- see [Architecture](#architecture) |
| `Allow`             | Only on `405`              | Lists the methods actually accepted for that path                 |

## MQTT connectivity

Uses Eclipse Paho's classic synchronous `MQTTClient` API (`mqtt_host`, `mqtt_port`, `mqtt_client_id`, `mqtt_qos`,
`mqtt_keepalive_seconds`, `mqtt_username`/`mqtt_password`). A broker outage never crashes the HTTP service and never
turns into an unbounded queue of pending publishes -- see [Concurrency model](#concurrency-model) for exactly how a
publish degrades under a slow or unreachable broker, and what HTTP status each failure mode maps to.

## MQTT subscriptions

There are two distinct, independent ways data gets *into* nshmqtt's current-state store -- the same store
`GET /metrics-state` renders, regardless of which path a given value took to get there:

```text
   HTTP PUT /metric/<name>                    a native MQTT publish
   (a script, a Domino agent,                 (a sensor, an IoT device, another
    a webhook, LotusScript, ...)               service already on the broker)
              |                                            |
              v                                            v
     nshmqtt's HTTP API                       nshmqtt's own subscription
     (numeric body required,                  (subscribe_topics filter; a matched
      see Current state / metrics)             message whose payload isn't a bare
              |                                 number is simply ignored)
              |                                            |
              +--------------------+-----------------------+
                                    v
                  one shared current-state store (state.json)
                                    |
                                    v
                    GET /metrics-state  /  state_file  /  Prometheus textfile
```

`POST /event` is deliberately not on this diagram's left-hand side -- an event never updates state directly, the way
`PUT /metric` does. But it isn't fully outside this picture either: with `subscribe_topics=#` (the
`docker-compose.yml` default), nshmqtt's own subscription sees every event it publishes come back over the broker,
indistinguishable from any other MQTT message -- so an event whose body happens to be a bare number, on a topic
`subscribe_topics` matches, *does* still reach state, by the right-hand path, same as any other MQTT publish would.
See [Events](#events)'s own note for the concrete example. The diagram's two boxes are about how something enters
the state store, not which HTTP endpoint originally sent it.

This diagram is specifically about the current-state store -- [Webhook forwarding](#webhook-forwarding) is a
third, separate consumer of MQTT messages (its own `webhook_topics` list, its own on/off switch) that never
touches this store at all; it POSTs matching messages onward to an HTTP(S) endpoint instead.

It's worth being clear on which of the two paths applies to a given source:

1. **HTTP `PUT /metric/<name>`** (see [HTTP API](#http-api)) -- for anything that speaks HTTP but not MQTT: a
   Domino agent, a shell script, LotusScript, a webhook.
2. **A native MQTT publish**, picked up by nshmqtt's own subscription -- for anything that already speaks MQTT
   directly: a sensor, an IoT device, another service already publishing to your broker. Nothing needs to call
   nshmqtt at all; it's just another subscriber on topics it's told to watch.

```ini
subscribe_enabled=true
subscribe_topics=#
```

`subscribe_topics` is a comma-separated list of MQTT topic filters. Incoming messages update the same current-state
store HTTP metric writes use -- a message whose topic matches a filter but whose payload isn't a bare number is
simply ignored (not an error, not logged above debug level), which is expected and routine on a broad subscription
like `#` (plenty of real MQTT traffic on a shared broker isn't a number at all -- device status strings, JSON
config blobs, whatever else is on that broker). Narrower subscriptions are recommended for production, both to
avoid that noise and because `#` means "everything," including topics that have nothing to do with what you
actually want in Prometheus. The one exception to "isn't a bare number is ignored" is a topic explicitly listed in
`prometheus_mqtt_json_topics` -- see [Prometheus support](#prometheus-support) for JSON payload handling.

**`PUT /metric` writing back to itself, if `subscribe_topics` overlaps it.** With `subscribe_topics=#` (this
project's default), nshmqtt's own subscription also receives every publish nshmqtt itself just made via
`PUT /metric` -- that write always publishes with retain **on**, and a broad `#` filter matches it right back. This
is a double-write, not a loop: the second `state.set()` (from the subscription) writes the exact same value the
first one (from the HTTP request) just wrote -- there's no window for it to change in between, so it's guaranteed
identical, never a race. The subscribe handler only updates state, it never re-publishes to MQTT, so nothing
re-triggers a second round -- one bounce, then it stops. The only real cost is one redundant `state_file` rewrite
per matching write, which is harmless at the write rates this is meant for. If `subscribe_topics` never overlaps
the names you `PUT /metric` to -- e.g. keeping HTTP-written names and MQTT-native device topics in separate
namespaces (`metrics/#` vs. `sensors/#`) -- this never happens at all.

Publishing directly to the broker in the `docker-compose.yml` stack (rather than through nshmqtt's HTTP API) looks
like this -- `mosquitto_pub` here stands in for whatever device or service actually has the data. This assumes a
host-installed `mosquitto_pub`; see [Mosquitto test commands](#mosquitto-test-commands) for the `docker exec`
alternative (no host install needed) and for a bare-metal/systemd broker instead of this compose stack:

```bash
# plain, through NGINX's stream{} passthrough on 1883
mosquitto_pub -h localhost -p 1883 -t 'sensors/kitchen/temperature' -m '21.4'

# or over MQTT/TLS on 8883, once ./gen-cert.sh has populated tls/
mosquitto_pub -h localhost -p 8883 --cafile tls/ca.crt -t 'sensors/kitchen/temperature' -m '21.4'
```

With `subscribe_topics=#` (or anything matching `sensors/kitchen/temperature`), that value shows up in
`GET /metrics-state` as `mqtt_sensors_kitchen_temperature 21.4` within one publish -- no restart, no
reconfiguration, nothing on the nshmqtt side needs to know that device exists ahead of time. Any MQTT-native
device -- an existing sensor, an LED matrix display, anything already talking to your broker -- becomes a
Prometheus source this way, just by publishing to a topic nshmqtt is watching.

One asymmetry worth knowing: nshmqtt doesn't care whether an *incoming* message was published with the MQTT retain
flag set or not -- it reacts to any matching publish either way. But when nshmqtt itself writes state (an HTTP
`PUT /metric`), it always publishes with retain **on** (see [Current state / metrics](#current-state--metrics)).
If you want a device's own publishes to survive a broker restart or be visible to a subscriber that connects
later, that's the device's own `-r`/retain choice to make, same as it would be without nshmqtt in the picture at
all -- nshmqtt doesn't change how retain works on the broker for anyone else.

## Webhook forwarding

The other direction of the gateway: instead of (or alongside) tracking current state, forward MQTT messages to an
HTTP(S) webhook as JSON -- for piping MQTT traffic into something that only speaks HTTP (a Slack/Teams-style
incoming webhook, a serverless function, a logging/alerting pipeline, another internal service). Completely
independent of `subscribe_enabled`/`subscribe_topics` above -- either can be configured and run without the
other, with its own topic scope:

```ini
webhook_enabled=true
webhook_url=https://example.com/hook
webhook_topics=sensors/#,alerts/#
```

`webhook_topics` uses the same comma-separated MQTT topic filter syntax as `subscribe_topics`, but is matched
independently: nshmqtt subscribes to both lists (whichever are enabled) on the same underlying MQTT session, and
checks each arrived message's topic against each feature's own list itself (Paho never tells a caller which
subscription matched -- see `mqtt_topic.h`). Unlike `subscribe_enabled`, **every** matching message is forwarded,
not just numeric payloads -- a webhook receiver can be handed JSON, plain text, or anything else that was
published.

Each matching message becomes one JSON POST:

```json
{"topic":"sensors/kitchen/temperature","payload":"21.4","retain":false,"timestamp":1700000000}
```

`payload` is always a JSON string field, whatever the original MQTT payload actually was -- a number, JSON, plain
text, or arbitrary bytes -- so the envelope itself can never become invalid JSON regardless of what was published.
`retain` is the MQTT retain flag the message actually arrived with. `timestamp` is whole Unix seconds, at the
moment nshmqtt processed the message (not when it was originally published, if that differs).

**Auth header**, for a receiver that requires its own credential -- a single configurable header, not a fixed
scheme, since a webhook's own auth convention isn't nshmqtt's to assume:

```ini
webhook_auth_header=Authorization
webhook_auth_value=Bearer some-token
```

Both empty (default) sends no extra header. Sent on every request exactly as configured -- nshmqtt does not
interpret, refresh, or template it.

**TLS**, for an `https://` `webhook_url`: certificates are verified by default. `webhook_tls_insecure=true` skips
verification entirely -- local/self-signed testing only; using it against a real endpoint defeats the point of
`https://` in the first place.

```ini
webhook_timeout_seconds=5   # bounds one request (connect + transfer)
webhook_queue_size=256      # bounded queue between the MQTT subscribe worker and the webhook worker thread
```

**Delivery is fire-and-forget, same philosophy as an unreachable MQTT broker never blocking HTTP requests
elsewhere in this project.** A failed delivery (connection refused, timeout, non-2xx response) is logged and
counted (`webhook_failed_total`) -- never retried, never queued for later. A full queue (the webhook receiver
can't keep up, or is unreachable) drops the new message and counts it (`webhook_queue_full_total`) rather than
growing without bound. See [Concurrency model](#concurrency-model) for the same design applied to MQTT publishing.

**If `webhook_topics` and `subscribe_topics` overlap**, a message matching both could in principle be delivered
to nshmqtt's subscription more than once by the broker (MQTT's own spec permits, but doesn't require, delivering
a message once per overlapping subscription) -- not something nshmqtt tries to prevent, since a well-behaved
webhook receiver is expected to tolerate occasional duplicate delivery anyway, the same assumption any real-world
webhook integration (Stripe's, GitHub's, ...) already makes of its own receivers. If this matters to you, keep
the two topic lists disjoint (e.g. `subscribe_topics=metrics/#`, `webhook_topics=events/#`).

Trying it locally with a one-off HTTP receiver (Python's standard library, nothing to install):

```bash
python3 -c "
import http.server
class H(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get('Content-Length', 0))
        print(self.rfile.read(n).decode())
        self.send_response(200); self.end_headers()
http.server.HTTPServer(('127.0.0.1', 8000), H).serve_forever()
"
```

then `webhook_url=http://127.0.0.1:8000/hook` and publish to a topic in `webhook_topics` -- the JSON envelope
prints on the receiver's terminal as each message arrives.

## State persistence

Current state only -- not history, not a database:

```json
{"server1/load": 17.3, "room/temperature": 22.4}
```

Written atomically (temp file + `rename()`) on every change, to `state_file` (default
`/var/lib/nshmqtt/state.json`). No SQLite, no historical database -- see [Non-goals](#non-goals).

## Prometheus support

Two independent output modes, either or both may be enabled at once:

```ini
prometheus_http=true                              # GET /metrics and GET /metrics-state through the HTTP interface
prometheus_textfile=/var/lib/nshmqtt/nshmqtt.prom  # periodic atomic write, for node_exporter's textfile collector
```

**Split into two endpoints, by content.** `GET /metrics` is service-only -- request counters, MQTT connection/pool
status, queue depth, counts like `state_entries` and `metric_name_collisions` -- nothing derived from an actual
topic name or payload value, so it's always safe to leave open (Prometheus can scrape it with no token even when
`http_auth_tokens` is configured). `GET /metrics-state` is the actual content: one gauge series per current value.
That's real payload data, so it's gated behind `http_auth_tokens` when configured, same as `/event/*` and
`/metric/*` (see [Authentication](#authentication)). The textfile output writes both together into one file --
there's no HTTP-level access control to split there, it's a local file.

Neither requires (or creates) a separate TCP listener; both endpoints exist wherever the rest of the HTTP API
already does (the UNIX socket, and TCP too if `tcp_port` is enabled). The `docker-compose.yml` stack does enable
`tcp_port`, specifically so NGINX can route metrics scraping over a different connection than the write API and
`/health` -- see [Docker](#docker).

**Two independent name prefixes, not one.** `prometheus_prefix` names series on the service endpoint (`GET
/metrics`, default `nshmqtt_`); `prometheus_mqtt_state_prefix` names series on the content endpoint (`GET
/metrics-state`, default `mqtt_`) -- e.g. `nshmqtt_up` (service) vs. `mqtt_room_temperature` (content, from a topic
`room/temperature`). The `docker-compose.yml` stack sets `prometheus_mqtt_state_prefix` explicitly to `mqtt_` too,
even though that's now also the built-in default -- kept explicit there since it's the setting a Grafana
dashboard's queries actually depend on. If you'd rather have both halves share one namespace, set
`prometheus_mqtt_state_prefix=nshmqtt_` explicitly to restore the pre-default-change behavior.

*Why two separate settings*: `/metrics` is data about nshmqtt itself, `/metrics-state` is data nshmqtt is exposing
on behalf of whatever published it -- different enough in nature to default to visually distinct namespaces. A
single shared prefix also means a topic that happens to be named e.g. `up` or `mqtt_connected` normalizes to the
*exact same* Prometheus name as one of nshmqtt's own reserved service-metric names -- a real collision risk
(confirmed by testing: publishing to a topic literally named `up` produced a second, different `nshmqtt_up` series
alongside the real one before this split existed), especially sharp once both halves land together in one
`prometheus_textfile`. Splitting the prefixes closes that off entirely.

MQTT-style hierarchical names are normalized into Prometheus metric names deterministically: every byte that isn't
`[A-Za-z0-9_]` becomes `_`, and a leading digit gets an underscore inserted before it.

```text
server1/system/cpu/load  ->  <prefix>server1_system_cpu_load
```

**The topic name is written through one byte at a time -- full control, no hidden mapping.** This is the entire
transform: a single pass over the topic name's raw bytes, each one either kept as-is (`[A-Za-z0-9_]`) or replaced
with `_`. There's no lookup table, no Unicode/UTF-8 codepoint awareness, and no attempt to collapse repeated
underscores -- a topic byte-for-byte becomes a metric name byte-for-byte, so what you published is always
recognizable in what Prometheus shows. One consequence worth knowing: a multi-byte UTF-8 character in a topic name
(anything outside plain ASCII) has each of its bytes individually replaced with `_`, since the loop has no notion
of a "character," only bytes -- e.g. a topic containing `café` normalizes with the accented character's two
UTF-8 bytes each becoming a separate `_`, not one.

**Optional `state_topic_prefix`, off by default.** A just-in-case escape hatch, separate from
`prometheus_mqtt_state_prefix`: it's prepended to the raw topic name *before* the byte-by-byte substitution pass
above runs, so it goes through the exact same substitution as the rest of the name -- any character in the prefix
itself that isn't `[A-Za-z0-9_]` also becomes `_`, no special-casing. Empty (the default) means today's behavior,
completely unchanged.

```ini
state_topic_prefix=site1/
```

```text
cpu/load  ->  <prometheus_mqtt_state_prefix>site1_cpu_load
```

This only changes the rendered Prometheus name -- it has no effect on `state.json`, on `/metric/<name>`
addressing, or on `DELETE` semantics, all of which continue to use the topic name exactly as published.

**JSON payloads on explicitly configured topics, via `prometheus_mqtt_json_topics`.** Some devices publish their
entire state as one JSON object on a single topic rather than one bare number per topic -- an
[AWTRIX](https://blueforcer.github.io/awtrix3/) smart pixel clock's `awtrix/state/device` topic is a real example:

```json
{
  "batteryPercent": 91,
  "batteryVoltage": 4.12,
  "matrixPower": false,
  "wifi": {"enabled": true, "attempts": 0, "connects": 1}
}
```

nshmqtt does not parse arbitrary MQTT JSON payloads automatically -- that would mean guessing at every device's own
shape, and turning "ignored, not a bare number" into a much larger surface than the "no topic-to-metric mapping
configuration" rule just above already rules out. Instead, JSON-to-Prometheus expansion is opt-in per **exact**
topic name:

```ini
prometheus_mqtt_json_topics=awtrix/state/device,sensor/foo/state
```

This is a plain comma-separated list of literal topic strings, not filters -- no `+`/`#` wildcard matching here,
deliberately, for this first implementation. It also doesn't add a subscription of its own: a topic still only
reaches nshmqtt at all via `subscribe_enabled`/`subscribe_topics` as before (`subscribe_topics=#` continues to
subscribe to everything exactly as it always has) -- this list only changes how the payload of an already-received,
listed topic gets interpreted.

For a listed topic, the JSON object is recursively flattened into one state entry per leaf, joining nested keys
with `_`, and the example above becomes:

```text
batteryPercent      91
batteryVoltage      4.12
matrixPower         0
wifi_enabled        1
wifi_attempts       0
wifi_connects       1
```

Each leaf's name is combined with the topic itself (`<topic>/<leaf path>`, e.g. `awtrix/state/device/wifi_enabled`)
to form its state name, then normalized into a Prometheus name exactly like any other state entry -- same
byte-by-byte substitution, same `prometheus_mqtt_state_prefix`/`state_topic_prefix`, same collision detection (see
below); nothing about JSON topics gets a separate naming convention. Conversion rules for each JSON value found:

| JSON value                | Result                                                     |
| ------------------------- | ---------------------------------------------------------- |
| number (integer or float) | kept as a Prometheus numeric value                         |
| boolean                   | `1` (`true`) or `0` (`false`)                              |
| object                    | descended into; its own keys joined to the parent with `_` |
| string                    | ignored -- not added as a series                           |
| `null`                    | ignored                                                    |
| array                     | ignored completely, for now                                |

A message on a listed topic whose payload isn't valid JSON at all (or isn't an object) is dropped, logged at debug
level, and does not partially update any of that topic's existing leaves -- the same "reject the whole thing, don't
guess" behavior `state.json` loading itself uses. A JSON payload on a topic *not* in `prometheus_mqtt_json_topics`
is unaffected by any of this: if it also isn't a bare number, it's simply ignored, exactly as any other non-numeric
message on a broad subscription already was before this feature existed.

One thing this doesn't handle: if a later message on the same topic omits a field a previous one had, that field's
old state entry is not removed -- the same staleness behavior a plain numeric topic already has (nothing removes
an entry nshmqtt itself didn't delete). `DELETE /metric/<name>` still works on any individual flattened entry by
its full name if you need to clear one.

**Naming collisions are detected, not silently merged.** If two different topic names normalize to the same
Prometheus name under `prometheus_mqtt_state_prefix` (e.g. `room.temp` and `room_temp` both become
`mqtt_room_temp` in the `docker-compose.yml` stack), the alphabetically-first name keeps that series; every later
colliding name is dropped from `/metrics-state` and counted in `<prometheus_prefix>metric_name_collisions` (on the
service-only `/metrics`, under the *service* prefix, since the count itself carries no content) instead, so the
situation is visible to an operator rather than one value silently overwriting another under the same series
name. This was flagged early on as the one part of this design with real potential to grow into a rules engine if
handled carelessly -- the implementation deliberately stays this plain rather than adding any kind of
topic-to-metric mapping configuration.

## Health endpoint

```http
GET /health
```

Always `200` as long as the HTTP layer itself is up -- MQTT connectivity is reported in the body, not the status
code, since a temporarily unreachable broker is an expected, tolerated condition (see
[MQTT connectivity](#mqtt-connectivity)), not a reason to fail a container health check. Minimal text by default
(most health-check clients only look at the status code):

```text
status=ok
mqtt_connected=true
```

or JSON with `Accept: application/json`:

```json
{"status":"ok","version":"0.1.0","uptime_seconds":123.4,"mqtt_connected":true,"state_entries":3}
```

## curl examples

Through NGINX -- the normal way to reach nshmqtt (see [Architecture](#architecture)), and what these examples use:
the `docker-compose.yml` stack's plain-HTTP port. Swap in `https://localhost:8444` (with `--cacert tls/ca.crt`,
see [TLS](#tls)) for the TLS listener, or your own NGINX's address for a bare-metal deployment.

```bash
curl -X POST 'http://localhost:8081/event/domino/server1/backup' \
  -H 'Content-Type: application/json' -d '{"status":"completed","duration":127}'

# same event, overriding qos and retain for this one publish (see Events)
curl -X POST 'http://localhost:8081/event/domino/server1/backup?qos=2&retain=true' \
  -H 'Content-Type: application/json' -d '{"status":"completed","duration":127}'

# same override, as headers instead -- either form works, header wins if both are given
curl -X POST 'http://localhost:8081/event/domino/server1/backup' \
  -H 'Content-Type: application/json' -H 'X-Mqtt-Qos: 2' -H 'X-Mqtt-Retain: true' \
  -d '{"status":"completed","duration":127}'

curl -X PUT 'http://localhost:8081/metric/server1/load' -d '17.3'

curl -X DELETE 'http://localhost:8081/metric/server1/load'

curl 'http://localhost:8081/event/server1/status?value=completed'
curl 'http://localhost:8081/metric/kitchen/temperature?value=19.5'

# with http_auth_tokens configured, every call above also needs this header
curl -X PUT 'http://localhost:8081/metric/server1/load' -H 'X-Mqtt-Api-Key: some-long-random-token' -d '17.3'

# same calls, over TLS (once ./gen-cert.sh has populated tls/, see TLS)
curl --cacert tls/ca.crt -X PUT 'https://localhost:8444/metric/server1/load' -d '17.3'

# metrics are on their own dedicated port, not 8081 -- see Docker's own section
curl 'http://localhost:9100/metrics'
curl 'http://localhost:9100/metrics-state'
```

See [`examples/curl-example.sh`](examples/curl-example.sh) for a complete, runnable walkthrough of the whole API
against exactly this (including the expected error responses, and TLS/token support via `NSHMQTT_URL`/
`NSHMQTT_METRICS_URL`/`NSHMQTT_AUTH_TOKEN`) -- run it as-is against `docker compose up -d`, no setup beyond that.

Talking to nshmqtt's own UNIX socket directly -- its primary interface, for a bare-metal/systemd install on the
same host, or from inside the nshmqtt container itself -- looks the same, just addressed differently:

```bash
curl -X PUT --unix-socket /run/nshmqtt/nshmqtt.sock \
  'http://localhost/metric/server1/load' -d '17.3'
```

See [`examples/curl-example-socket.sh`](examples/curl-example-socket.sh) for a short version of the walkthrough
above addressed this way, and [`tests/integration_test.sh`](tests/integration_test.sh) for the same API wired up
as an automated test against a real Mosquitto broker.

## MQTT examples

Quick reference for pushing to and reading from the broker directly with `mosquitto_pub`/`mosquitto_sub` -- the
native-MQTT counterpart to the curl examples above, against this stack's ports. See
[Mosquitto test commands](#mosquitto-test-commands) for the `-C 1`/`-W` semantics explained, the `docker exec`
alternative if these aren't installed on your host, and the `-h 127.0.0.1` form for a bare-metal broker.

**Push** (publish):

```bash
mosquitto_pub -h localhost -p 1883 -t 'sensors/kitchen/temperature' -m '21.4'
mosquitto_pub -h localhost -p 1883 -t 'sensors/kitchen/temperature' -m '21.4' -r   # retained

# over MQTT/TLS on 8883, once ./gen-cert.sh has populated tls/
mosquitto_pub -h localhost -p 8883 --cafile tls/ca.crt -t 'sensors/kitchen/temperature' -m '21.4'

# via docker exec -- nothing to install on the host
docker compose exec mosquitto mosquitto_pub -t 'sensors/kitchen/temperature' -m '21.4'
```

**Consume** (subscribe):

```bash
mosquitto_sub -h localhost -p 1883 -t '#' -v                                 # watch everything, live
mosquitto_sub -h localhost -p 1883 -t 'sensors/kitchen/temperature' -C 1 -v  # current value of one topic
mosquitto_sub -h localhost -p 1883 -t '#' -v -W 2                            # current value of every topic, then exit

# over MQTT/TLS on 8883
mosquitto_sub -h localhost -p 8883 --cafile tls/ca.crt -t '#' -v

# via docker exec -- nothing to install on the host
docker compose exec mosquitto mosquitto_sub -t '#' -v
```

Runnable versions: [`examples/mosquitto-example.sh`](examples/mosquitto-example.sh) (host-installed client) and
[`examples/mosquitto-example-docker.sh`](examples/mosquitto-example-docker.sh) (same operations, via `docker
exec`).

## NGINX example

See [`examples/nginx-nshmqtt.conf`](examples/nginx-nshmqtt.conf) for a complete, standalone config: HTTPS
termination, basic-auth-protected `/event/`, `/metric/`, and `/metrics-state` endpoints, a rate limit on the write
paths, an IP-restricted (lighter-weight) `/metrics`, and an unauthenticated `/health` -- all `proxy_pass`ed straight
through to nshmqtt's UNIX socket. Adapt the auth mechanism to your environment; basic auth is only used there
because it needs no extra NGINX modules to demonstrate end to end. This is independent of and stacks with nshmqtt's
own `http_auth_tokens` (see [Authentication](#authentication)) -- NGINX auth failing never even reaches nshmqtt;
nshmqtt's own check still applies to whatever gets through.

`docker-compose.yml` in this project runs a fuller example: NGINX in front of both nshmqtt's HTTP API and
mosquitto's native MQTT port (via a `stream{}` TCP passthrough) -- see [`nginx/nginx.conf`](nginx/nginx.conf).
Prometheus and Grafana sit behind a second, separate NGINX container instead, see
[`nginx/nginx-monitoring.conf`](nginx/nginx-monitoring.conf) and the "Docker" section below for why.

## TLS

Server-side TLS only for now (no client certs/mTLS yet -- a later step, not this one). NGINX terminates it for
both protocols; nshmqtt and mosquitto stay plain internally, same pattern as the remote-broker `stream{}` example
in [MQTT connectivity](#mqtt-connectivity), just applied to inbound clients here instead of an outbound connection.

```bash
./gen-cert.sh            # writes tls/ca.{key,crt} (created once, reused) and tls/tls.{key,crt} (regenerated fresh)
docker compose up -d     # nginx.conf's ssl_certificate/ssl_certificate_key point at tls/tls.crt and tls/tls.key
```

`nginx/nginx.conf` adds a TLS listener alongside every plain one, on the same `server{}` block (so nothing is
duplicated) -- `8443` next to `8080` for HTTPS, `8883` next to `1883` for MQTT/TLS (mapped to host `8444` and
`8883` respectively in `docker-compose.yml`, since `8443` is already claimed by another project's stack on a
typical dev machine, same reasoning as `8081` for `8080`). The plain listeners are not going away -- they're what
this project's own examples and tests already assume, so TLS is additive, not a cutover. Grafana and Prometheus's
own listeners (`3443`, `9444`) are TLS-only, no plain fallback -- see the "Docker" section below.

`gen-cert.sh` is a self-contained local test CA: an ECDSA P-256 CA (`tls/ca.key`/`tls/ca.crt`, generated once and
reused -- regenerating it would invalidate every client that already trusts the old one) signs one leaf cert
(`tls/tls.key`/`tls/tls.crt`, regenerated fresh every run) covering both TLS listeners, since one NGINX process
terminates both. **Not for production** -- a real deployment wants a real CA-issued cert (e.g. via ACME) instead;
`tls/ca.key` living unencrypted on disk, bind-mounted into a container, is a local-testing shortcut, not something
to replicate for anything that matters. `tls/` (including the private keys) is gitignored -- never commit it, test
CA or not.

Any client connecting over TLS needs `tls/ca.crt` (never `tls/ca.key`) in its own trust store to validate the leaf:

```bash
curl --cacert tls/ca.crt 'https://localhost:8444/health'
mosquitto_pub --cafile tls/ca.crt -h localhost -p 8883 -t 'test' -m 'hello'
```

## Mosquitto test commands

A quick reference for watching everything nshmqtt itself publishes -- see [MQTT subscriptions](#mqtt-subscriptions)
for the fuller picture of the *other* direction (publishing to the broker directly so nshmqtt picks it up),
including retained-message semantics and more publish examples matching this stack's ports.

**Three ways to run `mosquitto_pub`/`mosquitto_sub` against this stack**, depending on what's available on the
machine you're running them from:

1. **Installed on the host**, talking to the broker over the network -- the commands throughout this section as
   written, e.g. `mosquitto_sub -h localhost -p 1883 -t '#' -v`. Nothing to configure beyond having the package
   installed:
   ```bash
   apt install mosquitto-clients   # Debian/Ubuntu
   apk add mosquitto-clients       # Alpine
   ```
2. **Via `docker exec`, inside the `mosquitto` container itself** -- nothing to install on the host at all, since
   the `eclipse-mosquitto` image already bundles `mosquitto_pub`/`mosquitto_sub`. Drop `-h`/`-p` (from inside that
   container the broker is just `localhost` on its default port) and prefix any command in this section with either
   of these -- both reach the same running container, `docker compose exec` by service name (run from the project
   root, where `docker-compose.yml` is) or `docker exec` by container name from anywhere:
   ```bash
   docker compose exec mosquitto mosquitto_sub -t '#' -v
   docker exec -it mosquitto mosquitto_sub -t '#' -v
   ```
3. **Directly against a bare-metal/systemd mosquitto** instead of the compose stack -- same commands, `-h 127.0.0.1`
   in place of `-h localhost -p 1883` (see [Installation](#installation)).

A runnable version of everything below: [`examples/mosquitto-example.sh`](examples/mosquitto-example.sh) (option 1,
a host-installed client) and [`examples/mosquitto-example-docker.sh`](examples/mosquitto-example-docker.sh) (option
2, the same operations via `docker exec` -- nothing to install first).

```bash
mosquitto_sub -h localhost -p 1883 -t '#' -v
```

**Getting the current value of one topic from the command line, without watching for future updates.** Every state
write nshmqtt makes (an HTTP `PUT /metric`) is published with the MQTT retain flag set (see
[Current state / metrics](#current-state--metrics)), so a fresh subscriber gets the broker's last known value
immediately on subscribe, with no publish needing to happen first. `mosquitto_sub -C 1` exits after exactly one
message, which turns that into a one-shot "get" -- the native-MQTT equivalent of a single series from
`GET /metrics-state`, with no HTTP round trip:

```bash
mosquitto_sub -h localhost -p 1883 -t 'server1/load' -C 1 -v
```

`-C 1` only works cleanly against a topic you know is retained (any state nshmqtt has written, or any value a
device published with its own `-r`/retain flag -- see [MQTT subscriptions](#mqtt-subscriptions)'s note on that). If
the topic never had a retained message, this blocks until one actually arrives rather than returning immediately.
For a whole tree of topics at once (`#` or any other wildcard) rather than one specific name, `-C` doesn't work the
same way -- there's no single "one message" when several topics could each have their own retained value. Use
`-W <seconds>` instead: it exits once no *new* message has arrived within that window, which in practice means "all
the currently-retained values have come in, and it's been quiet since":

```bash
mosquitto_sub -h localhost -p 1883 -t '#' -v -W 2
```

## Concurrency model

Three independent pieces:

**HTTP worker pool** (`threads=`, default: CPU core count clamped to 4-20) -- one thread handles one connection's
request/response.

**MQTT connection pool** (`mqtt_pool_size=`, default `1`) -- N independent MQTT connections, each with its own Paho
handle and its own dedicated worker thread, all pulling publish jobs from one shared bounded queue
(`mqtt_queue_size`). This is a pool of whole *connections*, not a pool of threads sharing one connection: Eclipse
Paho's own header documentation states plainly, *"The MQTTClient API is not thread safe, whereas the MQTTAsync API
is."* Calling `MQTTClient_publish` concurrently from multiple threads on the *same* handle would be a real data
race -- so instead, each connection's handle is only ever touched by its own worker thread, and parallelism comes
from having several connections rather than several threads sharing one. HTTP worker threads hand off publish jobs
through the shared queue rather than calling Paho directly -- which gives them exactly the decoupling they need: an
HTTP request blocks only until some connection finishes the job or `mqtt_publish_timeout_seconds` elapses, never on
however long the actual network I/O to a slow or wedged broker would otherwise take. Each connection beyond the
first gets `mqtt_client_id` suffixed `-N` (the broker sees N distinct sessions); only the first connection ever
subscribes, regardless of pool size, so `subscribe_enabled` messages are never delivered more than once.

**Subscribe worker thread** -- one, draining messages from the pool's one subscribing connection. Kept separate
from the publish workers because Paho's own message-arrived callback must stay fast and non-blocking (copy the
data, enqueue it, return -- never process it inline), and sharing a thread with a potentially slow publish() call
risks delaying MQTT-level keepalive handling on that connection.

```text
                                    +-->  connection 0 worker  -->  Paho MQTTClient (client_id)
HTTP worker  --publish job-->  shared queue  -->  connection 1 worker  -->  Paho MQTTClient (client_id-1)
                                    +-->  connection N worker  -->  Paho MQTTClient (client_id-N)

Paho callback on connection 0 only (Paho's own thread) --copy+enqueue-->  subscribe worker thread  -->  StateStore
```

Publish outcomes map to HTTP status directly: `Ok` -> `200`, `QueueFull` (the broker clearly isn't keeping up) ->
`503`, `Timeout` (queued, but no result within `mqtt_publish_timeout_seconds`) -> `504`, `Failed` (Paho reported a
definite failure) -> `502`. A publish that fails or times out also marks the connection as needing reconnection,
even before Paho's own `on_connection_lost` callback fires -- a wedged connection that keeps failing publishes is
treated the same as a dropped one.

## Installation

```bash
sudo make install
```

installs the binary and a starter config (does **not** create the `nshmqtt` user/group or install the systemd
unit). See [`etc/nshmqtt.service`](etc/nshmqtt.service) for a ready-to-adapt systemd unit with a sandboxing/
hardening profile, and create the `nshmqtt` system user/group and `/etc/nshmqtt/nshmqtt.conf` (from
`etc/nshmqtt.conf.example`) manually. A single install script that automates all of this in one step is a
reasonable follow-up, not included yet.

## Docker

```bash
docker build -t nshmqtt .
# or:
./build.sh   # builds the image and extracts the binary to ./nshmqtt
```

This is a normal dynamic Alpine build, not a fully static one: Alpine's `paho-mqtt-c-dev` package ships only shared
libraries (no `libpaho-mqtt3c.a`), so there's no static Paho (or libcurl) to link against. The runtime image is
still small (Alpine + `paho-mqtt-c` + `libcurl` + `libstdc++`, a few MB), just not a from-scratch single binary.
See the Dockerfile's own
comments, and [`docker/fortify_shim.cpp`](docker/fortify_shim.cpp) for a couple of Alpine/musl compatibility
symbols this build needs (`std::shared_ptr` and `condition_variable::wait_for` against a steady-clock deadline,
both used by the MQTT worker queue).

`docker-compose.yml` brings up a local stack: mosquitto, nshmqtt, and NGINX in front of both. Prometheus and
Grafana (with Prometheus auto-provisioned as a datasource), plus a second, separate NGINX container that fronts
just those two, are optional, behind a `monitoring` profile -- the core stack runs fully without any of them,
since the core NGINX's `9100` already serves `/metrics`/`/metrics-state` to any Prometheus, bundled or external:

```bash
docker compose up -d                              # core stack only
docker compose --profile monitoring up -d         # core stack + Prometheus + Grafana
```

Every published port has its own bind-address variable, overridable in a local `.env` file without touching the
tracked compose file (e.g. `HTTP_BIND=0.0.0.0` to reach the write API from elsewhere on the network) -- and every
one of them has its own port-number variable too, e.g. `GRAFANA_PORT=4000` if `3000` collides with something else
on your host. Defaults differ by what the endpoint actually is, not just its protocol:

| Endpoint                          | Bind default | Port default | Why                                           |
| --------------------------------- | ------------ | ------------ | --------------------------------------------- |
| `HTTP_BIND` / write API           | `127.0.0.1`  | `8081`       | plain, local-only                             |
| `MQTT_BIND` / native MQTT         | `127.0.0.1`  | `1883`       | plain, local-only                             |
| `METRICS_BIND` / metrics          | `0.0.0.0`    | `9100`       | the exporter endpoint Prometheus reaches      |
| `GRAFANA_BIND` / Grafana          | `0.0.0.0`    | `3000`       | TLS-only; the dashboard you want open         |
| `PROMETHEUS_BIND` / Prometheus UI | `127.0.0.1`  | `9090`       | TLS-only, but more of an admin/debugging tool |

TLS listeners (`8444`, `8883`, and Grafana/Prometheus's own) always stay on all interfaces regardless of the table
above -- terminating TLS is what makes exposing them safe in the first place, so there's no loopback-only default
to override for those.

The core NGINX (`nginx/nginx.conf`) publishes four genuinely separate things, not just different paths behind one
port:

- The write API (`/event/*`, `/metric/*`) and `/health` on `8080`/`8443` -- reaching nshmqtt over its UNIX socket
  internally (a named volume shared between the two containers, mounted at `/run/nshmqtt` in both), matching
  nshmqtt's own documented primary interface.
- Metrics on NGINX's own **dedicated** `9100` listener -- published to the host under the same port number
  (`localhost:9100/metrics`, `localhost:9100/metrics-state`), the Prometheus exporter port convention
  (`node_exporter`'s own default). Only these two paths exist there; everything else 404s, the same way `8080`
  does for paths it doesn't recognize. Internally, NGINX reaches nshmqtt's own TCP listener at `nshmqtt:9100` to
  serve this -- but that internal `9100` is not itself published to the host; NGINX's own `9100` is the only
  externally-reachable one.
- Native MQTT on `1883`/`8883`, over the `stream{}` passthrough to mosquitto.

A second, separate NGINX container (`nginx-monitoring`, `nginx/nginx-monitoring.conf`) publishes the other two,
both TLS-only:

- Grafana, on its own dedicated listener (`3000` externally by default) -- proxied to `grafana:3000` internally.
  Grafana itself is never published directly (see its own service definition); this container is the only way to
  reach it.
- Prometheus's own UI, on its own dedicated listener (`9090` externally by default) -- same pattern, proxied to
  `prometheus:9090` internally, never published directly by Prometheus itself.

nshmqtt itself doesn't enforce any of the core split -- both of its own listeners (socket and TCP) answer the
identical full route table, `tcp_port` doesn't restrict which paths are available on it -- it's entirely
`nginx.conf`'s own `server`/`location` blocks doing the separation. The in-stack Prometheus is routed through the
core NGINX's `9100` too (see `prometheus/prometheus.yml`), the same connection an external Prometheus would use,
so metrics scraping goes through the same exposure policy as everything else the core NGINX fronts.

`nginx-monitoring` is a genuinely separate container, not just more `server{}` blocks added to the core NGINX's
own config, because it shares the same `monitoring` profile as Grafana and Prometheus: compose starts and stops
all three together, and this container simply doesn't exist at all when the profile isn't active. Both
`nginx.conf` and `nginx-monitoring.conf` use `upstream{}` blocks with the `resolve` parameter (mainline NGINX
since 1.27.3) plus a `zone` and the `resolver` directive, so any backend container restarting with a new IP on
Docker's own network is tolerated without an NGINX restart.

*Why a separate container*: an earlier version of this stack proxied Grafana/Prometheus from inside the core,
always-running NGINX instead, relying on `resolve` alone to tolerate their hostnames not existing yet when the
`monitoring` profile was off. It worked, but `resolve` re-resolves on a background timer regardless of traffic, so
NGINX logged a harmless but permanent "could not be resolved" error for `grafana`/`prometheus` the entire time the
profile was inactive. Splitting the container out removes the condition entirely: by the time `nginx-monitoring`
exists at all, Grafana and Prometheus are already coming up as part of the same profile.

```bash
./reset-state.sh
```

clears everything persisted for the compose stack's current-state: nshmqtt's own `state.json` **and** mosquitto's
retained messages together, not just one or the other -- clearing only nshmqtt's own store isn't enough on its own
to actually get a clean slate, since every metric write publishes with the MQTT retain flag set (see
[Current state / metrics](#current-state--metrics)), and `subscribe_enabled=true` in this stack means nshmqtt just
resubscribes to everything mosquitto still has retained the moment it restarts, re-populating exactly what was
just cleared. `./reset-state.sh` stops both, clears both, and brings back up whichever of the two were actually
running -- safe to run whether the stack is up or down, and doesn't touch mosquitto's non-retained state,
Prometheus's history, or Grafana's dashboards.

## Security considerations

The UNIX socket should not be world-accessible -- `socket_mode=0660` plus group membership (see the systemd unit's
`SupplementaryGroups=`) is the normal way NGINX gets access without making the socket generally reachable. The
optional TCP listener defaults to disabled and, if enabled, defaults to loopback unless `tcp_address` is set
explicitly. nshmqtt does not implement HTTPS, ACLs, or rate limiting itself -- NGINX is the normal place for all of
that (see [NGINX example](#nginx-example)); duplicating it inside nshmqtt would work against the project's own
"small, auditable, dependency-light" goal. Request *authentication* is the one exception: nshmqtt has its own
minimal token check (`http_auth_tokens`, see [Authentication](#authentication)) specifically because the optional
TCP listener can be reached with no NGINX in the path at all -- NGINX-only auth would leave that route unprotected.

## Testing

```bash
make test                       # unit tests: HTTP parsing, config, JSON
                                 # helpers, state persistence, Prometheus
                                 # name normalization/collision detection
tests/integration_test.sh       # protocol-level tests against a running
                                 # daemon, including a real MQTT round trip
                                 # if mosquitto/mosquitto_sub are on PATH,
                                 # and webhook forwarding (against a mock
                                 # HTTP receiver) if python3 is too
tests/compose_smoke_test.sh     # container-wiring tests against
                                 # `docker compose up -d` -- the shared
                                 # UNIX socket volume, NGINX proxying
                                 # (plain and TLS, HTTP and MQTT), and
                                 # Prometheus scrape health; run after
                                 # bringing the stack up, doesn't manage
                                 # it itself
```

## Non-goals

MQTT broker functionality, HTTP TLS, a web UI/dashboard, a historical metrics database, a workflow/rule engine,
general-purpose JSON transformation, a separate Prometheus HTTP server or dedicated metrics TCP listener, SQLite,
unlimited offline MQTT queues, or Node-RED/EMQX-style processing. `nshmqtt` is intentionally glue.
