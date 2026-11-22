# Referencia de la API

[English](../api.md) · **Español**

Todo lo que expone el SDK, en el orden en que se usa. El núcleo es `src/stellar_mcu.h`; el cliente
de red para ESP32 es `platforms/esp32-arduino/stellar_rpc.h`. Un programa completo que usa los dos
está en [`examples/esp32_pay_and_confirm`](../../examples/esp32_pay_and_confirm/src/main.cpp).

El código y sus comentarios están en inglés; esta página explica cada función en español.

Convenciones:

- Las cuentas, las semillas, los contratos y los identificadores de red son arreglos de 32 bytes.
  Las direcciones de texto (`G...`, `S...`, `C...`) se convierten con `stl_addr_encode` y
  `stl_addr_decode`.
- Los montos son `int64_t` en la unidad mínima (1 XLM = 10 000 000).
- Nada reserva memoria: cada búfer lo pasas tú, con su capacidad.
- Las funciones que devuelven un largo devuelven `0` cuando la salida no cabe.

## Lo que aporta el microcontrolador

```c
typedef struct {
  void (*sha256)(const uint8_t *data, size_t n, uint8_t out[32]);
  void (*sign)(const uint8_t seed[32], const uint8_t public_key[32], const uint8_t *message, size_t n,
               uint8_t signature[64]);
} stl_crypto;
```

El núcleo no trae criptografía propia. Le das una función de SHA-256 y una de firma Ed25519; el
ejemplo usa mbedTLS y Monocypher.

## Direcciones, montos y base64

| Función | Qué hace |
|---|---|
| `bool stl_addr_encode(uint8_t type, const uint8_t data[32], char out[57])` | De 32 bytes a dirección de texto. `type` es `STL_ADDR_ACCOUNT` (`G...`), `STL_ADDR_SEED` (`S...`) o `STL_ADDR_CONTRACT` (`C...`) |
| `bool stl_addr_decode(const char *text, uint8_t type, uint8_t data[32])` | De dirección de texto a 32 bytes. Devuelve `false` si el largo, el tipo o la suma de control están mal |
| `bool stl_amount_parse(const char *text, int64_t *units)` | De `"12.5"` a `125000000`. Hasta 7 decimales, solo dígitos y un punto, sin coma flotante |
| `size_t stl_amount_format(int64_t units, char *out, size_t cap)` | De `12345678` a `"1.2345678"`, sin ceros al final |
| `size_t stl_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap)` | De bytes a texto en base64, terminado en cero. Devuelve el largo |
| `int stl_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap)` | De texto en base64 a bytes. Devuelve la cantidad de bytes, o `-1` si hay un carácter inválido o no cabe |

## Activos

```c
typedef struct {
  bool native;        // true para XLM
  char code[13];      // hasta 12 letras
  uint8_t issuer[32];
} stl_asset;
```

| Función | Qué hace |
|---|---|
| `void stl_asset_contract(const stl_crypto *c, const uint8_t network_id[32], const stl_asset *a, uint8_t contract[32])` | Dirección del contrato del token (SAC) de un activo en una red. `network_id` es el SHA-256 de la frase de la red |

## Transacciones

```c
typedef struct {
  uint8_t source[32];    // la cuenta que paga la comisión y pone la secuencia
  uint32_t fee;          // total, en stroops
  int64_t sequence;      // la de la cuenta más uno
  uint64_t valid_until;  // tiempo Unix; 0 = sin límite
  bool has_memo;         // el memo solo lo escribe stl_tx_payment
  uint64_t memo;         // memo numérico
  bool has_op_source;    // la operación se hace en nombre de otra cuenta
  uint8_t op_source[32];
} stl_tx;

typedef struct {
  const uint8_t *contract;     // 32 bytes
  const char *function;        // "transfer" o "transfer_from"
  const uint8_t *accounts[3];  // en el orden de los argumentos
  uint8_t n_accounts;
  int64_t amount;
} stl_token_call;
```

Una transacción se escribe en un búfer tuyo a través de un escritor `stl_xdr`. Si el búfer queda
corto, el campo `overflow` del escritor pasa a `true`: revísalo antes de enviar.

| Función | Qué hace |
|---|---|
| `void stl_tx_begin(stl_xdr *x, uint8_t *buf, size_t cap, const uint8_t network_id[32])` | Empieza una transacción en `buf` |
| `void stl_tx_payment(stl_xdr *x, const stl_tx *t, const uint8_t destination[32], const stl_asset *a, int64_t amount)` | Un pago clásico. `a` es `NULL` para XLM |
| `void stl_tx_token_call(stl_xdr *x, const stl_tx *t, const stl_token_call *call, const uint8_t *soroban_data, size_t soroban_data_len)` | Una llamada a un contrato de token (SEP-41). Pasa `NULL` en `soroban_data` para armar la transacción que se manda a `simulateTransaction`; después pasa el `transactionData` que devolvió la simulación para armar la que se firma. Nunca escribe el memo, aunque `has_memo` esté puesto: la red rechaza una llamada a contrato que lo lleve (`txSOROBAN_INVALID`, desde el protocolo 25; [Stellar Docs](https://developers.stellar.org/docs/build/guides/transactions/pooled-accounts-muxed-accounts-memos)) |
| `void stl_token_auth(stl_xdr *x, const stl_token_call *call)` | La autorización que esa llamada necesita, armada por el aparato, para compararla con la que propone el nodo |
| `void stl_tx_sign(stl_xdr *x, const stl_crypto *c, const uint8_t seed[32], const uint8_t public_key[32], uint8_t hash[32])` | Firma y agrega la firma. Deja el hash de la transacción en `hash` |
| `void stl_tx_unsigned(stl_xdr *x)` | Cierra el sobre sin firmas, para simular |
| `const uint8_t *stl_tx_envelope(const stl_xdr *x)` y `size_t stl_tx_envelope_len(const stl_xdr *x)` | El sobre terminado y su largo: se pasa a base64 y se envía a `sendTransaction` |

Para un pago clásico alcanza un búfer de 1 KB; para una llamada a un contrato de token, reserva 2 KB.

`stl_xdr_init(stl_xdr *x, uint8_t *buf, size_t cap)` inicia un escritor sobre un búfer sin empezar
una transacción; es lo que se usa con `stl_token_auth`.

## Consultas al nodo

Arman las piezas que necesita una consulta a Stellar RPC y leen lo que vuelve.

| Función | Qué hace |
|---|---|
| `size_t stl_account_key(uint8_t out[40], const uint8_t account[32])` | La clave de una cuenta en el ledger, para `getLedgerEntries` |
| `bool stl_account_parse(const uint8_t *in, size_t n, int64_t *balance, int64_t *sequence)` | Lee el saldo y la secuencia de la entrada de cuenta que devuelve el nodo |
| `size_t stl_scval_symbol(uint8_t *out, size_t cap, const char *symbol)` | Un símbolo como `"transfer"`, para filtrar eventos por tema |
| `size_t stl_scval_account(uint8_t out[44], const uint8_t account[32])` | Una cuenta como tema de un evento |
| `bool stl_event_amount(const uint8_t *value, size_t n, int64_t *amount, bool *has_memo, uint64_t *memo)` | El monto de un evento `transfer`, y su memo si lo trae. Acepta los dos formatos de SEP-41: el entero solo o el mapa con el memo |

## JSON mínimo

Lo justo para leer una respuesta del nodo que ya está en memoria. Buscan `"clave":` en el texto.

| Función | Qué hace |
|---|---|
| `const char *stl_json_string(const char *json, const char *key, size_t *n)` | Puntero al valor de texto y su largo; `NULL` si no está |
| `bool stl_json_int(const char *json, const char *key, int64_t *value)` | Un valor entero, con comillas o sin ellas |

## Pedido de pago (SEP-7)

| Función | Qué hace |
|---|---|
| `size_t stl_sep7_pay(char *out, size_t cap, const uint8_t destination[32], int64_t amount, const stl_asset *a, bool has_memo, uint64_t memo)` | Arma el enlace `web+stellar:pay?...` que abre una billetera, para mostrarlo como QR. `a` es `NULL` para XLM |
| `size_t stl_sep7_network(char *out, size_t cap, size_t len, const char *passphrase)` | Agrega la red a un pedido armado por `stl_sep7_pay`. SEP-7 la pide en cualquier red que no sea la pública, como testnet. `len` es lo que devolvió `stl_sep7_pay` |

## Cliente de red para ESP32

`platforms/esp32-arduino/stellar_rpc.h` hace consultas JSON-RPC por HTTPS a un nodo de Stellar RPC.
Verifica el certificado del nodo contra la raíz de `root_ca.h` y mantiene la conexión abierta entre
consultas.

| Función | Qué hace |
|---|---|
| `bool stellar_rpc::call(const char *method, const char *paramsJson)` | Hace la consulta. `paramsJson` es el JSON de los parámetros o `nullptr`. Devuelve `false` si no hubo respuesta válida |
| `const char *stellar_rpc::body()` | La respuesta de la última consulta, hasta donde cabe en el búfer |
| `bool stellar_rpc::truncated()` | La respuesta no cupo entera |
| `char *stellar_rpc::params()` y `size_t stellar_rpc::paramsCap()` | Un búfer para armar los parámetros de la siguiente consulta |
| `void stellar_rpc::close()` | Cierra la conexión y libera su memoria |

El nodo por defecto es `soroban-testnet.stellar.org`. Para usar otro, define `STELLAR_RPC_HOST` al
compilar, y `STELLAR_RPC_PATH` si su dirección lleva una ruta; otro nodo puede necesitar otro
certificado raíz en `root_ca.h`.
