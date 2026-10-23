#!/bin/sh
# Serve the touch panel. Open http://<this machine>:8090/ad4-touch.html
#
# The WebSocket is not subject to CORS, so the panel works from any host as long
# as the moonraker address in the header (gear chip, saved in localStorage) is
# reachable from the browser. Only the optional configfile read is blocked
# cross-origin, which just leaves the rotation_distance field blank.
cd "$(dirname "$0")" || exit 1
exec python3 -m http.server "${1:-8090}" --bind 0.0.0.0
