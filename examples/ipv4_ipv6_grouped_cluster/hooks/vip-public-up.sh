#!/bin/sh
set -eu

GW="198.51.100.1"
DEV="$LCS_INTERFACE"
TABLE="public"

ip route replace default via "$GW" dev "$DEV" table "$TABLE"

ip rule del from "$LCS_ADDRESS" table "$TABLE" 2>/dev/null || true
ip rule add from "$LCS_ADDRESS" table "$TABLE" priority 100
