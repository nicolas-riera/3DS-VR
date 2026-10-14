#!/usr/bin/env bash
# Builds dist/3DS-VR.{3dsx,cia,3ds}.
# Uses $DEVKITPRO (default /opt/devkitpro). makerom and bannertool must be in
# PATH, or in $CTRTOOLS (default /opt/ctrtools).
set -e
cd "$(dirname "$0")"
export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"
CTRTOOLS="${CTRTOOLS:-/opt/ctrtools}"
if [ -d "$CTRTOOLS" ]; then export PATH="$CTRTOOLS:$PATH"; fi
make -j"$(nproc)" "$@"
