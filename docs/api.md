# API reference

**English** · [Español](es/api.md)

Everything the SDK exposes, in the order you would use it. The core is `src/stellar_mcu.h`; the
ESP32 network client is `platforms/esp32-arduino/stellar_rpc.h`. A complete program that uses both
is in [`examples/esp32_pay_and_confirm`](../examples/esp32_pay_and_confirm/src/main.cpp).

Conventions:

- Accounts, seeds, contracts and network ids are raw 32-byte arrays. Text addresses (`G...`,
  `S...`, `C...`) are converted with `stl_addr_encode` and `stl_addr_decode`.
- Amounts are `int64_t` in the smallest unit (1 XLM = 10 000 000).
- Nothing allocates memory: you pass every buffer, with its capacity.
- Functions that return a length return `0` when the output does not fit.

## What the board provides

```c
typedef struct {
  void (*sha256)(const uint8_t *data, size_t n, uint8_t out[32]);
  void (*sign)(const uint8_t seed[32], const uint8_t public_key[32], const uint8_t *message, size_t n,
               uint8_t signature[64]);
} stl_crypto;
```

The core has no cryptography of its own. You give it a SHA-256 function and an Ed25519 signing
function; the example uses mbedTLS and Monocypher.

## Addresses, amounts and base64

| Function | What it does |
|---|---|
| `bool stl_addr_encode(uint8_t type, const uint8_t data[32], char out[57])` | 32 bytes to a text address. `type` is `STL_ADDR_ACCOUNT` (`G...`), `STL_ADDR_SEED` (`S...`) or `STL_ADDR_CONTRACT` (`C...`) |
| `bool stl_addr_decode(const char *text, uint8_t type, uint8_t data[32])` | Text address to 32 bytes. Returns `false` if the length, the type or the checksum is wrong |
| `bool stl_amount_parse(const char *text, int64_t *units)` | `"12.5"` to `125000000`. Up to 7 decimals, digits and one dot only, no floating point |
| `size_t stl_amount_format(int64_t units, char *out, size_t cap)` | `12345678` to `"1.2345678"`, without trailing zeros |
| `size_t stl_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap)` | Bytes to base64 text, null-terminated. Returns the length |
| `int stl_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap)` | Base64 text to bytes. Returns the number of bytes, or `-1` on a bad character or if it does not fit |

## Assets

```c
typedef struct {
  bool native;        // true for XLM
  char code[13];      // up to 12 characters
  uint8_t issuer[32];
} stl_asset;
```

| Function | What it does |
|---|---|
| `void stl_asset_contract(const stl_crypto *c, const uint8_t network_id[32], const stl_asset *a, uint8_t contract[32])` | Address of the asset's token contract (SAC) on a network. `network_id` is the SHA-256 of the network passphrase |

## Transactions

```c
typedef struct {
  uint8_t source[32];    // the account that pays the fee and provides the sequence number
  uint32_t fee;          // total, in stroops
  int64_t sequence;      // the account's sequence number plus one
  uint64_t valid_until;  // Unix time; 0 = no limit
  bool has_memo;
  uint64_t memo;         // numeric memo
  bool has_op_source;    // the operation is made on behalf of another account
  uint8_t op_source[32];
} stl_tx;

typedef struct {
  const uint8_t *contract;     // 32 bytes
  const char *function;        // "transfer" or "transfer_from"
  const uint8_t *accounts[3];  // in the order of the arguments
  uint8_t n_accounts;
  int64_t amount;
} stl_token_call;
```

A transaction is written into a buffer you provide through an `stl_xdr` writer. If the buffer is
too small, the writer's `overflow` field becomes `true`: check it before sending.

| Function | What it does |
|---|---|
| `void stl_tx_begin(stl_xdr *x, uint8_t *buf, size_t cap, const uint8_t network_id[32])` | Starts a transaction in `buf` |
| `void stl_tx_payment(stl_xdr *x, const stl_tx *t, const uint8_t destination[32], const stl_asset *a, int64_t amount)` | A classic payment. `a` is `NULL` for XLM |
| `void stl_tx_token_call(stl_xdr *x, const stl_tx *t, const stl_token_call *call, const uint8_t *soroban_data, size_t soroban_data_len)` | A call to a token contract (SEP-41). Pass `NULL` as `soroban_data` to build the transaction that is sent to `simulateTransaction`; then pass the `transactionData` the simulation returned to build the one that is signed |
| `void stl_token_auth(stl_xdr *x, const stl_token_call *call)` | The authorization that call needs, built by the device, to compare it with the one the node proposes |
| `void stl_tx_sign(stl_xdr *x, const stl_crypto *c, const uint8_t seed[32], const uint8_t public_key[32], uint8_t hash[32])` | Signs and appends the signature. Leaves the transaction hash in `hash` |
| `void stl_tx_unsigned(stl_xdr *x)` | Closes the envelope with no signatures, for simulation |
| `const uint8_t *stl_tx_envelope(const stl_xdr *x)` and `size_t stl_tx_envelope_len(const stl_xdr *x)` | The finished envelope and its length: encode it in base64 and send it to `sendTransaction` |

A 1 KB buffer is enough for a classic payment; for a token contract call, reserve 2 KB.

`stl_xdr_init(stl_xdr *x, uint8_t *buf, size_t cap)` starts a writer over a buffer without
starting a transaction; it is what you use with `stl_token_auth`.

## Node queries

These build the pieces a Stellar RPC request needs and read what comes back.

| Function | What it does |
|---|---|
| `size_t stl_account_key(uint8_t out[40], const uint8_t account[32])` | The ledger key of an account, for `getLedgerEntries` |
| `bool stl_account_parse(const uint8_t *in, size_t n, int64_t *balance, int64_t *sequence)` | Reads the balance and the sequence number from the account entry the node returns |
| `size_t stl_scval_symbol(uint8_t *out, size_t cap, const char *symbol)` | A symbol such as `"transfer"`, to filter events by topic |
| `size_t stl_scval_account(uint8_t out[44], const uint8_t account[32])` | An account as an event topic |
| `bool stl_event_amount(const uint8_t *value, size_t n, int64_t *amount, bool *has_memo, uint64_t *memo)` | The amount of a `transfer` event, and its memo if it has one. Accepts both SEP-41 formats: the integer alone, or the map with the memo |

## Minimal JSON

Enough to read a node response that is already in memory. They look for `"key":` in the text.

| Function | What it does |
|---|---|
| `const char *stl_json_string(const char *json, const char *key, size_t *n)` | Pointer to the string value and its length; `NULL` if it is not there |
| `bool stl_json_int(const char *json, const char *key, int64_t *value)` | An integer value, quoted or not |

## Payment request (SEP-7)

| Function | What it does |
|---|---|
| `size_t stl_sep7_pay(char *out, size_t cap, const uint8_t destination[32], int64_t amount, const stl_asset *a, bool has_memo, uint64_t memo)` | Builds the `web+stellar:pay?...` link a wallet can open, to show it as a QR code. `a` is `NULL` for XLM |

## ESP32 network client

`platforms/esp32-arduino/stellar_rpc.h` makes JSON-RPC requests to a Stellar RPC node over HTTPS.
It verifies the node's certificate against the root in `root_ca.h` and keeps the connection open
between requests.

| Function | What it does |
|---|---|
| `bool stellar_rpc::call(const char *method, const char *paramsJson)` | Makes the request. `paramsJson` is the JSON of the parameters or `nullptr`. Returns `false` if there was no valid response |
| `const char *stellar_rpc::body()` | The response to the last request, as far as it fits in the buffer |
| `bool stellar_rpc::truncated()` | The response did not fit entirely |
| `char *stellar_rpc::params()` and `size_t stellar_rpc::paramsCap()` | A buffer to build the parameters of the next request |
| `void stellar_rpc::close()` | Closes the connection and frees its memory |

The node is `soroban-testnet.stellar.org` by default. To use another one, define
`STELLAR_RPC_HOST` at build time; another node may need another root certificate in `root_ca.h`.
