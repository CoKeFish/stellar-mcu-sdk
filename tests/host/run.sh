#!/bin/sh
# Builds and runs on a PC the test that compares the core with the official JavaScript SDK.
# It only needs a C compiler. From the repository root:  sh tests/host/run.sh
set -e
CC="${CC:-gcc}"
"$CC" -std=c99 -Wall -Wextra \
  -Isrc -Ithird_party/monocypher -Itests/vectors \
  tests/host/test_vectors.c src/stellar_mcu.c \
  third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c \
  -o /tmp/test_vectors
/tmp/test_vectors
