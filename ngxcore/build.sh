#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
CC="${MINGW_CC:-x86_64-w64-mingw32-clang}"
"$CC" -O1 -shared -o nvngx.dll ngxcore.c
echo "Built $(pwd)/nvngx.dll"
echo "Note: Windows NGX loaders enforce WinVerifyTrust; sign nvngx.dll with a trusted Authenticode certificate before use."
