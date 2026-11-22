// Example: a standalone payment device. An ESP32 with a screen, a keypad and a relay that
// charges, sees the payment arrive and pays, talking straight to a Stellar RPC node.
// There is no server of your own, no app and no card: only the microcontroller and the SDK.
//
//   A  Charge   type an amount; the device gives the charge an order number, builds the payment
//               request (SEP-7) and prints it on the serial monitor, and the screen shows a QR with
//               the device's account, the amount and the order number. When the payment arrives
//               the device beeps, lights the LED and pulses the relay
//   B  Pay      type an amount, the device signs and sends a payment (a refund to whoever paid it
//               last, or to PAY_TO)
//   C  Account  the device's account as a QR, and its balance
//   Keypad: digits, D = decimal point, * = erase, # = confirm, A = cancel.
//   The same keys work from the serial monitor (115200 baud), without a keypad.
//
// On testnet the device creates its own account the first time: it makes a key, stores it and asks
// friendbot for test funds. Wiring and parts: see the README next to this folder.
#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Keypad.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <Wire.h>
#include <mbedtls/sha256.h>
#include <qrcode.h>

#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "root_ca.h"
#include "stellar_mcu.h"
#include "stellar_rpc.h"

// ---------------------------------------------------------------- what you have to set
// Wi-Fi: write it here, or create src/wifi_secrets.h (ignored by git) with the same two defines.
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#ifndef WIFI_SSID
#define WIFI_SSID "your-network"
#define WIFI_PASSWORD "your-password"
#endif
// Where "Pay" sends the money when nobody has paid the device yet. It has to exist on testnet.
static const char PAY_TO[] = "GDJBOTBQNWREAVDUUS5XN7EJRRMUFLBNMFEJXSEHUHLYZDFEI2BL747I";

static const char NETWORK[] = "Test SDF Network ; September 2015";
static const uint32_t VALIDITY_S = 90;          // a payment enters the network within this window or never
static const uint32_t CHARGE_WAIT_MS = 180000;  // how long a charge waits to be paid
static const uint32_t RELAY_MS = 1000;          // how long the relay stays on after a payment

// ---------------------------------------------------------------- wiring (same as the README)
#if CONFIG_IDF_TARGET_ESP32S3
static const uint8_t PIN_SDA = 8, PIN_SCL = 9;
static byte ROW_PINS[4] = {4, 5, 6, 7}, COL_PINS[4] = {15, 16, 17, 18};
static const uint8_t PIN_BUZZER = 21, PIN_RELAY = 2, PIN_LED_OK = 41, PIN_LED_ERROR = 42;
#else  // classic ESP32
static const uint8_t PIN_SDA = 21, PIN_SCL = 22;
static byte ROW_PINS[4] = {32, 33, 25, 26}, COL_PINS[4] = {27, 14, 13, 15};
static const uint8_t PIN_BUZZER = 17, PIN_RELAY = 16, PIN_LED_OK = 2, PIN_LED_ERROR = 12;
#endif

static char KEYS[4][4] = {{'1', '2', '3', 'A'}, {'4', '5', '6', 'B'}, {'7', '8', '9', 'C'}, {'*', '0', '#', 'D'}};
static Keypad keypad(makeKeymap(KEYS), ROW_PINS, COL_PINS, 4, 4);
static Adafruit_SSD1306 oled(128, 64, &Wire, -1);
static bool has_screen = false;

// ---------------------------------------------------------------- what the microcontroller provides to the SDK
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

static uint8_t g_seed[32], g_public[32], g_network_id[32], g_contract[32];
static char g_account[57];
static uint8_t g_envelope[1200];
static char g_b64[1700];
static char g_last_event_id[40] = "";  // events up to this one were already counted
static uint8_t g_last_payer[32];       // who paid the device last: "Pay" refunds them
static bool g_has_payer = false;

// ---------------------------------------------------------------- screen, sound and relay
static void show(const char *title, const char *big, const char *small) {
  Serial.printf("[screen] %s | %s | %s\n", title, big, small);
  if (!has_screen) return;
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  oled.setCursor(0, 0);
  oled.print(title);
  oled.setTextSize(2);
  oled.setCursor(0, 22);
  oled.print(big);
  oled.setTextSize(1);
  oled.setCursor(0, 54);
  oled.print(small);
  oled.display();
}

// A QR on the left half and three short lines on the right. An account (56 capital letters and
// digits) fits in a version 3 code, which is the largest one a phone still reads on this screen.
static void show_qr(const char *text, const char *line1, const char *line2, const char *line3) {
  Serial.printf("[screen] QR %s | %s | %s | %s\n", text, line1, line2, line3);
  if (!has_screen) return;
  const uint8_t version = 3, scale = 2;
  QRCode qr;
  uint8_t modules[qrcode_getBufferSize(version)];
  if (qrcode_initText(&qr, modules, version, ECC_LOW, text) < 0) return;
  uint8_t margin = (64 - qr.size * scale) / 2;
  oled.clearDisplay();
  oled.fillRect(0, 0, 64, 64, SSD1306_WHITE);
  for (uint8_t y = 0; y < qr.size; y++) {
    for (uint8_t x = 0; x < qr.size; x++) {
      if (qrcode_getModule(&qr, x, y)) oled.fillRect(margin + x * scale, margin + y * scale, scale, scale, SSD1306_BLACK);
    }
  }
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  const char *lines[] = {line1, line2, line3};
  for (uint8_t i = 0; i < 3; i++) {
    oled.setCursor(68, 8 + i * 20);
    oled.print(lines[i]);
  }
  oled.display();
}

static void beep(uint16_t ms) {
  digitalWrite(PIN_BUZZER, HIGH);
  delay(ms);
  digitalWrite(PIN_BUZZER, LOW);
}

// What happens when the money is in: sound, light and a pulse on the relay.
static void signal_paid() {
  digitalWrite(PIN_LED_OK, HIGH);
  beep(80);
  delay(40);
  beep(120);
  digitalWrite(PIN_RELAY, HIGH);
  Serial.println("Relay ON");
  delay(RELAY_MS);
  digitalWrite(PIN_RELAY, LOW);
  Serial.println("Relay OFF");
  digitalWrite(PIN_LED_OK, LOW);
}

static void signal_error() {
  digitalWrite(PIN_LED_ERROR, HIGH);
  beep(400);
  digitalWrite(PIN_LED_ERROR, LOW);
}

// A key from the keypad or from the serial monitor; 0 if there is none.
static char next_key() {
  char key = keypad.getKey();
  if (key) return key;
  while (Serial.available()) {
    char c = (char)toupper(Serial.read());
    if (isdigit(c) || (c >= 'A' && c <= 'D') || c == '*' || c == '#') return c;
  }
  return 0;
}

// ---------------------------------------------------------------- node queries
// Latest ledger and its close time: that time sets the validity window, with no clock of its own.
static bool network_status(int64_t *ledger, int64_t *close_time) {
  return stellar_rpc::call("getHealth", nullptr) && stl_json_int(stellar_rpc::body(), "latestLedger", ledger) &&
         stl_json_int(stellar_rpc::body(), "latestLedgerCloseTime", close_time);
}

// Balance and sequence number of an account. `exists` is false if the node answered and the
// account is not on the network.
static bool read_account(const uint8_t account[32], int64_t *balance, int64_t *sequence, bool *exists) {
  uint8_t key[40], entry[200];
  char key_b64[64];
  stl_account_key(key, account);
  stl_b64_encode(key, 40, key_b64, sizeof key_b64);
  snprintf(stellar_rpc::params(), stellar_rpc::paramsCap(), "{\"keys\":[\"%s\"]}", key_b64);
  *exists = true;
  if (!stellar_rpc::call("getLedgerEntries", stellar_rpc::params())) return false;
  size_t n = 0;
  const char *xdr = stl_json_string(stellar_rpc::body(), "xdr", &n);
  int bytes = xdr ? stl_b64_decode(xdr, n, entry, sizeof entry) : -1;
  if (bytes <= 0 || !stl_account_parse(entry, (size_t)bytes, balance, sequence)) {
    *exists = false;
    return false;
  }
  return true;
}

// Testnet only: asks friendbot to create the account with test funds.
static bool fund_on_testnet() {
  stellar_rpc::close();  // one secure connection at a time: each one takes about 50 KB of RAM
  WiFiClientSecure client;
  client.setCACert(STELLAR_RPC_ROOT_CA_PEM);
  if (!client.connect("friendbot.stellar.org", 443)) return false;
  client.printf("GET /?addr=%s HTTP/1.1\r\nHost: friendbot.stellar.org\r\nUser-Agent: curl/8.0\r\n"
                "Connection: close\r\n\r\n",
                g_account);
  uint32_t start = millis();
  while (!client.available() && client.connected() && millis() - start < 40000) delay(50);
  String status = client.readStringUntil('\n');  // "HTTP/1.1 200 OK"; 400 = it already existed
  client.stop();
  Serial.printf("friendbot: %s\n", status.c_str());
  return status.indexOf(" 200") > 0 || status.indexOf(" 400") > 0;
}

// An incoming payment found in the network's events.
struct Incoming {
  int64_t amount;
  char tx[65];
  bool by_order;  // the payment carried this charge's order number
};

// Looks in the XLM contract's events for a "transfer" to the device of at least `min_amount`, newer
// than the ones already counted. A payment that carries an order number (a numeric memo) counts
// only if it is `order`; one with no order number counts by its amount. Remembers who paid.
static bool find_incoming(int64_t from_ledger, int64_t min_amount, uint64_t order, Incoming *in) {
  char contract_text[57], topic_transfer[32], topic_me[72];
  uint8_t buf[64];
  stl_addr_encode(STL_ADDR_CONTRACT, g_contract, contract_text);
  size_t n = stl_scval_symbol(buf, sizeof buf, "transfer");
  stl_b64_encode(buf, n, topic_transfer, sizeof topic_transfer);
  n = stl_scval_account(buf, g_public);
  stl_b64_encode(buf, n, topic_me, sizeof topic_me);
  snprintf(stellar_rpc::params(), stellar_rpc::paramsCap(),
           "{\"startLedger\":%lld,\"filters\":[{\"type\":\"contract\",\"contractIds\":[\"%s\"],"
           "\"topics\":[[\"%s\",\"*\",\"%s\",\"**\"]]}],\"pagination\":{\"limit\":5}}",
           (long long)from_ledger, contract_text, topic_transfer, topic_me);
  if (!stellar_rpc::call("getEvents", stellar_rpc::params())) return false;

  // Each event starts with {"type": ; they are checked one by one in a copy.
  const char *p = strstr(stellar_rpc::body(), "{\"type\":");
  while (p) {
    const char *next = strstr(p + 8, "{\"type\":");
    static char event[1400];
    size_t len = next ? (size_t)(next - p) : strlen(p);
    if (len >= sizeof event) len = sizeof event - 1;
    memcpy(event, p, len);
    event[len] = 0;
    p = next;

    char id[40] = "";
    const char *field = stl_json_string(event, "id", &n);
    if (!field || n >= sizeof id) continue;
    memcpy(id, field, n);
    if (strcmp(id, g_last_event_id) <= 0) continue;  // already counted
    strcpy(g_last_event_id, id);                     // counted from now on, whatever its amount

    uint8_t raw[160];
    bool has_memo = false;
    uint64_t memo = 0;
    field = stl_json_string(event, "value", &n);
    int bytes = field ? stl_b64_decode(field, n, raw, sizeof raw) : -1;
    if (bytes <= 0 || !stl_event_amount(raw, (size_t)bytes, &in->amount, &has_memo, &memo)) continue;
    if (has_memo && memo != order) {
      Serial.printf("A payment for order %llu arrived: it is not this charge\n", (unsigned long long)memo);
      continue;
    }
    if (in->amount < min_amount) {
      Serial.println("A payment smaller than the charge arrived: it does not count");
      continue;
    }
    in->by_order = has_memo;
    field = stl_json_string(event, "txHash", &n);
    snprintf(in->tx, sizeof in->tx, "%.*s", field ? (int)n : 0, field ? field : "");

    // Who paid: the second topic is the sender's address.
    g_has_payer = false;
    const char *topics = strstr(event, "\"topic\":[\"");
    const char *second = topics ? strstr(topics + 10, "\",\"") : NULL;
    const char *end = second ? strchr(second + 3, '"') : NULL;
    if (end && stl_b64_decode(second + 3, (size_t)(end - second - 3), raw, sizeof raw) == 44 && raw[3] == 18 &&
        raw[7] == 0) {  // an account (G...), not a contract
      memcpy(g_last_payer, raw + 12, 32);
      g_has_payer = true;
    }
    return true;
  }
  return false;
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

// ---------------------------------------------------------------- the three things the device does
static void home() {
  int64_t balance = 0, sequence = 0;
  bool exists = true;
  char text[24] = "?";
  if (read_account(g_public, &balance, &sequence, &exists)) stl_amount_format(balance, text, sizeof text);
  Serial.printf("BALANCE %s XLM\n", text);
  char line[24];
  snprintf(line, sizeof line, "%.9s XLM", text);
  show("Stellar MCU SDK", line, "A charge B pay C acct");
}

// Lets the user type an amount. Returns false if they cancel.
static bool type_amount(const char *title, int64_t *units, char *text, size_t cap) {
  text[0] = 0;
  show(title, "0", "# ok  * erase  A back");
  for (;;) {
    char key = next_key();
    size_t len = strlen(text);
    if (!key) {
      delay(10);
      continue;
    }
    if (key == 'A') return false;
    if (key == '#') {
      if (len && text[len - 1] == '.') text[--len] = 0;
      if (stl_amount_parse(text, units) && *units > 0) return true;
      continue;
    }
    if (key == '*' && len) text[len - 1] = 0;
    if (isdigit(key) && len + 1 < cap && len < 9) {
      text[len] = key;
      text[len + 1] = 0;
    }
    if (key == 'D' && !strchr(text, '.') && len + 2 < cap) strcat(text, len ? "." : "0.");
    show(title, text[0] ? text : "0", "# ok  * erase  A back");
  }
}

// Charge: show where to pay and watch the network until the payment arrives.
static void charge() {
  int64_t units = 0, ledger = 0, close_time = 0;
  char amount[16];
  if (!type_amount("Charge (XLM)", &units, amount, sizeof amount)) return;
  if (!network_status(&ledger, &close_time)) {
    show("Charge", "No node", "Check the network");
    signal_error();
    delay(2500);
    return;
  }
  // The charge gets an order number, and the SDK builds the payment request (SEP-7) that carries
  // the account, the amount and that number. The request goes out on the serial monitor: as a QR it
  // does not fit on this screen at a size a phone reads, so the screen shows the account as a QR
  // and the amount and the order number as text.
  uint32_t order = 100000 + esp_random() % 900000;
  char request[200], order_text[12];
  size_t request_len = stl_sep7_pay(request, sizeof request, g_public, units, NULL, true, order);
  snprintf(order_text, sizeof order_text, "#%u", (unsigned)order);
  Serial.printf("CHARGE %s XLM to %s order %u from ledger %lld\n", amount, g_account, (unsigned)order,
                (long long)ledger);
  if (request_len) Serial.printf("REQUEST %s\n", request);
  show_qr(g_account, "Pay XLM", amount, order_text);

  uint32_t start = millis(), last_query = 0;
  while (millis() - start < CHARGE_WAIT_MS) {
    char key = next_key();
    if (key == 'A' || key == '*') {
      Serial.println("CHARGE cancelled");
      return;
    }
    if (millis() - last_query < 2000) {
      delay(10);
      continue;
    }
    Incoming in;
    bool found = find_incoming(ledger, units, order, &in);
    last_query = millis();
    if (!found) continue;

    char paid[16], line[24];
    stl_amount_format(in.amount, paid, sizeof paid);
    Serial.printf("PAID %s XLM tx %s %s after %u ms\n", paid, in.tx,
                  in.by_order ? "matched by order number" : "no order number, matched by amount",
                  (unsigned)(millis() - start));
    snprintf(line, sizeof line, "tx %.16s", in.tx);
    show("Paid", paid, line);
    signal_paid();
    uint32_t shown = millis();
    while (millis() - shown < 6000 && !next_key()) delay(20);
    return;
  }
  Serial.println("CHARGE expired: nobody paid");
  show("Charge", "Expired", "Nobody paid");
  signal_error();
  delay(2500);
}

// Pay: the device builds a payment, signs it with its key and sends it.
static void pay() {
  int64_t units = 0, balance = 0, sequence = 0, ledger = 0, close_time = 0;
  bool exists = true;
  char amount[16];
  uint8_t destination[32];
  if (!type_amount("Pay (XLM)", &units, amount, sizeof amount)) return;
  if (g_has_payer) {
    memcpy(destination, g_last_payer, 32);  // a refund to whoever paid last
  } else if (!stl_addr_decode(PAY_TO, STL_ADDR_ACCOUNT, destination)) {
    show("Pay", "No dest.", "Set PAY_TO");
    signal_error();
    delay(2500);
    return;
  }
  char to[57];
  stl_addr_encode(STL_ADDR_ACCOUNT, destination, to);
  show("Pay", amount, "Signing...");
  if (!read_account(g_public, &balance, &sequence, &exists) || !network_status(&ledger, &close_time)) {
    show("Pay", "No node", "Check the network");
    signal_error();
    delay(2500);
    return;
  }

  stl_tx t;
  memset(&t, 0, sizeof t);
  memcpy(t.source, g_public, 32);
  t.fee = 100;
  t.sequence = sequence + 1;
  t.valid_until = (uint64_t)close_time + VALIDITY_S;  // the time comes from the node

  stl_xdr x;
  uint8_t hash[32];
  char hash_hex[65], line[24];
  stl_tx_begin(&x, g_envelope, sizeof g_envelope, g_network_id);
  stl_tx_payment(&x, &t, destination, NULL, units);
  stl_tx_sign(&x, &CRYPTO, g_seed, g_public, hash);
  for (int i = 0; i < 32; i++) sprintf(hash_hex + 2 * i, "%02x", hash[i]);
  Serial.printf("PAY %s XLM to %s tx %s\n", amount, to, hash_hex);
  show("Pay", amount, "Confirming...");

  uint32_t start = millis();
  if (send_and_wait(&x, hash_hex)) {
    Serial.printf("SENT %s XLM tx %s in %u ms\n", amount, hash_hex, (unsigned)(millis() - start));
    snprintf(line, sizeof line, "tx %.16s", hash_hex);
    show("Sent", amount, line);
    digitalWrite(PIN_LED_OK, HIGH);
    beep(120);
    digitalWrite(PIN_LED_OK, LOW);
  } else {
    Serial.println("PAY failed: the network did not confirm it");
    show("Pay", "Failed", "Not confirmed");
    signal_error();
  }
  uint32_t shown = millis();
  while (millis() - shown < 6000 && !next_key()) delay(20);
}

// Account: the device's account as a QR, to send it funds from any wallet.
static void account() {
  int64_t balance = 0, sequence = 0;
  bool exists = true;
  char text[24] = "?";
  if (read_account(g_public, &balance, &sequence, &exists)) stl_amount_format(balance, text, sizeof text);
  Serial.printf("ACCOUNT %s balance %s XLM\n", g_account, text);
  show_qr(g_account, "Balance", text, "XLM");
  uint32_t shown = millis();
  while (millis() - shown < 30000 && !next_key()) delay(20);
}

// ---------------------------------------------------------------- start
static void halt(const char *reason) {
  Serial.printf("Halted: %s\n", reason);
  show("Stopped", "!", reason);
  for (;;) delay(1000);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  pinMode(PIN_RELAY, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_LED_OK, OUTPUT);
  pinMode(PIN_LED_ERROR, OUTPUT);
  Wire.begin(PIN_SDA, PIN_SCL);
  has_screen = oled.begin(SSD1306_SWITCHCAPVCC, 0x3C);
  Serial.println("\nStellar MCU SDK: standalone device");
  show("Stellar MCU SDK", "Wi-Fi", "Connecting...");

  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);  // lower power keeps the 3.3 V rail steady with a screen attached
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) delay(500);
#ifdef WIFI_SSID2  // an optional second network, tried when the first one is not around
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID2, WIFI_PASSWORD2);
    for (int i = 0; i < 30 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  }
#endif
  if (WiFi.status() != WL_CONNECTED) halt("No Wi-Fi");

  // The device's key: created once with the chip's random generator and stored.
  Preferences store;
  store.begin("stellar-mcu", false);
  if (!store.isKey("seed") || store.getBytes("seed", g_seed, 32) != 32) {
    esp_fill_random(g_seed, 32);  // with Wi-Fi on, the chip delivers true randomness
    store.putBytes("seed", g_seed, 32);
  }
  store.end();
  uint8_t copy[32], key[64];
  memcpy(copy, g_seed, 32);  // Monocypher wipes the seed it receives
  crypto_ed25519_key_pair(key, g_public, copy);
  crypto_wipe(key, sizeof key);
  stl_addr_encode(STL_ADDR_ACCOUNT, g_public, g_account);
  Serial.printf("Device account: %s\n", g_account);

  // The network and the token contract (XLM): its events are the ones watched when charging.
  mcu_sha256((const uint8_t *)NETWORK, strlen(NETWORK), g_network_id);
  stl_asset xlm = {true, "", {0}};
  stl_asset_contract(&CRYPTO, g_network_id, &xlm, g_contract);

  // First time on testnet: the account does not exist yet, so the device asks for test funds.
  int64_t balance = 0, sequence = 0;
  bool exists = true;
  for (int attempt = 0; !read_account(g_public, &balance, &sequence, &exists); attempt++) {
    if (attempt >= 12) halt("No account");
    if (!exists && attempt == 0) {
      show("Stellar MCU SDK", "Funding", "Asking friendbot...");
      if (!fund_on_testnet()) Serial.printf("Fund it by hand: https://friendbot.stellar.org/?addr=%s\n", g_account);
    }
    delay(3000);
  }
  beep(60);
  Serial.println("READY");
  home();
}

void loop() {
  char key = next_key();
  if (key == 'A') charge();
  if (key == 'B') pay();
  if (key == 'C') account();
  if (key == 'A' || key == 'B' || key == 'C') home();
  delay(10);
}
