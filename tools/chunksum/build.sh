#!/bin/bash
# Build chunksum statically for the phone (TWRP, arm64) and for this machine.
set -euo pipefail
cd "$(dirname "$0")"
aarch64-linux-gnu-gcc-11 -O2 -static -Wall -Werror -o chunksum-arm64 chunksum.c
cc -O2 -Wall -Werror -o chunksum-host chunksum.c
