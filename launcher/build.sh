#!/bin/sh
HERE="$(cd "$(dirname "$0")" && pwd)"
LAUNCHER_DIR="$HERE" PAYLOAD=mc3_nx exec "$HERE/../runtime/launcher/build.sh" "$@"
