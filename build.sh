#!/bin/sh
# Build addon.node (x86_64 Linux, N-API v8 — loads on Node 22 and later).
# Requires g++ and the Node headers (/usr/include/node). No downloads.
set -e
cd "$(dirname "$0")"
g++ -O3 -std=c++17 -fPIC -shared -s -DNAPI_VERSION=8 -I/usr/include/node \
  addon.cc core.cpp json.cpp util.cpp -o addon.node
ls -l addon.node
