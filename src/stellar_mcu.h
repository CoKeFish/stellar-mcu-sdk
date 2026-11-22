// Stellar MCU SDK core: the minimum of Stellar that runs inside the device to build, sign and read
// payments without going through a server. Portable C99: it includes nothing from the chip and
// allocates no memory. The board provides SHA-256 and the Ed25519 signature (stl_crypto); the
// network side lives in the platform adapter (platforms/).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- What the board provides ----
typedef struct {
  void (*sha256)(const uint8_t *data, size_t n, uint8_t out[32]);
  void (*sign)(const uint8_t seed[32], const uint8_t public_key[32], const uint8_t *message, size_t n,
               uint8_t signature[64]);
} stl_crypto;

// ---- base64 ----
size_t stl_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap);
int stl_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap);

// ---- Addresses (SEP-23) ----
#define STL_ADDR_ACCOUNT 0x30   // G...
#define STL_ADDR_SEED 0x90      // S...
#define STL_ADDR_CONTRACT 0x10  // C...
bool stl_addr_encode(uint8_t type, const uint8_t data[32], char out[57]);
bool stl_addr_decode(const char *text, uint8_t type, uint8_t data[32]);

// ---- Amounts: text with up to 7 decimals <-> smallest units (stroops) ----
bool stl_amount_parse(const char *text, int64_t *units);
size_t stl_amount_format(int64_t units, char *out, size_t cap);

// ---- XDR writer over a fixed buffer ----
typedef struct {
  uint8_t *buf;
  size_t cap;
  size_t n;
  bool overflow;
} stl_xdr;
void stl_xdr_init(stl_xdr *x, uint8_t *buf, size_t cap);

// ---- Assets ----
typedef struct {
  bool native;
  char code[13];  // up to 12 characters
  uint8_t issuer[32];
} stl_asset;
// Address of an asset's token contract (SAC) on a network.
void stl_asset_contract(const stl_crypto *c, const uint8_t network_id[32], const stl_asset *a,
                        uint8_t contract[32]);

// ---- Transactions ----
typedef struct {
  uint8_t source[32];    // the account that pays the fee and provides the sequence number
  uint32_t fee;          // total, in stroops
  int64_t sequence;      // the account's sequence number plus one
  uint64_t valid_until;  // Unix time; 0 = no limit
  // Numeric memo. Only stl_tx_payment writes it: the network rejects a contract call that carries
  // a memo (txSOROBAN_INVALID, since Protocol 25), so stl_tx_token_call leaves it out.
  bool has_memo;
  uint64_t memo;
  // Account on whose behalf the operation is made, when it is not `source`. The device's signature
  // counts for that account when its key is registered there as a signer.
  bool has_op_source;
  uint8_t op_source[32];
} stl_tx;

// A call to a token contract (SEP-41): transfer(from, to, amount) or
// transfer_from(spender, from, to, amount). The addresses are accounts (G...).
typedef struct {
  const uint8_t *contract;     // 32 bytes
  const char *function;        // "transfer" or "transfer_from"
  const uint8_t *accounts[3];  // in the order of the arguments
  uint8_t n_accounts;
  int64_t amount;
} stl_token_call;

// The buffer ends up as: [network id (32)][envelope type (4)][transaction...][signatures].
// What gets signed is the SHA-256 of everything up to the end of the transaction; the envelope
// starts at buf+32.
void stl_tx_begin(stl_xdr *x, uint8_t *buf, size_t cap, const uint8_t network_id[32]);
void stl_tx_payment(stl_xdr *x, const stl_tx *t, const uint8_t destination[32], const stl_asset *a,
                    int64_t amount);
// soroban_data = the data returned by the simulation (NULL to build the transaction that is sent
// to be simulated). Never writes the memo of `t`.
void stl_tx_token_call(stl_xdr *x, const stl_tx *t, const stl_token_call *call, const uint8_t *soroban_data,
                       size_t soroban_data_len);
// The authorization that call needs, built by the device (to compare it with the node's).
void stl_token_auth(stl_xdr *x, const stl_token_call *call);
void stl_tx_sign(stl_xdr *x, const stl_crypto *c, const uint8_t seed[32], const uint8_t public_key[32],
                 uint8_t hash[32]);
void stl_tx_unsigned(stl_xdr *x);
static inline const uint8_t *stl_tx_envelope(const stl_xdr *x) { return x->buf + 32; }
static inline size_t stl_tx_envelope_len(const stl_xdr *x) { return x->n - 32; }

// ---- Node queries ----
size_t stl_account_key(uint8_t out[40], const uint8_t account[32]);
bool stl_account_parse(const uint8_t *in, size_t n, int64_t *balance, int64_t *sequence);
size_t stl_scval_symbol(uint8_t *out, size_t cap, const char *symbol);
size_t stl_scval_account(uint8_t out[44], const uint8_t account[32]);
// Amount of a "transfer" event: accepts both SEP-41 formats (integer, or map with a memo).
bool stl_event_amount(const uint8_t *value, size_t n, int64_t *amount, bool *has_memo, uint64_t *memo);

// ---- Minimal JSON: looks for "key": in a text that was already received ----
const char *stl_json_string(const char *json, const char *key, size_t *n);
bool stl_json_int(const char *json, const char *key, int64_t *value);

// ---- Payment request (SEP-7) ----
size_t stl_sep7_pay(char *out, size_t cap, const uint8_t destination[32], int64_t amount, const stl_asset *a,
                    bool has_memo, uint64_t memo);

#ifdef __cplusplus
}
#endif
