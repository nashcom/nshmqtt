// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#pragma once

// Substitutes NSHMQTT_* placeholder tokens found in a POST /event body
// with freshly generated values before publishing -- gated behind
// event_placeholders_enabled (see config.h, off by default), applied to
// /event only, never PUT /metric or the webhook envelope. Bare tokens, no
// {{ }}/${ } wrapper: the names are distinctive enough (NSHMQTT_-prefixed,
// long, specific) that an accidental collision with real payload content
// is negligible, and it keeps both the implementation and the caller's
// mental model simpler than a delimited-placeholder syntax would need to
// be.
//
// Recognized tokens:
//   NSHMQTT_RANDOM_HEX8/HEX16/HEX32/HEX64  that many random hex
//                                          characters (4/8/16/32 random
//                                          bytes) -- the number is a
//                                          character count, not a byte
//                                          count
//   NSHMQTT_RANDOM_UUID                    a random UUIDv4
//   NSHMQTT_TIMESTAMP_S/MS/US/NS           Unix epoch time, as a plain
//                                          integer, at that unit
//   NSHMQTT_DATETIME_UTC                   ISO 8601, e.g.
//                                          2026-09-10T14:23:01Z
//   NSHMQTT_DATE_UTC                       e.g. 2026-09-10
//   NSHMQTT_TIME_UTC                       e.g. 14:23:01
//
// Every occurrence gets its own independently generated value -- two
// NSHMQTT_RANDOM_HEX8 tokens in the same payload produce two different
// values, never one value reused twice. Text that doesn't match any
// recognized token is left untouched.

#include <string>

namespace nshmqtt
{

std::string substitute_event_placeholders(const std::string &payload);

} // namespace nshmqtt
