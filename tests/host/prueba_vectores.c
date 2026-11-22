// Prueba en el PC: el núcleo arma direcciones, transacciones, autorizaciones y filtros de eventos,
// y cada resultado se compara byte a byte con lo que produce el SDK oficial de JavaScript
// (tests/vectores/vectores.h, generado por tests/vectores/vectores.mjs).
//
// No necesita placa ni red. La placa aporta SHA-256 y la firma; aquí las aporta este archivo
// (SHA-256 propio) y Monocypher (Ed25519).
//
// Cómo correrla: ver tests/host/correr.sh
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "nucleo.h"
#include "vectores.h"

// ---------------------------------------------------------------- SHA-256 para el PC
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static uint32_t girar(uint32_t v, unsigned n) { return (v >> n) | (v << (32 - n)); }

static void sha256_bloque(uint32_t h[8], const uint8_t b[64]) {
  uint32_t w[64], a[8];
  for (int i = 0; i < 16; i++)
    w[i] = (uint32_t)b[4 * i] << 24 | (uint32_t)b[4 * i + 1] << 16 | (uint32_t)b[4 * i + 2] << 8 | b[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = girar(w[i - 15], 7) ^ girar(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = girar(w[i - 2], 17) ^ girar(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  memcpy(a, h, sizeof a);
  for (int i = 0; i < 64; i++) {
    uint32_t s1 = girar(a[4], 6) ^ girar(a[4], 11) ^ girar(a[4], 25);
    uint32_t ch = (a[4] & a[5]) ^ (~a[4] & a[6]);
    uint32_t t1 = a[7] + s1 + ch + K256[i] + w[i];
    uint32_t s0 = girar(a[0], 2) ^ girar(a[0], 13) ^ girar(a[0], 22);
    uint32_t mayoria = (a[0] & a[1]) ^ (a[0] & a[2]) ^ (a[1] & a[2]);
    uint32_t t2 = s0 + mayoria;
    a[7] = a[6]; a[6] = a[5]; a[5] = a[4]; a[4] = a[3] + t1;
    a[3] = a[2]; a[2] = a[1]; a[1] = a[0]; a[0] = t1 + t2;
  }
  for (int i = 0; i < 8; i++) h[i] += a[i];
}

static void pc_sha256(const uint8_t *datos, size_t n, uint8_t salida[32]) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  uint8_t bloque[64];
  size_t i = 0;
  for (; i + 64 <= n; i += 64) sha256_bloque(h, datos + i);
  size_t resto = n - i;
  memcpy(bloque, datos + i, resto);
  bloque[resto++] = 0x80;
  if (resto > 56) {
    memset(bloque + resto, 0, 64 - resto);
    sha256_bloque(h, bloque);
    resto = 0;
  }
  memset(bloque + resto, 0, 56 - resto);
  uint64_t bits = (uint64_t)n * 8;
  for (int j = 0; j < 8; j++) bloque[56 + j] = (uint8_t)(bits >> (56 - 8 * j));
  sha256_bloque(h, bloque);
  for (int j = 0; j < 8; j++) {
    salida[4 * j] = (uint8_t)(h[j] >> 24);
    salida[4 * j + 1] = (uint8_t)(h[j] >> 16);
    salida[4 * j + 2] = (uint8_t)(h[j] >> 8);
    salida[4 * j + 3] = (uint8_t)h[j];
  }
}

// ---------------------------------------------------------------- firma con Monocypher
static void pc_firmar(const uint8_t semilla[32], const uint8_t publica[32], const uint8_t *m, size_t n,
                      uint8_t firma[64]) {
  uint8_t llave[64];  // Monocypher guarda la llave secreta como semilla + llave pública
  memcpy(llave, semilla, 32);
  memcpy(llave + 32, publica, 32);
  crypto_ed25519_sign(firma, llave, m, n);
  crypto_wipe(llave, sizeof llave);
}

static const stl_cripto CRIPTO = {pc_sha256, pc_firmar};

// ---------------------------------------------------------------- comparación
static int g_pruebas = 0, g_fallos = 0;
static uint8_t g_tx[1200];
static char g_b64[1700];

static void comparar(const char *nombre, const char *obtenido, const char *esperado) {
  int ok = !strcmp(obtenido, esperado);
  g_pruebas++;
  if (!ok) {
    g_fallos++;
    printf("  obtenido: %s\n  esperado: %s\n", obtenido, esperado);
  }
  printf("%-30s %s\n", nombre, ok ? "igual" : "DISTINTO");
}

static const char *sobre_b64(const stl_xdr *x) {
  if (x->desbordo) return "(desbordo)";
  stl_b64_codificar(stl_tx_sobre(x), stl_tx_sobre_n(x), g_b64, sizeof g_b64);
  return g_b64;
}

static void hex(const uint8_t *d, size_t n, char *s) {
  static const char H[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    s[2 * i] = H[d[i] >> 4];
    s[2 * i + 1] = H[d[i] & 15];
  }
  s[2 * n] = 0;
}

int main(void) {
  char texto[260];
  uint8_t dato[32], h[32], buf[200], contrato[32], recursos[400], publica[32], copia[32], llave[64];
  stl_xdr x;
  stl_tx base;

  // 0. Las llaves de prueba: la pública sale de la semilla.
  memcpy(copia, V_SEMILLA_A, 32);  // Monocypher borra la semilla que recibe
  crypto_ed25519_key_pair(llave, publica, copia);
  if (memcmp(publica, V_PUB_A, 32)) {
    printf("la llave publica derivada de la semilla no coincide\n");
    return 1;
  }

  // 1. Direcciones
  stl_dir_codificar(STL_DIR_CUENTA, V_PUB_A, texto);
  comparar("direccion_cuenta", texto, V_CUENTA_A);
  stl_dir_codificar(STL_DIR_SEMILLA, V_SEMILLA_A, texto);
  comparar("direccion_semilla", texto, V_SECRETA_A);
  int vuelta = stl_dir_decodificar(V_CUENTA_B, STL_DIR_CUENTA, dato) && !memcmp(dato, V_PUB_B, 32);
  comparar("direccion_leer", vuelta ? "ok" : "mal", "ok");
  char alterada[57];
  strcpy(alterada, V_CUENTA_B);
  alterada[20] = alterada[20] == 'A' ? 'B' : 'A';
  comparar("direccion_alterada_rechazada",
           stl_dir_decodificar(alterada, STL_DIR_CUENTA, dato) ? "aceptada" : "rechazada", "rechazada");

  // 2. Contratos de token a partir del activo
  stl_activo usdc = {false, "USDC", {0}};
  stl_dir_decodificar(V_USDC_EMISOR, STL_DIR_CUENTA, usdc.emisor);
  stl_activo xlm = {true, "", {0}};
  stl_contrato_de_activo(&CRIPTO, V_ID_RED, &xlm, contrato);
  stl_dir_codificar(STL_DIR_CONTRATO, contrato, texto);
  comparar("contrato_xlm", texto, V_CONTRATO_NATIVO);
  stl_contrato_de_activo(&CRIPTO, V_ID_RED, &usdc, dato);
  stl_dir_codificar(STL_DIR_CONTRATO, dato, texto);
  comparar("contrato_usdc", texto, V_CONTRATO_USDC);

  // 3. Pagos clásicos
  memset(&base, 0, sizeof base);
  memcpy(base.origen, V_PUB_A, 32);
  base.comision = 100;
  base.secuencia = 123456789;
  base.valido_hasta = 1800000000;
  base.con_memo = true;
  base.memo = 4242;
  stl_tx_iniciar(&x, g_tx, sizeof g_tx, V_ID_RED);
  stl_tx_pago(&x, &base, V_PUB_B, NULL, 12345678);
  stl_tx_firmar(&x, &CRIPTO, V_SEMILLA_A, V_PUB_A, h);
  hex(h, 32, texto);
  comparar("pago_xlm_hash", texto, V_TX1_HASH);
  comparar("pago_xlm_sobre", sobre_b64(&x), V_TX1_SOBRE);

  base.con_memo = false;
  stl_tx_iniciar(&x, g_tx, sizeof g_tx, V_ID_RED);
  stl_tx_pago(&x, &base, V_PUB_B, &usdc, 25000000);
  stl_tx_firmar(&x, &CRIPTO, V_SEMILLA_A, V_PUB_A, h);
  comparar("pago_usdc_sobre", sobre_b64(&x), V_TX2_SOBRE);

  // 4. Llamada al contrato del token: transfer(de, para, monto)
  const stl_llamada_token transferir = {contrato, "transfer", {V_PUB_A, V_PUB_B, NULL}, 2, 5000000};
  int n_recursos = stl_b64_decodificar(V_DATOS_SOROBAN, strlen(V_DATOS_SOROBAN), recursos, sizeof recursos);
  stl_tx_iniciar(&x, g_tx, sizeof g_tx, V_ID_RED);
  stl_tx_llamar_token(&x, &base, &transferir, NULL, 0);
  stl_tx_sin_firma(&x);
  comparar("transferir_para_simular", sobre_b64(&x), V_TX3_SIM);

  base.comision = V_TX3_COMISION;
  stl_tx_iniciar(&x, g_tx, sizeof g_tx, V_ID_RED);
  stl_tx_llamar_token(&x, &base, &transferir, recursos, (size_t)n_recursos);
  stl_tx_firmar(&x, &CRIPTO, V_SEMILLA_A, V_PUB_A, h);
  comparar("transferir_sobre", sobre_b64(&x), V_TX3_SOBRE);

  stl_xdr_iniciar(&x, buf, sizeof buf);
  stl_autorizacion_token(&x, &transferir);
  stl_b64_codificar(buf, x.n, g_b64, sizeof g_b64);
  comparar("autorizacion", g_b64, V_AUTORIZACION);

  // 5. Consultas al nodo y lectura de eventos
  size_t n = stl_scval_simbolo(buf, sizeof buf, "transfer");
  stl_b64_codificar(buf, n, g_b64, sizeof g_b64);
  comparar("tema_transfer", g_b64, V_TEMA_TRANSFER);
  n = stl_scval_cuenta(buf, V_PUB_B);
  stl_b64_codificar(buf, n, g_b64, sizeof g_b64);
  comparar("tema_cuenta", g_b64, V_TEMA_CUENTA_B);
  n = stl_clave_cuenta(buf, V_PUB_A);
  stl_b64_codificar(buf, n, g_b64, sizeof g_b64);
  comparar("clave_cuenta", g_b64, V_CLAVE_CUENTA_A);

  int64_t monto = 0;
  bool con_memo = false;
  uint64_t memo = 0;
  int l = stl_b64_decodificar(V_EVENTO_ENTERO, strlen(V_EVENTO_ENTERO), buf, sizeof buf);
  int ok = stl_evento_monto(buf, (size_t)l, &monto, &con_memo, &memo) && monto == 5000000 && !con_memo;
  comparar("evento_entero", ok ? "ok" : "mal", "ok");
  l = stl_b64_decodificar(V_EVENTO_MAPA, strlen(V_EVENTO_MAPA), buf, sizeof buf);
  ok = stl_evento_monto(buf, (size_t)l, &monto, &con_memo, &memo) && monto == 12345678 && con_memo && memo == 4242;
  comparar("evento_mapa_con_memo", ok ? "ok" : "mal", "ok");

  // 6. Pedido de pago (SEP-7)
  stl_sep7_pago(texto, sizeof texto, V_PUB_B, 25000000, &usdc, true, 4242);
  comparar("sep7", texto, V_SEP7);

  // 7. Montos: texto con 7 decimales <-> unidades mínimas
  int64_t unidades = 0;
  ok = stl_monto_leer("2.5", &unidades) && unidades == 25000000;
  comparar("monto_leer", ok ? "ok" : "mal", "ok");
  stl_monto_texto(12345678, texto, sizeof texto);
  comparar("monto_texto", texto, "1.2345678");

  printf("\n%d de %d iguales\n", g_pruebas - g_fallos, g_pruebas);
  return g_fallos ? 1 : 0;
}
