// Stellar MCU SDK core (see stellar_mcu.h). No dynamic memory and no chip dependencies.
#include "stellar_mcu.h"

#include <stdio.h>
#include <string.h>

// ---------------------------------------------------------------- base64
static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t stl_b64_encode(const uint8_t *in, size_t n, char *out, size_t cap) {
  size_t len = ((n + 2) / 3) * 4;
  if (len + 1 > cap) return 0;
  size_t j = 0;
  for (size_t i = 0; i < n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16;
    if (i + 1 < n) v |= (uint32_t)in[i + 1] << 8;
    if (i + 2 < n) v |= in[i + 2];
    out[j++] = B64[(v >> 18) & 63];
    out[j++] = B64[(v >> 12) & 63];
    out[j++] = i + 1 < n ? B64[(v >> 6) & 63] : '=';
    out[j++] = i + 2 < n ? B64[v & 63] : '=';
  }
  out[j] = 0;
  return j;
}

static int b64_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

int stl_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap) {
  uint32_t acc = 0;
  int bits = 0;
  size_t j = 0;
  for (size_t i = 0; i < n; i++) {
    if (in[i] == '=') break;
    int v = b64_value(in[i]);
    if (v < 0) return -1;
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (j >= cap) return -1;
      out[j++] = (uint8_t)(acc >> bits);
    }
  }
  return (int)j;
}

// ---------------------------------------------------------------- addresses (SEP-23)
static const char B32[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

static uint16_t crc16(const uint8_t *d, size_t n) {
  uint16_t crc = 0;
  for (size_t i = 0; i < n; i++) {
    crc ^= (uint16_t)d[i] << 8;
    for (int b = 0; b < 8; b++) crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
  }
  return crc;
}

bool stl_addr_encode(uint8_t type, const uint8_t data[32], char out[57]) {
  uint8_t raw[35];
  raw[0] = type;
  memcpy(raw + 1, data, 32);
  uint16_t crc = crc16(raw, 33);
  raw[33] = (uint8_t)crc;
  raw[34] = (uint8_t)(crc >> 8);
  for (int i = 0; i < 7; i++) {  // 35 bytes = 7 groups of 5 bytes = 56 characters
    const uint8_t *g = raw + i * 5;
    uint64_t v = ((uint64_t)g[0] << 32) | ((uint64_t)g[1] << 24) | ((uint64_t)g[2] << 16) |
                 ((uint64_t)g[3] << 8) | g[4];
    for (int k = 0; k < 8; k++) out[i * 8 + k] = B32[(v >> (35 - 5 * k)) & 31];
  }
  out[56] = 0;
  return true;
}

bool stl_addr_decode(const char *text, uint8_t type, uint8_t data[32]) {
  if (strlen(text) != 56) return false;
  uint8_t raw[35];
  for (int i = 0; i < 7; i++) {
    uint64_t v = 0;
    for (int k = 0; k < 8; k++) {
      const char *p = strchr(B32, text[i * 8 + k]);
      if (!p || !*p) return false;
      v = (v << 5) | (uint64_t)(p - B32);
    }
    for (int k = 0; k < 5; k++) raw[i * 5 + k] = (uint8_t)(v >> (32 - 8 * k));
  }
  if (raw[0] != type) return false;
  uint16_t crc = crc16(raw, 33);
  if (raw[33] != (uint8_t)crc || raw[34] != (uint8_t)(crc >> 8)) return false;
  memcpy(data, raw + 1, 32);
  return true;
}

// ---------------------------------------------------------------- XDR writer
void stl_xdr_init(stl_xdr *x, uint8_t *buf, size_t cap) {
  x->buf = buf;
  x->cap = cap;
  x->n = 0;
  x->overflow = false;
}

static void put(stl_xdr *x, const uint8_t *d, size_t n) {
  if (x->n + n > x->cap) {
    x->overflow = true;
    return;
  }
  memcpy(x->buf + x->n, d, n);
  x->n += n;
}

static void u32(stl_xdr *x, uint32_t v) {
  uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
  put(x, b, 4);
}

static void u64(stl_xdr *x, uint64_t v) {
  u32(x, (uint32_t)(v >> 32));
  u32(x, (uint32_t)v);
}

// Fixed-length bytes, zero-padded up to a multiple of 4.
static void fixed(stl_xdr *x, const uint8_t *d, size_t n) {
  static const uint8_t zeros[4] = {0, 0, 0, 0};
  put(x, d, n);
  if (n % 4) put(x, zeros, 4 - n % 4);
}

// Variable-length bytes: the length and then the data.
static void variable(stl_xdr *x, const uint8_t *d, size_t n) {
  u32(x, (uint32_t)n);
  fixed(x, d, n);
}

// ---------------------------------------------------------------- assets
static void asset(stl_xdr *x, const stl_asset *a) {
  if (!a || a->native) {
    u32(x, 0);  // ASSET_TYPE_NATIVE
    return;
  }
  uint8_t code[12] = {0};
  size_t len = 0;  // length of the code, up to 12 characters (no strnlen: it is not C99)
  while (len < 12 && a->code[len]) len++;
  memcpy(code, a->code, len);
  if (len <= 4) {
    u32(x, 1);  // ASSET_TYPE_CREDIT_ALPHANUM4
    fixed(x, code, 4);
  } else {
    u32(x, 2);  // ASSET_TYPE_CREDIT_ALPHANUM12
    fixed(x, code, 12);
  }
  u32(x, 0);  // Ed25519 public key
  fixed(x, a->issuer, 32);
}

void stl_asset_contract(const stl_crypto *c, const uint8_t network_id[32], const stl_asset *a,
                        uint8_t contract[32]) {
  uint8_t buf[96];
  stl_xdr x;
  stl_xdr_init(&x, buf, sizeof buf);
  u32(&x, 8);  // ENVELOPE_TYPE_CONTRACT_ID
  fixed(&x, network_id, 32);
  u32(&x, 1);  // CONTRACT_ID_PREIMAGE_FROM_ASSET
  asset(&x, a);
  c->sha256(buf, x.n, contract);
}

// ---------------------------------------------------------------- transactions
void stl_tx_begin(stl_xdr *x, uint8_t *buf, size_t cap, const uint8_t network_id[32]) {
  stl_xdr_init(x, buf, cap);
  fixed(x, network_id, 32);
  u32(x, 2);  // ENVELOPE_TYPE_TX
}

// `memo`: whether this kind of transaction may carry one. A contract call may not: the network
// rejects it with txSOROBAN_INVALID (since Protocol 25), so the memo is never written there.
static void header(stl_xdr *x, const stl_tx *t, bool memo) {
  u32(x, 0);  // source: Ed25519 key, not multiplexed
  fixed(x, t->source, 32);
  u32(x, t->fee);
  u64(x, (uint64_t)t->sequence);
  if (t->valid_until) {
    u32(x, 1);  // PRECOND_TIME
    u64(x, 0);
    u64(x, t->valid_until);
  } else {
    u32(x, 0);
  }
  if (memo && t->has_memo) {
    u32(x, 2);  // MEMO_ID
    u64(x, t->memo);
  } else {
    u32(x, 0);
  }
  u32(x, 1);  // one operation
  if (t->has_op_source) {
    u32(x, 1);  // made on behalf of another account
    u32(x, 0);
    fixed(x, t->op_source, 32);
  } else {
    u32(x, 0);  // which uses the transaction's source
  }
}

void stl_tx_payment(stl_xdr *x, const stl_tx *t, const uint8_t destination[32], const stl_asset *a,
                    int64_t amount) {
  header(x, t, true);
  u32(x, 1);  // PAYMENT
  u32(x, 0);
  fixed(x, destination, 32);
  asset(x, a);
  u64(x, (uint64_t)amount);
  u32(x, 0);  // no Soroban data
}

static void address(stl_xdr *x, const uint8_t d[32]) {
  u32(x, 18);  // SCV_ADDRESS
  u32(x, 0);   // SC_ADDRESS_TYPE_ACCOUNT
  u32(x, 0);   // Ed25519 public key
  fixed(x, d, 32);
}

// The contract, the function and its arguments: the accounts in order and the amount last.
static void token_call(stl_xdr *x, const stl_token_call *call) {
  u32(x, 1);  // the contract being called
  fixed(x, call->contract, 32);
  variable(x, (const uint8_t *)call->function, strlen(call->function));
  u32(x, (uint32_t)call->n_accounts + 1);
  for (uint8_t i = 0; i < call->n_accounts; i++) address(x, call->accounts[i]);
  u32(x, 10);  // SCV_I128
  u64(x, call->amount < 0 ? UINT64_MAX : 0);
  u64(x, (uint64_t)call->amount);
}

void stl_token_auth(stl_xdr *x, const stl_token_call *call) {
  u32(x, 0);  // authorized by the account making the operation: covered by the transaction signature
  u32(x, 0);  // the authorized function is a contract call
  token_call(x, call);
  u32(x, 0);  // no sub-invocations
}

void stl_tx_token_call(stl_xdr *x, const stl_tx *t, const stl_token_call *call, const uint8_t *soroban_data,
                       size_t soroban_data_len) {
  header(x, t, false);  // a contract call cannot carry a memo
  u32(x, 24);  // INVOKE_HOST_FUNCTION
  u32(x, 0);   // call a contract
  token_call(x, call);
  if (soroban_data) {
    u32(x, 1);
    stl_token_auth(x, call);
    u32(x, 1);  // with Soroban data
    put(x, soroban_data, soroban_data_len);
  } else {
    u32(x, 0);  // no authorizations: the simulation computes them
    u32(x, 0);
  }
}

void stl_tx_sign(stl_xdr *x, const stl_crypto *c, const uint8_t seed[32], const uint8_t public_key[32],
                 uint8_t hash[32]) {
  uint8_t signature[64];
  c->sha256(x->buf, x->n, hash);
  c->sign(seed, public_key, hash, 32, signature);
  u32(x, 1);                     // one signature
  fixed(x, public_key + 28, 4);  // hint: the last 4 bytes of the public key
  variable(x, signature, 64);
}

void stl_tx_unsigned(stl_xdr *x) { u32(x, 0); }

// ---------------------------------------------------------------- node queries
size_t stl_account_key(uint8_t out[40], const uint8_t account[32]) {
  memset(out, 0, 8);  // entry type: account; Ed25519 key
  memcpy(out + 8, account, 32);
  return 40;
}

static uint64_t read_u64(const uint8_t *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}

static uint32_t read_u32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

bool stl_account_parse(const uint8_t *in, size_t n, int64_t *balance, int64_t *sequence) {
  if (n < 56 || read_u32(in) != 0) return false;  // 0 = account entry
  *balance = (int64_t)read_u64(in + 40);
  *sequence = (int64_t)read_u64(in + 48);
  return true;
}

size_t stl_scval_symbol(uint8_t *out, size_t cap, const char *symbol) {
  stl_xdr x;
  stl_xdr_init(&x, out, cap);
  u32(&x, 15);  // SCV_SYMBOL
  variable(&x, (const uint8_t *)symbol, strlen(symbol));
  return x.overflow ? 0 : x.n;
}

size_t stl_scval_account(uint8_t out[44], const uint8_t account[32]) {
  stl_xdr x;
  stl_xdr_init(&x, out, 44);
  address(&x, account);
  return x.n;
}

// Reads a 128-bit integer that fits in 64 bits.
static bool read_i128(const uint8_t *p, const uint8_t *end, int64_t *v) {
  if (end - p < 16) return false;
  uint64_t hi = read_u64(p), lo = read_u64(p + 8);
  if (!((hi == 0 && (int64_t)lo >= 0) || (hi == UINT64_MAX && (int64_t)lo < 0))) return false;
  *v = (int64_t)lo;
  return true;
}

bool stl_event_amount(const uint8_t *value, size_t n, int64_t *amount, bool *has_memo, uint64_t *memo) {
  const uint8_t *p = value, *end = value + n;
  *has_memo = false;
  if (end - p < 4) return false;
  uint32_t type = read_u32(p);
  p += 4;
  if (type == 10) return read_i128(p, end, amount);  // SCV_I128: the amount alone
  if (type != 17 || end - p < 8 || read_u32(p) != 1) return false;  // SCV_MAP present
  uint32_t entries = read_u32(p + 4);
  p += 8;
  bool has_amount = false;
  for (uint32_t i = 0; i < entries; i++) {
    if (end - p < 8 || read_u32(p) != 15) return false;  // the key is a symbol
    uint32_t len = read_u32(p + 4);
    const char *key = (const char *)(p + 8);
    size_t padded = (len + 3) & ~3u;
    if ((size_t)(end - p) < 8 + padded + 4) return false;
    p += 8 + padded;
    uint32_t value_type = read_u32(p);
    p += 4;
    if (len == 6 && !memcmp(key, "amount", 6)) {
      if (value_type != 10 || !read_i128(p, end, amount)) return false;
      p += 16;
      has_amount = true;
    } else if (value_type == 5) {  // SCV_U64: numeric memo
      if (end - p < 8) return false;
      if (len == 11 && !memcmp(key, "to_muxed_id", 11)) {
        *has_memo = true;
        *memo = read_u64(p);
      }
      p += 8;
    } else if (value_type == 13 || value_type == 14) {  // bytes or string: skipped
      if (end - p < 4) return false;
      size_t l = (read_u32(p) + 3) & ~3u;
      if ((size_t)(end - p) < 4 + l) return false;
      p += 4 + l;
    } else {
      return false;
    }
  }
  return has_amount;
}

// ---------------------------------------------------------------- minimal JSON
static const char *after_key(const char *json, const char *key) {
  char pattern[48];
  int len = snprintf(pattern, sizeof pattern, "\"%s\":", key);
  if (len <= 0 || (size_t)len >= sizeof pattern) return NULL;
  const char *p = strstr(json, pattern);
  if (!p) return NULL;
  p += len;
  while (*p == ' ') p++;
  return p;
}

const char *stl_json_string(const char *json, const char *key, size_t *n) {
  const char *p = after_key(json, key);
  if (!p || *p != '"') return NULL;
  p++;
  const char *end = strchr(p, '"');
  if (!end) return NULL;
  *n = (size_t)(end - p);
  return p;
}

bool stl_json_int(const char *json, const char *key, int64_t *value) {
  const char *p = after_key(json, key);
  if (!p) return false;
  if (*p == '"') p++;
  bool negative = *p == '-';
  if (negative) p++;
  if (*p < '0' || *p > '9') return false;
  int64_t v = 0;
  while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
  *value = negative ? -v : v;
  return true;
}

// ---------------------------------------------------------------- amounts
// "12.5" -> 125000000. No floating point: up to 7 decimals, only digits and one dot.
bool stl_amount_parse(const char *text, int64_t *units) {
  int64_t whole = 0, frac = 0;
  int digits = 0, decimals = 0;
  const char *p = text;
  for (; *p >= '0' && *p <= '9'; p++, digits++) {
    if (whole > 90000000000LL) return false;  // does not fit in 64 bits with 7 decimals
    whole = whole * 10 + (*p - '0');
  }
  if (*p == '.') {
    for (p++; *p >= '0' && *p <= '9'; p++, decimals++) {
      if (decimals >= 7) return false;
      frac = frac * 10 + (*p - '0');
    }
  }
  if (*p || (!digits && !decimals)) return false;
  for (; decimals < 7; decimals++) frac *= 10;
  *units = whole * 10000000 + frac;
  return true;
}

size_t stl_amount_format(int64_t units, char *out, size_t cap) {
  int len = snprintf(out, cap, "%lld.%07lld", (long long)(units / 10000000), (long long)(units % 10000000));
  if (len <= 0 || (size_t)len >= cap) return 0;
  while (out[len - 1] == '0') out[--len] = 0;
  if (out[len - 1] == '.') out[--len] = 0;
  return (size_t)len;
}

// ---------------------------------------------------------------- payment request (SEP-7)
size_t stl_sep7_pay(char *out, size_t cap, const uint8_t destination[32], int64_t amount, const stl_asset *a,
                    bool has_memo, uint64_t memo) {
  char account[57], amount_text[24];
  stl_addr_encode(STL_ADDR_ACCOUNT, destination, account);
  stl_amount_format(amount, amount_text, sizeof amount_text);
  int n = snprintf(out, cap, "web+stellar:pay?destination=%s&amount=%s", account, amount_text);
  if (n <= 0 || (size_t)n >= cap) return 0;
  if (a && !a->native) {
    stl_addr_encode(STL_ADDR_ACCOUNT, a->issuer, account);
    n += snprintf(out + n, cap - (size_t)n, "&asset_code=%s&asset_issuer=%s", a->code, account);
    if ((size_t)n >= cap) return 0;
  }
  if (has_memo) {
    n += snprintf(out + n, cap - (size_t)n, "&memo=%llu&memo_type=MEMO_ID", (unsigned long long)memo);
    if ((size_t)n >= cap) return 0;
  }
  return (size_t)n;
}
