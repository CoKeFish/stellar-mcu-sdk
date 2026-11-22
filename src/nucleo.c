// Núcleo del SDK (ver nucleo.h). Sin memoria dinámica y sin dependencias del chip.
#include "nucleo.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- base64
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t stl_b64_codificar(const uint8_t *e, size_t n, char *s, size_t cap) {
  size_t largo = ((n + 2) / 3) * 4;
  if (largo + 1 > cap) return 0;
  size_t j = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)e[i] << 16;
    if (i + 1 < n) v |= (uint32_t)e[i + 1] << 8;
    if (i + 2 < n) v |= e[i + 2];
    s[j++] = B64[(v >> 18) & 63];
    s[j++] = B64[(v >> 12) & 63];
    s[j++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
    s[j++] = i + 2 < n ? B64[v & 63] : '=';
  }
  s[j] = 0;
  return j;
}

static int b64_valor(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

int stl_b64_decodificar(const char *e, size_t n, uint8_t *s, size_t cap) {
  uint32_t acumulado = 0;
  int bits = 0;
  size_t j = 0;
  for (size_t i = 0; i < n; i++) {
    if (e[i] == '=') break;
    int v = b64_valor(e[i]);
    if (v < 0) return -1;
    acumulado = (acumulado << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (j >= cap) return -1;
      s[j++] = (uint8_t)(acumulado >> bits);
    }
  }
  return (int)j;
}

// ---------------------------------------------------------------- direcciones (SEP-23)
static const char B32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

static uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t crc = 0;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

bool stl_dir_codificar(uint8_t tipo, const uint8_t dato[32], char salida[57]) {
  uint8_t crudo[35];
  crudo[0] = tipo;
  memcpy(crudo + 1, dato, 32);
  uint16_t crc = crc16(crudo, 33);
  crudo[33] = (uint8_t)crc;
  crudo[34] = (uint8_t)(crc >> 8);
  for (int i = 0; i < 7; i++) {  // 35 bytes = 7 grupos de 5 bytes = 56 letras
    const uint8_t *g = crudo + i * 5;
    uint64_t v = ((uint64_t)g[0] << 32) | ((uint64_t)g[1] << 24) | ((uint64_t)g[2] << 16) |
                 ((uint64_t)g[3] << 8) | g[4];
    for (int k = 0; k < 8; k++) salida[i * 8 + k] = B32[(v >> (35 - 5 * k)) & 31];
  }
  salida[56] = 0;
  return true;
}

bool stl_dir_decodificar(const char *texto, uint8_t tipo, uint8_t dato[32]) {
  if (strlen(texto) != 56) return false;
  uint8_t crudo[35];
  for (int i = 0; i < 7; i++) {
    uint64_t v = 0;
    for (int k = 0; k < 8; k++) {
      const char *p = strchr(B32, texto[i * 8 + k]);
      if (!p || !*p) return false;
      v = (v << 5) | (uint64_t)(p - B32);
    }
    for (int k = 0; k < 5; k++) crudo[i * 5 + k] = (uint8_t)(v >> (32 - 8 * k));
  }
  if (crudo[0] != tipo) return false;
  uint16_t crc = crc16(crudo, 33);
  if (crudo[33] != (uint8_t)crc || crudo[34] != (uint8_t)(crc >> 8)) return false;
  memcpy(dato, crudo + 1, 32);
  return true;
}

// ---------------------------------------------------------------- escritor XDR
void stl_xdr_iniciar(stl_xdr *x, uint8_t *buf, size_t cap) {
  x->buf = buf;
  x->cap = cap;
  x->n = 0;
  x->desbordo = false;
}

static void poner(stl_xdr *x, const uint8_t *d, size_t n) {
  if (x->n + n > x->cap) {
    x->desbordo = true;
    return;
  }
  memcpy(x->buf + x->n, d, n);
  x->n += n;
}

static void u32(stl_xdr *x, uint32_t v) {
  uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
  poner(x, b, 4);
}

static void u64(stl_xdr *x, uint64_t v) {
  u32(x, (uint32_t)(v >> 32));
  u32(x, (uint32_t)v);
}

// Bytes de largo fijo, rellenados con ceros hasta un múltiplo de 4.
static void fijo(stl_xdr *x, const uint8_t *d, size_t n) {
  static const uint8_t ceros[4] = {0, 0, 0, 0};
  poner(x, d, n);
  if (n % 4) poner(x, ceros, 4 - n % 4);
}

// Bytes de largo variable: el largo y después los datos.
static void variable(stl_xdr *x, const uint8_t *d, size_t n) {
  u32(x, (uint32_t)n);
  fijo(x, d, n);
}

// ---------------------------------------------------------------- activos
static void activo(stl_xdr *x, const stl_activo *a) {
  if (!a || a->nativo) {
    u32(x, 0);  // ASSET_TYPE_NATIVE
    return;
  }
  uint8_t codigo[12] = {0};
  size_t largo = 0;  // largo del código, hasta 12 letras (sin strnlen, que no es de C99)
  while (largo < 12 && a->codigo[largo]) largo++;
  memcpy(codigo, a->codigo, largo);
  if (largo <= 4) {
    u32(x, 1);  // ASSET_TYPE_CREDIT_ALPHANUM4
    fijo(x, codigo, 4);
  } else {
    u32(x, 2);  // ASSET_TYPE_CREDIT_ALPHANUM12
    fijo(x, codigo, 12);
  }
  u32(x, 0);  // llave pública Ed25519
  fijo(x, a->emisor, 32);
}

void stl_contrato_de_activo(const stl_cripto *c, const uint8_t id_red[32], const stl_activo *a,
                            uint8_t contrato[32]) {
  uint8_t buf[96];
  stl_xdr x;
  stl_xdr_iniciar(&x, buf, sizeof buf);
  u32(&x, 8);  // ENVELOPE_TYPE_CONTRACT_ID
  fijo(&x, id_red, 32);
  u32(&x, 1);  // CONTRACT_ID_PREIMAGE_FROM_ASSET
  activo(&x, a);
  c->sha256(buf, x.n, contrato);
}

// ---------------------------------------------------------------- transacciones
void stl_tx_iniciar(stl_xdr *x, uint8_t *buf, size_t cap, const uint8_t id_red[32]) {
  stl_xdr_iniciar(x, buf, cap);
  fijo(x, id_red, 32);
  u32(x, 2);  // ENVELOPE_TYPE_TX
}

static void cabecera(stl_xdr *x, const stl_tx *t) {
  u32(x, 0);  // origen: llave Ed25519, sin multiplexar
  fijo(x, t->origen, 32);
  u32(x, t->comision);
  u64(x, (uint64_t)t->secuencia);
  if (t->valido_hasta) {
    u32(x, 1);  // PRECOND_TIME
    u64(x, 0);
    u64(x, t->valido_hasta);
  } else {
    u32(x, 0);
  }
  if (t->con_memo) {
    u32(x, 2);  // MEMO_ID
    u64(x, t->memo);
  } else {
    u32(x, 0);
  }
  u32(x, 1);  // una operación
  if (t->con_origen_op) {
    u32(x, 1);  // hecha en nombre de otra cuenta
    u32(x, 0);
    fijo(x, t->origen_op, 32);
  } else {
    u32(x, 0);  // que usa el origen de la transacción
  }
}

void stl_tx_pago(stl_xdr *x, const stl_tx *t, const uint8_t destino[32], const stl_activo *a, int64_t monto) {
  cabecera(x, t);
  u32(x, 1);  // PAYMENT
  u32(x, 0);
  fijo(x, destino, 32);
  activo(x, a);
  u64(x, (uint64_t)monto);
  u32(x, 0);  // sin datos de Soroban
}

static void direccion(stl_xdr *x, const uint8_t d[32]) {
  u32(x, 18);  // SCV_ADDRESS
  u32(x, 0);   // SC_ADDRESS_TYPE_ACCOUNT
  u32(x, 0);   // llave pública Ed25519
  fijo(x, d, 32);
}

// El contrato, la función y sus argumentos: las cuentas en orden y al final el monto.
static void llamada_token(stl_xdr *x, const stl_llamada_token *ll) {
  u32(x, 1);  // el contrato llamado
  fijo(x, ll->contrato, 32);
  variable(x, (const uint8_t *)ll->funcion, strlen(ll->funcion));
  u32(x, (uint32_t)ll->n_cuentas + 1);
  for (uint8_t i = 0; i < ll->n_cuentas; i++) direccion(x, ll->cuentas[i]);
  u32(x, 10);  // SCV_I128
  u64(x, ll->monto < 0 ? UINT64_MAX : 0);
  u64(x, (uint64_t)ll->monto);
}

void stl_autorizacion_token(stl_xdr *x, const stl_llamada_token *ll) {
  u32(x, 0);  // autoriza la cuenta que hace la operación: la cubre la firma de la transacción
  u32(x, 0);  // la función autorizada es una llamada a contrato
  llamada_token(x, ll);
  u32(x, 0);  // sin llamadas internas
}

void stl_tx_llamar_token(stl_xdr *x, const stl_tx *t, const stl_llamada_token *ll, const uint8_t *recursos,
                         size_t n_recursos) {
  cabecera(x, t);
  u32(x, 24);  // INVOKE_HOST_FUNCTION
  u32(x, 0);   // llamar a un contrato
  llamada_token(x, ll);
  if (recursos) {
    u32(x, 1);
    stl_autorizacion_token(x, ll);
    u32(x, 1);  // con datos de Soroban
    poner(x, recursos, n_recursos);
  } else {
    u32(x, 0);  // sin autorizaciones: las calcula la simulación
    u32(x, 0);
  }
}

void stl_tx_firmar(stl_xdr *x, const stl_cripto *c, const uint8_t semilla[32], const uint8_t publica[32],
                   uint8_t hash[32]) {
  uint8_t firma[64];
  c->sha256(x->buf, x->n, hash);
  c->firmar(semilla, publica, hash, 32, firma);
  u32(x, 1);                  // una firma
  fijo(x, publica + 28, 4);   // pista: los últimos 4 bytes de la llave pública
  variable(x, firma, 64);
}

void stl_tx_sin_firma(stl_xdr *x) { u32(x, 0); }

// ---------------------------------------------------------------- consultas al nodo
size_t stl_clave_cuenta(uint8_t salida[40], const uint8_t cuenta[32]) {
  memset(salida, 0, 8);  // tipo de entrada: cuenta; llave Ed25519
  memcpy(salida + 8, cuenta, 32);
  return 40;
}

static uint64_t leer_u64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}

static uint32_t leer_u32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

bool stl_cuenta_leer(const uint8_t *e, size_t n, int64_t *saldo, int64_t *secuencia) {
  if (n < 56 || leer_u32(e) != 0) return false;  // 0 = entrada de cuenta
  *saldo = (int64_t)leer_u64(e + 40);
  *secuencia = (int64_t)leer_u64(e + 48);
  return true;
}

size_t stl_scval_simbolo(uint8_t *salida, size_t cap, const char *simbolo) {
  stl_xdr x;
  stl_xdr_iniciar(&x, salida, cap);
  u32(&x, 15);  // SCV_SYMBOL
  variable(&x, (const uint8_t *)simbolo, strlen(simbolo));
  return x.desbordo ? 0 : x.n;
}

size_t stl_scval_cuenta(uint8_t salida[44], const uint8_t cuenta[32]) {
  stl_xdr x;
  stl_xdr_iniciar(&x, salida, 44);
  direccion(&x, cuenta);
  return x.n;
}

// Lee un entero de 128 bits que quepa en 64.
static bool leer_i128(const uint8_t *p, const uint8_t *fin, int64_t *v) {
  if (fin - p < 16) return false;
  uint64_t alto = leer_u64(p), bajo = leer_u64(p + 8);
  if (!((alto == 0 && (int64_t)bajo >= 0) || (alto == UINT64_MAX && (int64_t)bajo < 0))) return false;
  *v = (int64_t)bajo;
  return true;
}

bool stl_evento_monto(const uint8_t *v, size_t n, int64_t *monto, bool *con_memo, uint64_t *memo) {
  const uint8_t *p = v, *fin = v + n;
  *con_memo = false;
  if (fin - p < 4) return false;
  uint32_t tipo = leer_u32(p);
  p += 4;
  if (tipo == 10) return leer_i128(p, fin, monto);  // SCV_I128: el monto solo
  if (tipo != 17 || fin - p < 8 || leer_u32(p) != 1) return false;  // SCV_MAP presente
  uint32_t entradas = leer_u32(p + 4);
  p += 8;
  bool hay_monto = false;
  for (uint32_t i = 0; i < entradas; i++) {
    if (fin - p < 8 || leer_u32(p) != 15) return false;  // la clave es un símbolo
    uint32_t largo = leer_u32(p + 4);
    const char *clave = (const char *)(p + 8);
    size_t relleno = (largo + 3) & ~3u;
    if ((size_t)(fin - p) < 8 + relleno + 4) return false;
    p += 8 + relleno;
    uint32_t tipo_valor = leer_u32(p);
    p += 4;
    if (largo == 6 && !memcmp(clave, "amount", 6)) {
      if (tipo_valor != 10 || !leer_i128(p, fin, monto)) return false;
      p += 16;
      hay_monto = true;
    } else if (tipo_valor == 5) {  // SCV_U64: memo numérico
      if (fin - p < 8) return false;
      if (largo == 11 && !memcmp(clave, "to_muxed_id", 11)) {
        *con_memo = true;
        *memo = leer_u64(p);
      }
      p += 8;
    } else if (tipo_valor == 13 || tipo_valor == 14) {  // bytes o texto: se saltan
      if (fin - p < 4) return false;
      size_t l = (leer_u32(p) + 3) & ~3u;
      if ((size_t)(fin - p) < 4 + l) return false;
      p += 4 + l;
    } else {
      return false;
    }
  }
  return hay_monto;
}

// ---------------------------------------------------------------- JSON mínimo
static const char *despues_de_clave(const char *json, const char *clave) {
  char patron[48];
  int largo = snprintf(patron, sizeof patron, "\"%s\":", clave);
  if (largo <= 0 || (size_t)largo >= sizeof patron) return NULL;
  const char *p = strstr(json, patron);
  if (!p) return NULL;
  p += largo;
  while (*p == ' ') p++;
  return p;
}

const char *stl_json_texto(const char *json, const char *clave, size_t *n) {
  const char *p = despues_de_clave(json, clave);
  if (!p || *p != '"') return NULL;
  p++;
  const char *fin = strchr(p, '"');
  if (!fin) return NULL;
  *n = (size_t)(fin - p);
  return p;
}

bool stl_json_entero(const char *json, const char *clave, int64_t *valor) {
  const char *p = despues_de_clave(json, clave);
  if (!p) return false;
  if (*p == '"') p++;
  bool negativo = *p == '-';
  if (negativo) p++;
  if (*p < '0' || *p > '9') return false;
  int64_t v = 0;
  while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
  *valor = negativo ? -v : v;
  return true;
}

// ---------------------------------------------------------------- montos
// "12.5" -> 125000000. Sin coma flotante: hasta 7 decimales, solo dígitos y un punto.
bool stl_monto_leer(const char *texto, int64_t *unidades) {
  int64_t entero = 0, fraccion = 0;
  int digitos = 0, decimales = 0;
  const char *p = texto;
  for (; *p >= '0' && *p <= '9'; p++, digitos++) {
    if (entero > 90000000000LL) return false;  // no cabe en 64 bits con 7 decimales
    entero = entero * 10 + (*p - '0');
  }
  if (*p == '.') {
    for (p++; *p >= '0' && *p <= '9'; p++, decimales++) {
      if (decimales >= 7) return false;
      fraccion = fraccion * 10 + (*p - '0');
    }
  }
  if (*p || (!digitos && !decimales)) return false;
  for (; decimales < 7; decimales++) fraccion *= 10;
  *unidades = entero * 10000000 + fraccion;
  return true;
}

// ---------------------------------------------------------------- pedido de pago (SEP-7)
size_t stl_monto_texto(int64_t stroops, char *salida, size_t cap) {
  int largo = snprintf(salida, cap, "%lld.%07lld", (long long)(stroops / 10000000), (long long)(stroops % 10000000));
  if (largo <= 0 || (size_t)largo >= cap) return 0;
  while (salida[largo - 1] == '0') salida[--largo] = 0;
  if (salida[largo - 1] == '.') salida[--largo] = 0;
  return (size_t)largo;
}

size_t stl_sep7_pago(char *salida, size_t cap, const uint8_t destino[32], int64_t monto, const stl_activo *a,
                     bool con_memo, uint64_t memo) {
  char cuenta[57], cantidad[24];
  stl_dir_codificar(STL_DIR_CUENTA, destino, cuenta);
  stl_monto_texto(monto, cantidad, sizeof cantidad);
  int n = snprintf(salida, cap, "web+stellar:pay?destination=%s&amount=%s", cuenta, cantidad);
  if (n <= 0 || (size_t)n >= cap) return 0;
  if (a && !a->nativo) {
    stl_dir_codificar(STL_DIR_CUENTA, a->emisor, cuenta);
    n += snprintf(salida + n, cap - (size_t)n, "&asset_code=%s&asset_issuer=%s", a->codigo, cuenta);
    if ((size_t)n >= cap) return 0;
  }
  if (con_memo) {
    n += snprintf(salida + n, cap - (size_t)n, "&memo=%llu&memo_type=MEMO_ID", (unsigned long long)memo);
    if ((size_t)n >= cap) return 0;
  }
  return (size_t)n;
}
