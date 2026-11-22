#!/bin/sh
# Compila y corre en el PC la prueba que compara el núcleo con el SDK oficial de JavaScript.
# Solo necesita un compilador de C. Desde la raíz del repositorio:  sh tests/host/correr.sh
set -e
CC="${CC:-gcc}"
"$CC" -std=c99 -Wall -Wextra \
  -Isrc -Ithird_party/monocypher -Itests/vectores \
  tests/host/prueba_vectores.c src/nucleo.c \
  third_party/monocypher/monocypher.c third_party/monocypher/monocypher-ed25519.c \
  -o /tmp/prueba_vectores
/tmp/prueba_vectores
