# Stellar MCU SDK

**English** · [Español](README.es.md)

[![License](https://img.shields.io/badge/license-Apache%202.0-blue)](LICENSE)
![Language](https://img.shields.io/badge/language-C99-blue)
![Microcontrollers](https://img.shields.io/badge/microcontrollers-ESP32%20%7C%20ESP32--S3-blue)
![Network](https://img.shields.io/badge/Stellar-testnet-blue)

**Stellar payments on IoT devices.** `stellar-mcu-sdk` is a C library for microcontrollers, made to
bring payment systems to IoT devices such as turnstiles, dispensers and vending machines. It talks
to [Stellar RPC](https://developers.stellar.org/docs/data/apis/rpc) and builds and signs
transactions on the device itself, with no server of your own in between. Built by
[Paizap](https://github.com/CoKeFish/paizap).

It provides:

- facilities for building and signing transactions on the device: classic payments and calls to
  token contracts (`transfer`, `transfer_from`),
- what a device needs to query a Stellar RPC node and read the answer: accounts, transaction
  status and payment events,
- payment requests (SEP-7) to show as a QR code,
- a network client for Stellar RPC over HTTPS on ESP32.

**Try it:** [live demo](https://paizap.vercel.app/demo/) · [documentation](https://paizap.vercel.app/sdk/)

> Status: in development, on Stellar testnet. See [what has been verified](#what-has-been-verified).

## What it does

| Piece | What the device does |
|---|---|
| **Charge** | Builds the payment request (SEP-7) to show it as a QR code |
| **Confirm** | Asks a Stellar RPC node and checks by itself that the payment arrived, with its amount and its order number, if the payment carries one |
| **Pay** | Keeps its own key, builds the transaction, signs it and sends it: a classic payment or a call to a token contract (`transfer`, `transfer_from`) |

What you can build with those pieces:

- **A device that acts when it is paid,** such as a turnstile, a dispenser or a vending machine:
  it shows the payment request as a QR code and confirms the payment on the node.
- **A payment terminal.** Paizap's demo terminal for NFC cards charges with `transfer_from`, built
  and signed on the device.
- **A payment notifier:** a screen or a buzzer that announces each payment an account receives.
  It holds no key.

## Supported microcontrollers

| Microcontroller | Framework | Status | SHA-256 | Ed25519 |
|---|---|---|---|---|
| ESP32-S3 | Arduino, with PlatformIO | Tested on hardware, on testnet | mbedTLS, included in the ESP32 Arduino core | Monocypher 4.0.2 |
| ESP32 (classic) | Arduino, with PlatformIO | Builds | mbedTLS, included in the ESP32 Arduino core | Monocypher 4.0.2 |

The core ([`src/`](src/)) is portable C99: it allocates no memory and includes nothing from any
chip. A new microcontroller family only needs an adapter in [`platforms/`](platforms/) with the secure connection
to the node, plus two functions: SHA-256 and the Ed25519 signature.

## Installation

Add the SDK to your PlatformIO project as a folder, for example as a Git submodule:

```sh
git submodule add https://github.com/CoKeFish/stellar-mcu-sdk.git
```

and point your `platformio.ini` at it:

```ini
build_flags =
    -Istellar-mcu-sdk/src
    -Istellar-mcu-sdk/platforms/esp32-arduino
    -Istellar-mcu-sdk/third_party/monocypher
build_src_filter =
    +<*>
    +<../stellar-mcu-sdk/src/*.c>
    +<../stellar-mcu-sdk/platforms/esp32-arduino/*.cpp>
    +<../stellar-mcu-sdk/third_party/monocypher/*.c>
```

Then include it:

```c
#include "stellar_mcu.h"   // the core
#include "stellar_rpc.h"   // the ESP32 client for Stellar RPC
```

With another build system, compile `src/stellar_mcu.c`, your microcontroller's adapter and the two
Monocypher files, and add those three folders to the include path.

## Quick examples

`CRYPTO` holds the two functions your microcontroller provides (`stl_crypto`); the
[full example](examples/esp32_pay_and_confirm/src/main.cpp) shows them for ESP32.

### Send a payment

```c
stl_tx t = {0};
memcpy(t.source, public_key, 32);
t.fee = 100;
t.sequence = sequence + 1;         // read from the node with getLedgerEntries
t.valid_until = close_time + 120;  // the time comes from the node: no clock needed on the device
t.has_memo = true;
t.memo = 4242;                     // your order number

stl_xdr x;
uint8_t hash[32];
stl_tx_begin(&x, envelope, sizeof envelope, network_id);
stl_tx_payment(&x, &t, destination, NULL, 10000000);  // 1 XLM
stl_tx_sign(&x, &CRYPTO, seed, public_key, hash);     // signed on the device

char b64[1500];
stl_b64_encode(stl_tx_envelope(&x), stl_tx_envelope_len(&x), b64, sizeof b64);
// ...and b64 goes to sendTransaction
```

### Request a payment

```c
stl_asset usdc = {false, "USDC", {0}};
stl_addr_decode("GBBD47IF6LWK7P7MDEVSCWR7DPUWV3NY3DTQEVFL4NAT4AQH3ZLLFLA5", STL_ADDR_ACCOUNT, usdc.issuer);

char uri[256];
stl_sep7_pay(uri, sizeof uri, merchant, 25000000, &usdc, true, 4242);  // 2.5 USDC, order 4242
// web+stellar:pay?destination=G...&amount=2.5&asset_code=USDC&asset_issuer=G...&memo=4242&memo_type=MEMO_ID
```

### Confirm that a payment arrived

```c
// Topics for getEvents: "transfer" events of the token contract that go to your account
uint8_t buf[64];
char topic_transfer[32], topic_to[72];
size_t n = stl_scval_symbol(buf, sizeof buf, "transfer");
stl_b64_encode(buf, n, topic_transfer, sizeof topic_transfer);
n = stl_scval_account(buf, merchant);
stl_b64_encode(buf, n, topic_to, sizeof topic_to);

// ...and for each event the node returns, decode its "value" from base64 and read it:
int64_t amount;
bool has_memo;
uint64_t order;
if (stl_event_amount(value, value_len, &amount, &has_memo, &order)) {
  // paid: `amount` in stroops and, if has_memo, the order number
}
```

### Call a token contract

```c
stl_token_call call = {token_contract, "transfer_from", {spender, customer, merchant}, 3, 25000000};

stl_tx_begin(&x, envelope, sizeof envelope, network_id);
stl_tx_token_call(&x, &t, &call, NULL, 0);  // 1. build it for simulateTransaction
stl_tx_unsigned(&x);
// ...the node answers with transactionData and minResourceFee

t.fee = 100 + (uint32_t)min_resource_fee;
stl_tx_begin(&x, envelope, sizeof envelope, network_id);
stl_tx_token_call(&x, &t, &call, soroban_data, soroban_data_len);  // 2. build it again with that data
stl_tx_sign(&x, &CRYPTO, seed, public_key, hash);                  //    and sign it on the device
```

## Documentation

| Guide | Description |
|---|---|
| [API reference](docs/api.md) | Every function of the core and of the ESP32 client |
| [Example: pay and confirm](examples/esp32_pay_and_confirm/src/main.cpp) | A complete program: the device creates its key, pays 1 XLM, waits for the confirmation and finds the payment in the network's events |
| [Example: standalone device](examples/esp32_standalone_device/) | An IoT payment device on one microcontroller, with a screen, a keypad and a relay: it charges with a QR code and waits for the payment, pays, and shows its account and balance |
| [Referencia de la API](docs/es/api.md) | The API reference, in Spanish |

What is in the repository:

| Folder | Contents |
|---|---|
| `src/` | The core: `stellar_mcu.h` and `stellar_mcu.c` |
| `platforms/esp32-arduino/` | Stellar RPC client over HTTPS for ESP32 with Arduino |
| `third_party/monocypher/` | [Monocypher](https://monocypher.org), unmodified |
| `examples/` | ESP32 examples: pay and confirm, and a standalone device |
| `tests/` | Host tests against the official JavaScript SDK |
| `docs/` | API reference, in English and in Spanish (`docs/es/`) |

## Compatibility

**Stellar RPC.** The SDK works on Stellar RPC, not on Horizon. These are the methods the core has
helpers for:

| RPC method | What the SDK gives you |
|---|---|
| `getHealth` | Reads the latest ledger and its close time, which sets the expiration of a transaction without a clock on the device |
| `getLedgerEntries` | Builds the key of an account (`stl_account_key`) and reads its balance and sequence number (`stl_account_parse`) |
| `simulateTransaction` | Builds the token contract call to simulate, and builds it again with the data the node returns (`stl_tx_token_call`) |
| `sendTransaction` | The signed envelope, ready to send in base64 (`stl_tx_envelope`) |
| `getTransaction` | Reads the status of the transaction (`stl_json_string`) |
| `getEvents` | Builds the topics to filter by (`stl_scval_symbol`, `stl_scval_account`) and reads the amount and the memo of each payment (`stl_event_amount`) |

`stellar_rpc::call` takes the method name, so any other method is called the same way.

**SEPs.**

| SEP | What is supported |
|---|---|
| [SEP-7](https://github.com/stellar/stellar-protocol/blob/master/ecosystem/sep-0007.md) | `pay` requests with destination, amount, asset and numeric memo |
| [SEP-23](https://github.com/stellar/stellar-protocol/blob/master/ecosystem/sep-0023.md) | Account (`G...`), seed (`S...`) and contract (`C...`) addresses, encoded and decoded with their checksum |
| [SEP-41](https://github.com/stellar/stellar-protocol/blob/master/ecosystem/sep-0041.md) | Calls to `transfer` and `transfer_from`, and reading `transfer` events in both formats: the amount alone, or the map with the amount and `to_muxed_id` |

**Protocol.** The core builds the same bytes as `@stellar/stellar-sdk` 17.4.0, and its
transactions were accepted on testnet running protocol 30 (November 21, 2026).

## What has been verified

| What | How it was verified |
|---|---|
| The core builds the same bytes as the official JavaScript SDK | 23 checks on a PC, all passing. 16 compare byte for byte with `@stellar/stellar-sdk` 17.4.0: addresses, token contracts, payments, `transfer` and `transfer_from` calls, authorizations and event filters. The other 7 cover reading addresses and events, the SEP-7 text and amounts |
| A microcontroller pays on its own on testnet | An ESP32-S3 built, signed and sent a [classic payment](https://stellar.expert/explorer/testnet/tx/ca284b2c462693e4b2b6cbf0169b1732d9f165d4fdfa5efa0cf512975c0d0569) and a [transfer on the token contract](https://stellar.expert/explorer/testnet/tx/65894777de6cc74e886a70964700e123918b67fce4538dfe64441eda9fee8e06), and then detected them in the network's events (November 21, 2026) |
| A terminal charges a card and signs by itself | Paizap's demo terminal charges NFC cards on testnet with `transfer_from`, built and signed on the device: [this transaction](https://stellar.expert/explorer/testnet/tx/dab4fb6bd5882fc1ebc62faf4d17848e1db2ba4c8905a9ebdfbb8fab1b2cd09b) is one of those charges, and its only signature is the device's (November 22, 2026) |
| The examples build | For ESP32-S3 and classic ESP32, with PlatformIO |

Measurements on the ESP32-S3:

| What | Measurement |
|---|---|
| SDK core | 3.4 KB of flash, no fixed memory |
| Ed25519 signature | 18 KB of flash |
| Building and signing a payment | 7 milliseconds |
| From sending a payment to seeing it confirmed | 3 to 6 seconds |
| One node query, with the connection open | 0.3 seconds |
| Memory of the secure connection | 49 KB |

A 1 KB buffer is enough for a classic payment; for a token contract call, reserve 2 KB.

## Testing

**The tests, on a PC.** All you need is a C compiler:

```sh
sh tests/host/run.sh
```

They build addresses, payments, contract calls, authorizations and event filters with the core
and compare each result with what the official JavaScript SDK produces
([`tests/vectors/vectors.mjs`](tests/vectors/vectors.mjs) generates the reference values).

**The example, on an ESP32.** With [PlatformIO](https://platformio.org):

1. Set your Wi-Fi network in `examples/esp32_pay_and_confirm/src/main.cpp`.
2. Flash the firmware: `pio run -d examples/esp32_pay_and_confirm -e s3 -t upload`
   (or `-e esp32` for a classic ESP32).
3. Open the serial monitor. The device prints its account: give it test funds with
   `https://friendbot.stellar.org/?addr=<that account>`.
4. The device pays 1 XLM, waits for the confirmation and prints the link to the transaction.

## Why this SDK exists

Stellar has SDKs for the web, for phones and for servers. The one for the physical world was
missing.

- **The official list has no SDK for microcontrollers.** The documentation lists client SDKs for
  JavaScript, Python, Rust, iOS, Flutter, PHP, Kotlin Multiplatform, Java, Go and C# .NET
  ([Stellar Docs](https://developers.stellar.org/docs/tools/sdks/client-sdks)).
- **Whoever tried either wrote it by hand or put a computer in the middle.** The author of an
  ESP8266 candy dispenser wrote: "Because I'm not aware of any existing stellar arduino sdk, I've
  implemented consuming the horizon payment stream http endpoint on my own"
  ([stellar-candyman](https://github.com/sui77/stellar-candyman)). SatoshiPay's candy machine
  runs Node.js on a Raspberry Pi
  ([GitHub](https://github.com/satoshipay/stellar-candy-dispenser)).
- **The C library that exists is meant for hardware wallets,** and its README warns that it "has
  not been updated for some time" ([libstellar](https://github.com/overcat/libstellar)).
- **The little there is reads Horizon.** The documentation says that Horizon "is nearing
  end-of-life" and that RPC "is the recommended API for accessing and interacting with Stellar
  network data in real-time" ([Stellar Docs](https://developers.stellar.org/docs/data/apis)).
  This SDK works on Stellar RPC.

## Roadmap

From November 23 to December 22, 2026:

| Week | What | Goal |
|---|---|---|
| 1 (Nov 23 to 29) | Make the library ready for a third party: a getting-started guide and the two examples | Someone outside the team builds an example and pays on testnet with the guide alone |
| 2 (Nov 30 to Dec 6) | Bring the same core to a second microcontroller, the RP2040 with its Wi-Fi chip. Start the Stellar Instawards application, through the local ambassador chapter | The same payment example runs on 2 microcontrollers |
| 3 (Dec 7 to 13) | A new example: charging with an order number. The library already reads that number from the payment; the example waits for someone else's payment and recognizes it by that number | Example published and tested on testnet |
| 4 (Dec 14 to 22) | Submit the Stellar Instawards application and share the library with Stellar developers | Application submitted; 3 outside developers build it and leave feedback in this repository |

The month costs the time of the two of us and the hardware to test the second microcontroller: a
board with an RP2040 and Wi-Fi costs [$6](https://www.raspberrypi.com/news/raspberry-pi-pico-w-your-6-iot-platform/).
The first thing we check is that someone outside the team can use the library without our help.
What comes next is funded with [Stellar Instawards](https://stellar.org/blog/ecosystem/introducing-the-stellar-ambassador-program):
awards of up to $15K for early experimentation and prototyping, offered through the Stellar
ambassador chapters.

## Feedback and contributing

- [Report a bug or ask for a microcontroller](https://github.com/CoKeFish/stellar-mcu-sdk/issues).
- To add a microcontroller family, add a folder under `platforms/` with its connection to the node. The core does
  not change.
- Run `sh tests/host/run.sh` before sending a pull request.

## License

Apache 2.0. See [LICENSE](LICENSE). Monocypher keeps its own license (2-clause BSD or CC0), stated
in its files.

## Paizap

Paizap brings Stellar payments to IoT devices: turnstiles, dispensers, vending machines. It
maintains this SDK and publishes it so that anyone can build with it. Its demo, a payment terminal
for NFC cards, runs on this SDK.

### Team

| Who | Role | What they bring |
|---|---|---|
| **Rodion Romanovich** ([rodion.com.co](https://rodion.com.co), [@CoKeFish](https://github.com/CoKeFish)) | Hardware and Stellar | Electronics engineer: electronics engineering and blockchain integration for microcontrollers. His previous project on Stellar is [Kiba](https://github.com/CoKeFish/kiba), pay-per-call payments in USDC, built for the Stellar PULSO Hackathon. Before that he built [Mesura](https://github.com/CoKeFish/mesura-firmware), IoT sensors on Arduino, ESP32 and Raspberry Pi |
| **Clyn Roje Quispe Villegas** | Business and product | Business development and product focus |
