#!/bin/bash
# Copyright (c) 2026 Daniel Nashed / NashCom
# SPDX-License-Identifier: Apache-2.0

# Thin wrapper around `docker compose` for this stack's day-to-day admin
# commands -- not a replacement for compose, just one flag (--monitoring)
# instead of remembering `--profile monitoring` and which subcommands need
# it. `down`/`status`/`logs` always pass `--profile monitoring` regardless
# of the flag: compose only shows/manages containers from profiles named
# on the command line, so without it a running prometheus/grafana pair
# would be invisible to `down`/`status`/`logs` even though they're up.
# `up`/`restart` are the only ones where the profile is actually optional,
# since that's what decides whether prometheus/grafana start at all.
#
# Every argument is validated, not just pattern-matched against what's
# expected -- a mistyped `--monitorng` (or any other unrecognized
# argument) refuses instead of silently falling through to "no
# monitoring", which would otherwise look identical to success while
# quietly not doing what was asked.

set -eu

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cd "$SCRIPT_DIR"

usage() {
    cat >&2 <<'EOF'
Usage: ./stack.sh <command> [options]

Commands:
  up [--monitoring]        Start the stack (docker compose up -d)
  down                     Stop and remove the stack's containers
  restart [--monitoring]   Restart the stack
  status                   Show container status (docker compose ps)
  logs [-f] [service...]   Show logs (docker compose logs)

--monitoring includes prometheus, grafana, and nginx-monitoring (the
"monitoring" compose profile); without it, only mosquitto/nshmqtt/nginx
are affected.
EOF
    exit 1
}

error() {
    echo "stack.sh: $1" >&2
    usage
}

# Parses the optional --monitoring flag shared by `up` and `restart`:
# exactly zero or one argument, and if present it must be exactly
# "--monitoring" -- anything else (a typo, an extra argument, a service
# name meant for `logs`) is a usage error, not a silent no-op.
parse_monitoring_flag() {
    case $# in
        0)
            echo 0
            ;;
        1)
            [ "$1" = "--monitoring" ] || error "unknown option '$1'"
            echo 1
            ;;
        *)
            error "too many arguments: $*"
            ;;
    esac
}

[ $# -ge 1 ] || usage
cmd="$1"
shift

case "$cmd" in
    up)
        monitoring="$(parse_monitoring_flag "$@")"
        if [ "$monitoring" = "1" ]; then
            docker compose --profile monitoring up -d
        else
            docker compose up -d
        fi
        ;;

    down)
        [ $# -eq 0 ] || error "'down' takes no arguments: $*"
        docker compose --profile monitoring down
        ;;

    restart)
        monitoring="$(parse_monitoring_flag "$@")"
        if [ "$monitoring" = "1" ]; then
            docker compose --profile monitoring restart
        else
            docker compose restart
        fi
        ;;

    status)
        [ $# -eq 0 ] || error "'status' takes no arguments: $*"
        docker compose --profile monitoring ps
        ;;

    logs)
        docker compose --profile monitoring logs "$@"
        ;;

    *)
        error "unknown command '$cmd'"
        ;;
esac
