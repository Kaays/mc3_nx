#!/bin/sh
# Build mc3_nx.nsp in the AArch32 toolchain container (see runtime/tools/docker_build.sh).
#   ./build.sh            mc3_nx.nsp + mc3_nx.build
#   ./build.sh clean
# Then: launcher/build.sh  -> launcher/mc3_nx.nro (carries the NSP)
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
exec "$HERE/runtime/tools/docker_build.sh" "$@"
