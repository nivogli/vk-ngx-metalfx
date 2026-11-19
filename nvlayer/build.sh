#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
VULKAN_INC="${VULKAN_INC:-/opt/homebrew/include}"
clang -O2 -shared -fPIC -I"$VULKAN_INC" -o libVkLayer_metalfx_nvidia_id.dylib nvlayer.c
echo "Built $(pwd)/libVkLayer_metalfx_nvidia_id.dylib"
