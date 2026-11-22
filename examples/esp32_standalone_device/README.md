# Example: standalone device

An IoT payment device built around one microcontroller: an ESP32 with a screen, a keypad and a
relay that charges, sees the payment arrive and pays on the Stellar testnet. It talks straight to
an RPC node: there is no server of your own, no app and no card.
[Versión en español](README.es.md).

The relay stands in for what a real device does once it is paid: open a turnstile, run a
dispenser, start a washing machine. The keypad and the screen are only there to try it by hand.

## What it does

| Key | Action | What the SDK does |
|---|---|---|
| `A` | **Charge.** Type an amount and `#`. The screen shows a QR with the device's account. When the payment arrives, the device beeps, lights the LED and pulses the relay | Watches the token contract's events with `getEvents` and reads the amount |
| `B` | **Pay.** Type an amount and `#`. The device sends that amount to whoever paid it last (a refund) or to `PAY_TO` | Builds the payment, signs it with the device's key, sends it and waits for the network |
| `C` | **Account.** Shows the device's account as a QR and its balance | Reads the account from the node |

On the keypad: digits, `D` is the decimal point, `*` erases, `#` confirms and `A` cancels. The same
keys work from the serial monitor at 115200 baud, so you can try it without a keypad.

The first time, the device creates its own account: it makes a key with the chip's random
generator, stores it and asks friendbot for test funds. Nothing to set up by hand.

## Parts and wiring

An ESP32-S3 or a classic ESP32, an SSD1306 OLED (128×64, I2C), a 4×4 membrane
keypad, an active buzzer, two LEDs with their resistors and a one-channel relay module. Without
the screen or the keypad the example still runs from the serial monitor.

| Part | ESP32-S3 | Classic ESP32 |
|---|---|---|
| OLED SDA · SCL | 8 · 9 | 21 · 22 |
| Keypad rows R1 to R4 | 4 · 5 · 6 · 7 | 32 · 33 · 25 · 26 |
| Keypad columns C1 to C4 | 15 · 16 · 17 · 18 | 27 · 14 · 13 · 15 |
| Buzzer | 21 | 17 |
| Relay input | 2 | 16 |
| Green LED · red LED | 41 · 42 | 2 · 12 |

## Run it

1. Set your Wi-Fi network at the top of `src/main.cpp`, or create `src/wifi_secrets.h` (ignored
   by git) with `#define WIFI_SSID "..."` and `#define WIFI_PASSWORD "..."`.
2. Flash the firmware: `pio run -d examples/esp32_standalone_device -e s3 -t upload`
   (`-e esp32` for a classic ESP32).
3. Wait for the home screen with the balance. Press `A`, type an amount and `#`, and pay that
   amount in XLM to the account in the QR from any testnet wallet.

## Limits of this example

- **Testnet and XLM only.** Another asset needs its token contract and, for the payer, a trustline.
- **The QR is the bare account.** A full payment request (SEP-7) does not fit on a 128×64 screen
  at a size a phone can read, so the payer types the amount.
- **A charge matches by amount.** The first new payment of at least the amount typed closes the
  charge. With several customers at once, give each charge an order number (a memo) instead.
- **The key is stored without protection.** For anything beyond a demo, turn on the chip's
  encrypted storage and keep little money in the account.
