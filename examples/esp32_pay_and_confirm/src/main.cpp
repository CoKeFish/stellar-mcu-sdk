// Example: an ESP32 pays on the Stellar testnet and checks by itself that the payment arrived.
//
// What it does, with no server of your own in the middle:
//   1. Creates its key the first time and stores it. Prints its account so you can fund it.
//   2. Builds a 1 XLM payment with an order number (memo), signs it and sends it to an RPC node.
//   3. Waits for the network to confirm it.
//   4. Looks for the payment in the network's events and reads the amount and the order number.
//
// To use it: set your Wi-Fi network and a destination account below, flash the firmware, open the
// serial monitor and give test funds to the account it prints:
//   https://friendbot.stellar.org/?addr=<the account the device prints>
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>

#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "stellar_mcu.h"
#include "stellar_rpc.h"

// ---------------------------------------------------------------- what you have to set
static const char WIFI_SSID[] = "your-network";
static const char WIFI_PASSWORD[] = "your-password";
// Account that receives the payment: it has to exist on testnet. This one is a test account that
// only receives; replace it with yours.
static const char DESTINATION[] = "GDJBOTBQNWREAVDUUS5XN7EJRRMUFLBNMFEJXSEHUHLYZDFEI2BL747I";

static const char NETWORK[] = "Test SDF Network ; September 2015";
static const int64_t AMOUNT = 10000000;     // 1 XLM, in smallest units (7 decimals)
static const uint64_t ORDER_NUMBER = 4242;  // travels as the memo and comes back in the event
static const uint32_t VALIDITY_S = 120;     // the payment enters the network within this window or never

// ---------------------------------------------------------------- what the microcontroller provides
static void mcu_sha256(const uint8_t *data, size_t n, uint8_t out[32]) { mbedtls_sha256_ret(data, n, out, 0); }

static void mcu_sign(const uint8_t seed[32], const uint8_t public_key[32], const uint8_t *message, size_t n,
                       uint8_t signature[64]) {
  uint8_t key[64];  // Monocypher keeps the secret key as seed + public key
  memcpy(key, seed, 32);
  memcpy(key + 32, public_key, 32);
  crypto_ed25519_sign(signature, key, message, n);
  crypto_wipe(key, sizeof key);
}

static const stl_crypto CRYPTO = {mcu_sha256, mcu_sign};

static uint8_t g_seed[32], g_public[32], g_destination[32], g_network_id[32], g_contract[32];
static uint8_t g_envelope[1200];
static char g_b64[1700];

// ---------------------------------------------------------------- node queries
// Latest ledger and its close time: that time sets the validity window, with no clock of its own.
static bool network_status(int64_t *ledger, int64_t *close_time) {
  return stellar_rpc::call("getHealth", nullptr) && stl_json_int(stellar_rpc::body(), "latestLedger", ledger) &&
         stl_json_int(stellar_rpc::body(), "latestLedgerCloseTime", close_time);
}

// Balance and sequence number of an account. false if the account does not exist on the network yet.
static bool read_account(const uint8_t account[32], int64_t *balance, int64_t *sequence) {
  uint8_t key[40], entry[200];
  char key_b64[64];
  stl_account_key(key, account);
  stl_b64_encode(key, 40, key_b64, sizeof key_b64);
  snprintf(stellar_rpc::params(), stellar_rpc::paramsCap(), "{\"keys\":[\"%s\"]}", key_b64);
  if (!stellar_rpc::call("getLedgerEntries", stellar_rpc::params())) return false;
  size_t n = 0;
  const char *xdr = stl_json_string(stellar_rpc::body(), "xdr", &n);
  if (!xdr) return false;
  int bytes = stl_b64_decode(xdr, n, entry, sizeof entry);
  return bytes > 0 && stl_account_parse(entry, (size_t)bytes, balance, sequence);
}

// Sends the signed envelope and waits up to 30 seconds for the network to confirm it.
static bool send_and_wait(const stl_xdr *x, const char *hash_hex) {
  if (x->overflow || !stl_b64_encode(stl_tx_envelope(x), stl_tx_envelope_len(x), g_b64, sizeof g_b64)) return false;
  snprintf(stellar_rpc::params(), stellar_rpc::paramsCap(), "{\"transaction\":\"%s\"}", g_b64);
  if (!stellar_rpc::call("sendTransaction", stellar_rpc::params())) return false;
  size_t n = 0;
  const char *status = stl_json_string(stellar_rpc::body(), "status", &n);
  if (!status || strncmp(status, "PENDING", 7)) {
    Serial.printf("The network did not accept the submission: %.400s\n", stellar_rpc::body());
    return false;
  }
  for (int query = 0; query < 30; query++) {
    delay(1000);
    snprintf(stellar_rpc::params(), stellar_rpc::paramsCap(), "{\"hash\":\"%s\"}", hash_hex);
    if (!stellar_rpc::call("getTransaction", stellar_rpc::params())) continue;
    status = stl_json_string(stellar_rpc::body(), "status", &n);
    if (status && !strncmp(status, "SUCCESS", 7)) return true;
    if (status && !strncmp(status, "FAILED", 6)) return false;
  }
  return false;
}

// Looks in the token contract's events for the "transfer" to `g_destination` that belongs to the
// transaction, and reads its amount and its order number.
static bool find_payment(int64_t from_ledger, const char *hash_hex, int64_t *amount, uint64_t *order) {
  char contract_text[57], topic_transfer[32], topic_destination[72];
  uint8_t buf[64];
  stl_addr_encode(STL_ADDR_CONTRACT, g_contract, contract_text);
  size_t n = stl_scval_symbol(buf, sizeof buf, "transfer");
  stl_b64_encode(buf, n, topic_transfer, sizeof topic_transfer);
  n = stl_scval_account(buf, g_destination);
  stl_b64_encode(buf, n, topic_destination, sizeof topic_destination);

  for (int attempt = 0; attempt < 12; attempt++) {
    snprintf(stellar_rpc::params(), stellar_rpc::paramsCap(),
             "{\"startLedger\":%lld,\"filters\":[{\"type\":\"contract\",\"contractIds\":[\"%s\"],"
             "\"topics\":[[\"%s\",\"*\",\"%s\",\"**\"]]}],\"pagination\":{\"limit\":10}}",
             (long long)from_ledger, contract_text, topic_transfer, topic_destination);
    if (stellar_rpc::call("getEvents", stellar_rpc::params())) {
      // Each event starts with {"type": ; they are checked one by one in a copy.
      const char *p = strstr(stellar_rpc::body(), "{\"type\":");
      while (p) {
        const char *next = strstr(p + 8, "{\"type\":");
        static char event[1200];
        size_t len = next ? (size_t)(next - p) : strlen(p);
        if (len >= sizeof event) len = sizeof event - 1;
        memcpy(event, p, len);
        event[len] = 0;
        if (strstr(event, hash_hex)) {
          size_t l = 0;
          const char *value = stl_json_string(event, "value", &l);
          uint8_t raw[160];
          bool has_memo = false;
          int bytes = value ? stl_b64_decode(value, l, raw, sizeof raw) : -1;
          *order = 0;
          return bytes > 0 && stl_event_amount(raw, (size_t)bytes, amount, &has_memo, order);
        }
        p = next;
      }
    }
    delay(1500);
  }
  return false;
}

// ---------------------------------------------------------------- the example
static void halt(const char *reason) {
  Serial.printf("Halted: %s\n", reason);
  for (;;) delay(1000);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\nStellar MCU SDK: pay and confirm from the device");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  if (WiFi.status() != WL_CONNECTED) halt("no Wi-Fi: check WIFI_SSID and WIFI_PASSWORD");

  // The device's key: created once with the chip's random generator and stored.
  Preferences store;
  store.begin("stellar-mcu", false);
  if (store.getBytes("seed", g_seed, 32) != 32) {
    esp_fill_random(g_seed, 32);  // with Wi-Fi on, the chip delivers true randomness
    store.putBytes("seed", g_seed, 32);
  }
  store.end();
  uint8_t copy[32], key[64];
  memcpy(copy, g_seed, 32);  // Monocypher wipes the seed it receives
  crypto_ed25519_key_pair(key, g_public, copy);
  crypto_wipe(key, sizeof key);

  char account[57];
  stl_addr_encode(STL_ADDR_ACCOUNT, g_public, account);
  Serial.printf("Device account: %s\n", account);
  if (!stl_addr_decode(DESTINATION, STL_ADDR_ACCOUNT, g_destination)) halt("DESTINATION is not a valid account");

  // The network and the token contract (XLM): its events are the ones queried when confirming.
  mcu_sha256((const uint8_t *)NETWORK, strlen(NETWORK), g_network_id);
  stl_asset xlm = {true, "", {0}};
  stl_asset_contract(&CRYPTO, g_network_id, &xlm, g_contract);

  int64_t balance = 0, sequence = 0, ledger = 0, close_time = 0;
  for (int i = 0; !read_account(g_public, &balance, &sequence); i++) {
    if (i == 0) Serial.printf("Give it test funds: https://friendbot.stellar.org/?addr=%s\n", account);
    delay(5000);
  }
  char text[32];
  stl_amount_format(balance, text, sizeof text);
  Serial.printf("Balance: %s XLM\n", text);
  if (!network_status(&ledger, &close_time)) halt("the node is not responding");

  // 1. Build and sign the payment.
  stl_tx t;
  memset(&t, 0, sizeof t);
  memcpy(t.source, g_public, 32);
  t.fee = 100;
  t.sequence = sequence + 1;
  t.valid_until = (uint64_t)close_time + VALIDITY_S;  // the time comes from the node
  t.has_memo = true;
  t.memo = ORDER_NUMBER;

  stl_xdr x;
  uint8_t hash[32];
  char hash_hex[65];
  uint32_t start = micros();
  stl_tx_begin(&x, g_envelope, sizeof g_envelope, g_network_id);
  stl_tx_payment(&x, &t, g_destination, NULL, AMOUNT);
  stl_tx_sign(&x, &CRYPTO, g_seed, g_public, hash);
  Serial.printf("Payment built and signed in %u microseconds\n", (unsigned)(micros() - start));
  for (int i = 0; i < 32; i++) sprintf(hash_hex + 2 * i, "%02x", hash[i]);

  // 2. Send it and wait for the network.
  start = millis();
  if (!send_and_wait(&x, hash_hex)) halt("the network did not confirm the payment");
  Serial.printf("Payment confirmed in %u ms\n", (unsigned)(millis() - start));
  Serial.printf("https://stellar.expert/explorer/testnet/tx/%s\n", hash_hex);

  // 3. Check it in the network's events, as a device that charges would.
  int64_t amount = 0;
  uint64_t order = 0;
  if (find_payment(ledger, hash_hex, &amount, &order)) {
    stl_amount_format(amount, text, sizeof text);
    Serial.printf("Payment seen on the network: %s XLM, order %llu\n", text, (unsigned long long)order);
  } else {
    Serial.println("The payment was confirmed, but it did not show up in this node's events.");
  }
  stellar_rpc::close();
}

void loop() { delay(1000); }
