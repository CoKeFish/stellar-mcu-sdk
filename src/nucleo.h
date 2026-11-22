// Núcleo del SDK: lo mínimo de Stellar que corre dentro del terminal para armar, firmar y leer
// pagos sin pasar por un servidor. C99 portable: no incluye nada del chip ni reserva memoria.
// La placa aporta SHA-256 y la firma Ed25519 (stl_cripto); la red está en nodo.h.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Lo que aporta la placa ----
typedef struct {
  void (*sha256)(const uint8_t *datos, size_t n, uint8_t salida[32]);
  void (*firmar)(const uint8_t semilla[32], const uint8_t publica[32], const uint8_t *mensaje, size_t n,
                 uint8_t firma[64]);
} stl_cripto;

// ---- base64 ----
size_t stl_b64_codificar(const uint8_t *entrada, size_t n, char *salida, size_t cap);
int stl_b64_decodificar(const char *entrada, size_t n, uint8_t *salida, size_t cap);

// ---- Direcciones (SEP-23) ----
#define STL_DIR_CUENTA 0x30    // G...
#define STL_DIR_SEMILLA 0x90   // S...
#define STL_DIR_CONTRATO 0x10  // C...
bool stl_dir_codificar(uint8_t tipo, const uint8_t dato[32], char salida[57]);
bool stl_dir_decodificar(const char *texto, uint8_t tipo, uint8_t dato[32]);

// ---- Montos: texto con hasta 7 decimales <-> unidades mínimas ----
bool stl_monto_leer(const char *texto, int64_t *unidades);
size_t stl_monto_texto(int64_t unidades, char *salida, size_t cap);

// ---- Escritor XDR sobre un búfer fijo ----
typedef struct {
  uint8_t *buf;
  size_t cap;
  size_t n;
  bool desbordo;
} stl_xdr;
void stl_xdr_iniciar(stl_xdr *x, uint8_t *buf, size_t cap);

// ---- Activos ----
typedef struct {
  bool nativo;
  char codigo[13];  // hasta 12 letras
  uint8_t emisor[32];
} stl_activo;
// Dirección del contrato del token (SAC) de un activo en una red.
void stl_contrato_de_activo(const stl_cripto *c, const uint8_t id_red[32], const stl_activo *a,
                            uint8_t contrato[32]);

// ---- Transacciones ----
typedef struct {
  uint8_t origen[32];     // la cuenta que paga la comisión y pone la secuencia
  uint32_t comision;      // total, en stroops
  int64_t secuencia;      // la de la cuenta más uno
  uint64_t valido_hasta;  // tiempo Unix; 0 = sin límite
  bool con_memo;
  uint64_t memo;          // memo numérico
  // Cuenta en cuyo nombre se hace la operación, si no es `origen`. La firma del terminal vale por
  // esa cuenta cuando su llave está registrada en ella como firmante.
  bool con_origen_op;
  uint8_t origen_op[32];
} stl_tx;

// Una llamada a un contrato de token (SEP-41): transfer(de, para, monto) o
// transfer_from(gastador, de, para, monto). Las direcciones son cuentas (G...).
typedef struct {
  const uint8_t *contrato;    // 32 bytes
  const char *funcion;        // "transfer" o "transfer_from"
  const uint8_t *cuentas[3];  // en el orden de los argumentos
  uint8_t n_cuentas;
  int64_t monto;
} stl_llamada_token;

// El búfer queda así: [id de red (32)][tipo de sobre (4)][transacción...][firmas].
// Lo que se firma es el SHA-256 de todo hasta el final de la transacción; el sobre empieza en buf+32.
void stl_tx_iniciar(stl_xdr *x, uint8_t *buf, size_t cap, const uint8_t id_red[32]);
void stl_tx_pago(stl_xdr *x, const stl_tx *t, const uint8_t destino[32], const stl_activo *a, int64_t monto);
// recursos = datos que devuelve la simulación (NULL para armar la transacción que se manda a simular).
void stl_tx_llamar_token(stl_xdr *x, const stl_tx *t, const stl_llamada_token *llamada, const uint8_t *recursos,
                         size_t n_recursos);
// La autorización que esa llamada necesita, armada por el aparato (para compararla con la del nodo).
void stl_autorizacion_token(stl_xdr *x, const stl_llamada_token *llamada);
void stl_tx_firmar(stl_xdr *x, const stl_cripto *c, const uint8_t semilla[32], const uint8_t publica[32],
                   uint8_t hash[32]);
void stl_tx_sin_firma(stl_xdr *x);
static inline const uint8_t *stl_tx_sobre(const stl_xdr *x) { return x->buf + 32; }
static inline size_t stl_tx_sobre_n(const stl_xdr *x) { return x->n - 32; }

// ---- Consultas al nodo ----
size_t stl_clave_cuenta(uint8_t salida[40], const uint8_t cuenta[32]);
bool stl_cuenta_leer(const uint8_t *entrada, size_t n, int64_t *saldo, int64_t *secuencia);
size_t stl_scval_simbolo(uint8_t *salida, size_t cap, const char *simbolo);
size_t stl_scval_cuenta(uint8_t salida[44], const uint8_t cuenta[32]);
// Monto de un evento «transfer»: acepta los dos formatos de SEP-41 (entero o mapa con memo).
bool stl_evento_monto(const uint8_t *valor, size_t n, int64_t *monto, bool *con_memo, uint64_t *memo);

// ---- JSON mínimo: busca "clave": en un texto ya recibido ----
const char *stl_json_texto(const char *json, const char *clave, size_t *n);
bool stl_json_entero(const char *json, const char *clave, int64_t *valor);

// ---- Pedido de pago (SEP-7) ----
size_t stl_sep7_pago(char *salida, size_t cap, const uint8_t destino[32], int64_t monto, const stl_activo *a,
                     bool con_memo, uint64_t memo);

#ifdef __cplusplus
}
#endif
