# Stellar MCU SDK

**English** · [Español](README.es.md)

**Stellar payments on any device.** An open library that lets a microcontroller charge, confirm
and sign payments on [Stellar](https://stellar.org), with no server in the middle. Built by
[Paizap](https://github.com/CoKeFish/paizap).

> Status: in development, on Stellar testnet. See [what has been verified](#what-has-been-verified).

## The problem

Stellar has SDKs for the web, for phones and for servers. The one for the physical world is missing.

- **Today a device cannot charge on Stellar by itself.** The official documentation lists SDKs for
  JavaScript, Python, Rust, Java, Go and other application languages, and none for
  microcontrollers ([Stellar Docs](https://developers.stellar.org/docs/tools/sdks/client-sdks)).
- **Whoever tries either writes it by hand or puts a computer in the middle.** The author of an
  ESP8266 candy dispenser says they do not know of any Stellar SDK for Arduino
  ([stellar-candyman](https://github.com/sui77/stellar-candyman)), and SatoshiPay's candy machine
  runs Node.js on a Raspberry Pi
  ([GitHub](https://github.com/satoshipay/stellar-candy-dispenser)).
- **The C library that exists is meant for hardware wallets,** and its README warns that it has not
  been updated in a while ([libstellar](https://github.com/overcat/libstellar)).
- **The little there is reads Horizon,** which the documentation already describes as close to the
  end of its life ([Stellar Docs](https://developers.stellar.org/docs/data/apis)). This SDK works
  on Stellar RPC.

## What it does

| Piece | What the device does |
|---|---|
| **Charge** | Builds the payment request (SEP-7) to show it as a QR code |
| **Confirm** | Asks a Stellar RPC node and checks by itself that the payment arrived, with its amount and its order number |
| **Pay** | Keeps its own key, builds the transaction, signs it and sends it: a classic payment or a call to a token contract (`transfer`, `transfer_from`) |

## What it looks like

A payment, from start to finish, from the board:

```c
stl_tx t = {0};
memcpy(t.source, public_key, 32);
t.fee = 100;
t.sequence = sequence + 1;              // read from the node with getLedgerEntries
t.valid_until = close_time + 120;       // the time comes from the node: no clock needed on board
t.has_memo = true;
t.memo = 4242;                          // the order number

stl_xdr x;
uint8_t hash[32];
stl_tx_begin(&x, envelope, sizeof envelope, network_id);
stl_tx_payment(&x, &t, destination, NULL, 10000000);   // 1 XLM
stl_tx_sign(&x, &CRYPTO, seed, public_key, hash);      // signed on the device
// ...and the envelope goes in base64 to sendTransaction
```

The full example, with the connection to the node and the payment confirmation, is in
[`examples/esp32_pay_and_confirm`](examples/esp32_pay_and_confirm/src/main.cpp). Every function is
described in the [API reference](docs/api.md).

## How it is built

- **A portable C core** ([`src/`](src/)), with no dynamic memory and no dependency on any chip:
  addresses, amounts, base64, XDR, transactions, token contract calls, authorizations, reading
  accounts and payment events, and SEP-7 requests.
- **One adapter per board** ([`platforms/`](platforms/)), which only provides the secure
  connection to the node. The board also provides SHA-256 and the Ed25519 signature through two
  functions.
- **Signing with [Monocypher](https://monocypher.org)** ([`third_party/`](third_party/monocypher/)),
  which takes about 18 KB.

| Folder | Contents |
|---|---|
| `src/` | The core: `stellar_mcu.h` and `stellar_mcu.c` |
| `platforms/esp32-arduino/` | RPC node client over HTTPS for ESP32 with Arduino |
| `third_party/monocypher/` | Monocypher, unmodified |
| `examples/` | ESP32 example: pay and confirm |
| `tests/` | Host tests against the official JavaScript SDK |
| `docs/` | API reference, in English and in Spanish (`docs/es/`) |

## What has been verified

| What | How it was verified |
|---|---|
| The core builds the same bytes as the official JavaScript SDK | 20 of 20 comparisons equal, on a PC, against `@stellar/stellar-sdk` 17.4.0: addresses, token contracts, payments, contract calls, authorizations, event filters and event parsing, SEP-7 and amounts |
| A board pays on its own on testnet | An ESP32-S3 built, signed and sent a [classic payment](https://stellar.expert/explorer/testnet/tx/ca284b2c462693e4b2b6cbf0169b1732d9f165d4fdfa5efa0cf512975c0d0569) and a [transfer on the token contract](https://stellar.expert/explorer/testnet/tx/65894777de6cc74e886a70964700e123918b67fce4538dfe64441eda9fee8e06), and then detected them in the network's events (November 21, 2026) |
| A terminal charges a card and signs by itself | The Paizap terminal made three card charges on testnet with `transfer_from`, built and signed on the board; the last one is [this transaction](https://stellar.expert/explorer/testnet/tx/6f2127ebd08855e3b83446a301e966da843d5f2ae21cae5bb7edba8dffcd3f46) (November 21, 2026) |
| The example builds | For ESP32-S3 and classic ESP32, with PlatformIO |

Measurements on the ESP32-S3:

| What | Measurement |
|---|---|
| SDK core | 3.4 KB of flash, no fixed memory |
| Ed25519 signature | 18 KB of flash |
| Building and signing a payment | 7 milliseconds |
| From sending a payment to seeing it confirmed | 3 to 6 seconds |
| One node query, with the connection open | 0.3 seconds |
| Memory of the secure connection | 49 KB |

## How to try it

**The tests, on a PC.** All you need is a C compiler:

```sh
sh tests/host/run.sh
```

**The example, on an ESP32.** With [PlatformIO](https://platformio.org):

1. Set your Wi-Fi network in `examples/esp32_pay_and_confirm/src/main.cpp`.
2. Flash the board: `pio run -d examples/esp32_pay_and_confirm -e s3 -t upload`
   (or `-e esp32` for a classic ESP32).
3. Open the serial monitor. The board prints its account: give it test funds with
   `https://friendbot.stellar.org/?addr=<that account>`.
4. The board pays 1 XLM, waits for the confirmation and prints the link to the transaction.

**In your own project.** Copy `src/`, your board's adapter and `third_party/monocypher/`, and look
in the example for the two functions your board has to provide (`stl_crypto`). A 1 KB buffer is
enough for a classic payment; for a token contract call, reserve 2 KB.

## Roadmap

From November 23 to December 22, 2026:

1. A usage guide and a second example: a payment notifier.
2. A second board family, without touching the core.
3. Detecting a payment by its order number, for charging with a QR code.
4. Applying to Stellar Instawards.

## License

Apache 2.0. Monocypher keeps its own license (2-clause BSD or CC0), stated in its files.

## Paizap

Paizap builds devices that charge on Stellar. This SDK is the piece it had to write for its first
device, a payment terminal for NFC cards, and it publishes it so that anyone can build the next
one.
