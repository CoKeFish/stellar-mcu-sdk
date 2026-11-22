# Stellar MCU SDK

[English](README.md) · **Español**

**Pagos de Stellar en cualquier aparato.** Una librería abierta para que un microcontrolador cobre,
confirme y firme pagos en [Stellar](https://stellar.org), sin un servidor en el medio. Hecha por
[Paizap](https://github.com/CoKeFish/paizap).

> Estado: en desarrollo, sobre Stellar testnet. Mira [qué está comprobado](#qué-está-comprobado).

## El problema

Stellar tiene SDK para la web, el celular y los servidores. Falta el del mundo físico.

- **Hoy un aparato no puede cobrar en Stellar por sí mismo.** La documentación oficial lista SDK
  para JavaScript, Python, Rust, Java, Go y otros lenguajes de aplicaciones, y ninguno para
  microcontroladores ([Stellar Docs](https://developers.stellar.org/docs/tools/sdks/client-sdks)).
- **Quien lo intenta lo escribe a mano o pone una computadora en el medio.** El autor de un
  dispensador de dulces con ESP8266 cuenta que no conoce ningún SDK de Stellar para Arduino
  ([stellar-candyman](https://github.com/sui77/stellar-candyman)), y la máquina de dulces de
  SatoshiPay corre Node.js en una Raspberry Pi
  ([GitHub](https://github.com/satoshipay/stellar-candy-dispenser)).
- **La librería en C que existe es para billeteras físicas** y su README avisa que lleva tiempo sin
  actualizarse ([libstellar](https://github.com/overcat/libstellar)).
- **Lo poco que hay lee Horizon,** que la documentación ya da por cercano al fin de su vida útil
  ([Stellar Docs](https://developers.stellar.org/docs/data/apis)). Este SDK trabaja sobre
  Stellar RPC.

## Qué hace

| Pieza | Qué hace el aparato |
|---|---|
| **Cobrar** | Arma el pedido de pago (SEP-7) para mostrarlo como QR |
| **Confirmar** | Le pregunta a un nodo de Stellar RPC y comprueba él mismo que el pago llegó, con su monto y su número de pedido |
| **Pagar** | Guarda su llave, arma la transacción, la firma y la envía: un pago clásico o una llamada a un contrato de token (`transfer`, `transfer_from`) |

## Cómo se ve

Un pago, de principio a fin, desde la placa:

```c
stl_tx t = {0};
memcpy(t.source, public_key, 32);
t.fee = 100;
t.sequence = sequence + 1;              // leída del nodo con getLedgerEntries
t.valid_until = close_time + 120;       // la hora la da el nodo: no hace falta reloj propio
t.has_memo = true;
t.memo = 4242;                          // el número de pedido

stl_xdr x;
uint8_t hash[32];
stl_tx_begin(&x, envelope, sizeof envelope, network_id);
stl_tx_payment(&x, &t, destination, NULL, 10000000);   // 1 XLM
stl_tx_sign(&x, &CRYPTO, seed, public_key, hash);      // firma en el aparato
// ...y el sobre va en base64 a sendTransaction
```

El ejemplo completo, con la conexión al nodo y la confirmación del pago, está en
[`examples/esp32_pay_and_confirm`](examples/esp32_pay_and_confirm/src/main.cpp). Cada función
está descrita en la [referencia de la API](docs/es/api.md). El código y sus comentarios están en
inglés.

## Cómo está hecho

- **Un núcleo en C portable** ([`src/`](src/)), sin memoria dinámica y sin depender de ningún chip:
  direcciones, montos, base64, XDR, transacciones, llamadas a contratos de token, autorizaciones,
  lectura de cuentas y de eventos de pago, y pedidos SEP-7.
- **Un adaptador por placa** ([`platforms/`](platforms/)), que solo aporta la conexión segura al
  nodo. La placa también aporta el SHA-256 y la firma Ed25519 a través de dos funciones.
- **Firma con [Monocypher](https://monocypher.org)** ([`third_party/`](third_party/monocypher/)),
  que ocupa unos 18 KB.

| Carpeta | Contenido |
|---|---|
| `src/` | El núcleo: `stellar_mcu.h` y `stellar_mcu.c` |
| `platforms/esp32-arduino/` | Cliente del nodo RPC por HTTPS para ESP32 con Arduino |
| `third_party/monocypher/` | Monocypher, sin cambios |
| `examples/` | Ejemplo para ESP32: pagar y confirmar |
| `tests/` | Pruebas en el PC contra el SDK oficial de JavaScript |
| `docs/` | Referencia de la API, en inglés y en español (`docs/es/`) |

## Qué está comprobado

| Qué | Cómo se comprobó |
|---|---|
| El núcleo arma lo mismo que el SDK oficial de JavaScript, byte a byte | 20 de 20 comparaciones iguales, en el PC, contra `@stellar/stellar-sdk` 17.4.0: direcciones, contratos de token, pagos, llamadas a contratos, autorizaciones, filtros y lectura de eventos, SEP-7 y montos |
| Una placa paga sola en testnet | Una ESP32-S3 armó, firmó y envió un [pago clásico](https://stellar.expert/explorer/testnet/tx/ca284b2c462693e4b2b6cbf0169b1732d9f165d4fdfa5efa0cf512975c0d0569) y una [transferencia en el contrato del token](https://stellar.expert/explorer/testnet/tx/65894777de6cc74e886a70964700e123918b67fce4538dfe64441eda9fee8e06), y después los detectó en los eventos de la red (21 de noviembre de 2026) |
| Un terminal cobra con tarjeta firmando él mismo | El terminal de Paizap hizo tres cobros con tarjeta en testnet con `transfer_from`, armados y firmados en la placa; el último es [esta transacción](https://stellar.expert/explorer/testnet/tx/6f2127ebd08855e3b83446a301e966da843d5f2ae21cae5bb7edba8dffcd3f46) (21 de noviembre de 2026) |
| El ejemplo compila | Para ESP32-S3 y ESP32 clásico, con PlatformIO |

Medidas en la ESP32-S3:

| Qué | Medida |
|---|---|
| Núcleo del SDK | 3,4 KB de flash, sin memoria fija |
| Firma Ed25519 | 18 KB de flash |
| Armar y firmar un pago | 7 milisegundos |
| De enviar un pago a verlo confirmado | 3 a 6 segundos |
| Una consulta al nodo, con la conexión abierta | 0,3 segundos |
| Memoria de la conexión segura | 49 KB |

## Cómo probarlo

**Las pruebas, en el PC.** Solo hace falta un compilador de C:

```sh
sh tests/host/run.sh
```

**El ejemplo, en una ESP32.** Con [PlatformIO](https://platformio.org):

1. Pon tu red Wi-Fi en `examples/esp32_pay_and_confirm/src/main.cpp`.
2. Graba la placa: `pio run -d examples/esp32_pay_and_confirm -e s3 -t upload`
   (o `-e esp32` para una ESP32 clásica).
3. Abre el monitor serie. La placa muestra su cuenta: dale fondos de prueba con
   `https://friendbot.stellar.org/?addr=<esa cuenta>`.
4. La placa paga 1 XLM, espera la confirmación y muestra el enlace a la transacción.

**En tu propio proyecto.** Copia `src/`, el adaptador de tu placa y `third_party/monocypher/`, y
mira en el ejemplo las dos funciones que tu placa tiene que aportar (`stl_crypto`). Para un pago
clásico alcanza un búfer de 1 KB; para una llamada a un contrato de token, reserva 2 KB.

## Hoja de ruta

Del 23 de noviembre al 22 de diciembre de 2026:

1. Guía de uso y un segundo ejemplo: un avisador de pagos.
2. Una segunda familia de placas, sin tocar el núcleo.
3. Detección de un pago por su número de pedido, para el cobro con QR.
4. Postulación a Stellar Instawards.

## Licencia

Apache 2.0. Monocypher conserva su propia licencia (BSD de 2 cláusulas o CC0), indicada en sus
archivos.

## Paizap

Paizap construye aparatos que cobran en Stellar. Este SDK es la pieza que tuvo que escribir para
su primer aparato, un terminal de cobro con tarjeta NFC, y la publica para que el siguiente lo
pueda construir cualquiera.
