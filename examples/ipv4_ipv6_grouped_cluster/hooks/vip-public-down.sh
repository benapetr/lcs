#!/bin/sh

GW="198.51.100.1"
DEV="$LCS_INTERFACE"
TABLE="public"

ip rule del from "$LCS_ADDRESS" table "$TABLE" 2>/dev/null || true
ip route del default via "$GW" dev "$DEV" table "$TABLE" 2>/dev/null || true
