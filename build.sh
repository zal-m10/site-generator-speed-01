#!/bin/sh
# Build addon.node (x86_64 Linux, N-API v8 — loads on Node 22 and later).
# Requires g++ and the Node headers (/usr/include/node). No downloads.
# -O3 + LTO (-flto) lets the optimiser inline across translation units
# (the SSE2 scan helpers and the small Value/json helpers in particular);
# -fvisibility=hidden keeps only the N-API module init visible, and
# -fno-plt avoids PLT indirection on the few remaining cross-DSO calls.
# The binary is stripped after linking. A clean rebuild is deterministic
# (same inputs give the same addon.node bytes).
set -e
cd "$(dirname "$0")"
g++ -O3 -DNDEBUG -std=c++17 -fPIC -shared -fvisibility=hidden -flto -fno-plt \
  -DNAPI_VERSION=8 -I/usr/include/node \
  addon.cc core.cpp json.cpp util.cpp -o addon.node
strip --strip-unneeded addon.node
ls -l addon.node
