# Ejemplo: aparato autónomo

Un aparato IoT de pagos hecho con un solo microcontrolador: un ESP32 con pantalla, teclado y relé
que cobra, ve llegar el pago y paga en la testnet de Stellar. Habla directo con un nodo RPC: no
hay servidor propio, ni app, ni tarjeta. [English version](README.md).

El relé hace las veces de lo que hace un aparato real cuando le pagan: abrir un torniquete,
activar un dispensador, arrancar una lavadora. El teclado y la pantalla están solo para probarlo
a mano.

## Qué hace

| Tecla | Acción | Qué hace el SDK |
|---|---|---|
| `A` | **Cobrar.** Escribe un monto y `#`. El aparato le da al cobro un número de pedido. La pantalla muestra un QR con la cuenta del aparato, el monto y el número de pedido, y el pedido de pago completo sale por el monitor serie. Cuando llega el pago, el aparato pita, enciende el LED y activa el relé | Arma el pedido de pago (SEP-7) con `stl_sep7_pay`, vigila los eventos del contrato del token con `getEvents` y lee el monto y el número de pedido de cada pago |
| `B` | **Pagar.** Escribe un monto y `#`. El aparato manda ese monto a quien le pagó por última vez (una devolución) o a `PAY_TO` | Arma el pago, lo firma con la llave del aparato, lo envía y espera a la red |
| `C` | **Cuenta.** Muestra la cuenta del aparato como QR y su saldo | Lee la cuenta en el nodo |

En el teclado: dígitos, `D` es el punto decimal, `*` borra, `#` confirma y `A` cancela. Las mismas
teclas sirven desde el monitor serie a 115200 baudios, así que se puede probar sin teclado.

La primera vez, el aparato crea su propia cuenta: hace una llave con el generador aleatorio del
chip, la guarda y le pide fondos de prueba a friendbot. No hay nada que preparar a mano.

## Piezas y conexiones

Un ESP32-S3 o un ESP32 clásico, una OLED SSD1306 (128×64, I2C), un teclado de
membrana 4×4, un buzzer activo, dos LEDs con sus resistencias y un relé de un canal. Sin la
pantalla o sin el teclado, el ejemplo igual corre desde el monitor serie.

| Pieza | ESP32-S3 | ESP32 clásico |
|---|---|---|
| OLED SDA · SCL | 8 · 9 | 21 · 22 |
| Filas del teclado R1 a R4 | 4 · 5 · 6 · 7 | 32 · 33 · 25 · 26 |
| Columnas del teclado C1 a C4 | 15 · 16 · 17 · 18 | 27 · 14 · 13 · 15 |
| Buzzer | 21 | 17 |
| Entrada del relé | 2 | 16 |
| LED verde · LED rojo | 41 · 42 | 2 · 12 |

## Cómo correrlo

1. Pon tu red Wi-Fi al inicio de `src/main.cpp`, o crea `src/wifi_secrets.h` (git lo ignora) con
   `#define WIFI_SSID "..."` y `#define WIFI_PASSWORD "..."`.
2. Graba el firmware: `pio run -d examples/esp32_standalone_device -e s3 -t upload`
   (`-e esp32` para un ESP32 clásico).
3. Espera la pantalla de inicio con el saldo. Aprieta `A`, escribe un monto y `#`, y paga ese
   monto en XLM a la cuenta del QR desde cualquier billetera de testnet. Para atar el pago al
   cobro, pon como memo numérico (memo ID) el número de pedido que muestra la pantalla
   (`#123456`).

## Límites de este ejemplo

- **Solo testnet y XLM.** Otro activo necesita su contrato de token y, para quien paga, una línea
  de confianza.
- **El QR es la cuenta sola.** El aparato arma el pedido de pago completo (SEP-7), con la cuenta,
  el monto y el número de pedido, y lo imprime por el monitor serie como
  `REQUEST web+stellar:pay?...`. Como QR ese pedido no cabe en una pantalla de 128×64 a un tamaño
  que un celular lea, así que la pantalla muestra la cuenta como QR y quien paga escribe el monto
  y el número de pedido. Una pantalla más grande puede mostrar el pedido mismo.
- **El cobro se reconoce por el número de pedido cuando el pago lo trae.** Un pago con memo
  numérico cuenta solo si el memo es el número de pedido del cobro. Un pago sin memo numérico
  sigue contando por su monto: el primero nuevo de al menos el monto escrito cierra el cobro.
- **La llave se guarda sin protección.** Para algo más que una demo, hay que activar el guardado
  cifrado del chip y tener poca plata en la cuenta.
