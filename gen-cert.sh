#!/bin/bash
# Copyright (c) 2026 Daniel Nashed / NashCom
# SPDX-License-Identifier: Apache-2.0


# Generates a local test CA and an ECDSA (P-256) leaf cert signed by it,
# for local testing only - populates ./tls/ so the compose stack's nginx
# can start with TLS enabled. Not for production; a real deployment wants
# a real CA-issued cert there instead (e.g. via ACME).
#
# Everything, including the CA private key, lives in tls/ - bind-mounted
# read-only into the nginx container (see docker-compose.yml). Fine for
# local testing; a production CA key should not be handled this way.
# tls/ca.crt (not ca.key) is what any external client -- mosquitto_pub/sub
# with --cafile, curl with --cacert, another MQTT/HTTP client -- needs in
# its own trust store to validate this stack's leaf cert.
#
# The CA (key + cert) is only ever generated once and reused on later
# runs - regenerating it would invalidate every client that already
# trusts the old ca.crt. The leaf (key + cert) is always regenerated
# fresh each run, since only this stack itself needs to trust it, and one
# leaf cert covers both TLS listeners nginx.conf adds (HTTPS and MQTT/TLS)
# -- same hostname, same nginx process terminating both.

set -e

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
TLS_DIR="${SCRIPT_DIR}/tls"
ENV_FILE="${SCRIPT_DIR}/.env"

if [ -r "$ENV_FILE" ]; then
  # shellcheck disable=SC1090
  . "$ENV_FILE"
fi

# Must match docker-compose.yml's nginx service name/hostname for TLS
# hostname verification to actually pass when a client connects by that
# name. Assumes a DNS name, not an IP - if you set TLS_HOST to a raw IP,
# add it as an IP: SAN by hand below.
#
# Defaults to localhost for local/compose use, but already supports a
# real FQDN for a real deployment: TLS_HOST=mqtt.example.com ./gen-cert.sh
# puts that name in both the CN and the SAN list (alongside nginx/
# localhost/127.0.0.1, so local testing keeps working too). Not yet wired
# into docker-compose.yml or NGINX's server_name -- when there's an
# actual hostname to deploy against, that's the next step; for now this
# is just the CA/leaf generation already being ready for it.
TLS_HOST="${TLS_HOST:-localhost}"

CA_DAYS="${CA_DAYS:-3650}"
LEAF_DAYS="${LEAF_DAYS:-365}"

mkdir -p "$TLS_DIR"

# CA - only created if missing, so re-running this script doesn't
# invalidate clients that already trust the existing ca.crt.
if [ -e "${TLS_DIR}/ca.key" ] && [ -e "${TLS_DIR}/ca.crt" ]; then
  echo "Existing CA found at ${TLS_DIR}/ca.key + ca.crt - reusing it."
  ca_created="no"
else
  openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "${TLS_DIR}/ca.key"
  openssl req -x509 -new -key "${TLS_DIR}/ca.key" -days "$CA_DAYS" -out "${TLS_DIR}/ca.crt" \
    -subj "/CN=nshmqtt local CA" \
    -addext "basicConstraints=critical,CA:TRUE" \
    -addext "keyUsage=critical,keyCertSign,cRLSign"
  ca_created="yes"
fi

# Leaf, signed by the CA - always regenerated fresh (key and cert both).
# One leaf covers both TLS listeners (see nginx.conf): the SAN list
# includes both the nginx service name in the compose network and
# localhost/127.0.0.1 for host-side testing.
openssl genpkey -algorithm EC -pkeyopt ec_paramgen_curve:P-256 -out "${TLS_DIR}/tls.key"

CSR=$(mktemp)
openssl req -new -key "${TLS_DIR}/tls.key" -out "$CSR" \
  -subj "/CN=${TLS_HOST}" \
  -addext "subjectAltName=DNS:${TLS_HOST},DNS:nginx,DNS:localhost,IP:127.0.0.1"

openssl x509 -req -in "$CSR" \
  -CA "${TLS_DIR}/ca.crt" -CAkey "${TLS_DIR}/ca.key" -CAcreateserial \
  -out "${TLS_DIR}/tls.crt" -days "$LEAF_DAYS" -copy_extensions copyall

rm -f "$CSR"

chmod 600 "${TLS_DIR}/ca.key" "${TLS_DIR}/tls.key"

if [ "$ca_created" = "yes" ]; then
  echo "Wrote ${TLS_DIR}/ca.key + ca.crt (CA, keep ca.key private)."
else
  echo "Reused existing ${TLS_DIR}/ca.key + ca.crt (CA)."
fi
echo "Wrote ${TLS_DIR}/tls.key + tls.crt (leaf, signed by the CA above)."
echo "Trust ${TLS_DIR}/ca.crt in any client that will connect over TLS (curl --cacert,"
echo "mosquitto_pub/sub --cafile, a browser's/OS's trust store for a manual check, ...)."
