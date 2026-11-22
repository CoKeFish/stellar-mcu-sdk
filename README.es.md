# Stellar MCU SDK

[English](README.md) · **Español**

[![Licencia](https://img.shields.io/badge/licencia-Apache%202.0-blue)](LICENSE)
![Lenguaje](https://img.shields.io/badge/lenguaje-C99-blue)
![Microcontroladores](https://img.shields.io/badge/microcontroladores-ESP32%20%7C%20ESP32--S3-blue)
![Red](https://img.shields.io/badge/Stellar-testnet-blue)

**Pagos de Stellar en dispositivos IoT.** `stellar-mcu-sdk` es una librería en C para
microcontroladores, hecha para llevar sistemas de pago a dispositivos IoT como torniquetes,
dispensadores y máquinas expendedoras. Habla con [Stellar RPC](https://developers.stellar.org/docs/data/apis/rpc) y
arma y firma las transacciones en el propio aparato, sin un servidor tuyo en el medio. Hecha por
[Paizap](https://github.com/CoKeFish/paizap).

Qué trae:

- lo necesario para armar y firmar transacciones en el aparato: pagos clásicos y llamadas a
  contratos de token (`transfer`, `transfer_from`),
- lo que un aparato necesita para consultar un nodo de Stellar RPC y leer la respuesta: cuentas,
  estado de una transacción y eventos de pago,
- pedidos de pago (SEP-7) para mostrar como QR,
- un cliente de red para Stellar RPC por HTTPS en ESP32.

El código y sus comentarios están en inglés; este README y la
[referencia de la API](docs/es/api.md) están también en español.

**Pruébalo:** [demo en vivo](https://paizap.vercel.app/demo/) · [documentación](https://paizap.vercel.app/sdk/)

> Estado: en desarrollo, sobre Stellar testnet. Mira [qué está comprobado](#qué-está-comprobado).

## Qué hace

| Pieza | Qué hace el aparato |
|---|---|
| **Cobrar** | Arma el pedido de pago (SEP-7) para mostrarlo como QR |
| **Confirmar** | Le pregunta a un nodo de Stellar RPC y comprueba él mismo que el pago llegó, con su monto y su número de pedido, si el pago lo trae |
| **Pagar** | Guarda su llave, arma la transacción, la firma y la envía: un pago clásico o una llamada a un contrato de token (`transfer`, `transfer_from`) |

Qué se puede construir con esas piezas:

- **Un aparato que actúa cuando le pagan,** como un torniquete, un dispensador o una máquina
  expendedora: muestra el pedido de pago como QR y confirma el pago en el nodo.
- **Un terminal de cobro.** El terminal de demostración de Paizap para tarjetas NFC cobra con
  `transfer_from`, armado y firmado en el aparato.
- **Un avisador de pagos:** una pantalla o un zumbador que anuncia cada pago que recibe una
  cuenta. No guarda ninguna llave.

## Microcontroladores

| Microcontrolador | Entorno | Estado | SHA-256 | Ed25519 |
|---|---|---|---|---|
| ESP32-S3 | Arduino, con PlatformIO | Probado en el aparato, en testnet | mbedTLS, incluida en el núcleo de Arduino para ESP32 | Monocypher 4.0.2 |
| ESP32 (clásico) | Arduino, con PlatformIO | Compila | mbedTLS, incluida en el núcleo de Arduino para ESP32 | Monocypher 4.0.2 |

El núcleo ([`src/`](src/)) es C99 portable: no reserva memoria y no incluye nada de ningún chip.
Una familia nueva de microcontroladores solo necesita un adaptador en [`platforms/`](platforms/) con la conexión segura al
nodo, más dos funciones: el SHA-256 y la firma Ed25519.

## Instalación

Agrega el SDK a tu proyecto de PlatformIO como una carpeta, por ejemplo como submódulo de Git:

```sh
git submodule add https://github.com/CoKeFish/stellar-mcu-sdk.git
```

y apunta tu `platformio.ini` a esa carpeta:

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

Después inclúyelo:

```c
#include "stellar_mcu.h"   // el núcleo
#include "stellar_rpc.h"   // el cliente de Stellar RPC para ESP32
```

Con otro sistema de compilación, compila `src/stellar_mcu.c`, el adaptador de tu microcontrolador y los dos
archivos de Monocypher, y agrega esas tres carpetas a la ruta de inclusión.

## Ejemplos rápidos

`CRYPTO` tiene las dos funciones que aporta tu microcontrolador (`stl_crypto`); el
[ejemplo completo](examples/esp32_pay_and_confirm/src/main.cpp) las muestra para ESP32.

### Enviar un pago

```c
stl_tx t = {0};
memcpy(t.source, public_key, 32);
t.fee = 100;
t.sequence = sequence + 1;         // leída del nodo con getLedgerEntries
t.valid_until = close_time + 120;  // la hora la da el nodo: no hace falta reloj propio
t.has_memo = true;
t.memo = 4242;                     // tu número de pedido

stl_xdr x;
uint8_t hash[32];
stl_tx_begin(&x, envelope, sizeof envelope, network_id);
stl_tx_payment(&x, &t, destination, NULL, 10000000);  // 1 XLM
stl_tx_sign(&x, &CRYPTO, seed, public_key, hash);     // firma en el aparato

char b64[1500];
stl_b64_encode(stl_tx_envelope(&x), stl_tx_envelope_len(&x), b64, sizeof b64);
// ...y b64 va a sendTransaction
```

### Pedir un pago

```c
stl_asset usdc = {false, "USDC", {0}};
stl_addr_decode("GBBD47IF6LWK7P7MDEVSCWR7DPUWV3NY3DTQEVFL4NAT4AQH3ZLLFLA5", STL_ADDR_ACCOUNT, usdc.issuer);

char uri[256];
stl_sep7_pay(uri, sizeof uri, merchant, 25000000, &usdc, true, 4242);  // 2.5 USDC, pedido 4242
// web+stellar:pay?destination=G...&amount=2.5&asset_code=USDC&asset_issuer=G...&memo=4242&memo_type=MEMO_ID
```

### Confirmar que un pago llegó

```c
// Temas para getEvents: los eventos «transfer» del contrato del token que van a tu cuenta
uint8_t buf[64];
char topic_transfer[32], topic_to[72];
size_t n = stl_scval_symbol(buf, sizeof buf, "transfer");
stl_b64_encode(buf, n, topic_transfer, sizeof topic_transfer);
n = stl_scval_account(buf, merchant);
stl_b64_encode(buf, n, topic_to, sizeof topic_to);

// ...y por cada evento que devuelve el nodo, pasa su "value" de base64 a bytes y léelo:
int64_t amount;
bool has_memo;
uint64_t order;
if (stl_event_amount(value, value_len, &amount, &has_memo, &order)) {
  // pagado: `amount` en stroops y, si has_memo, el número de pedido
}
```

### Llamar a un contrato de token

```c
stl_token_call call = {token_contract, "transfer_from", {spender, customer, merchant}, 3, 25000000};

stl_tx_begin(&x, envelope, sizeof envelope, network_id);
stl_tx_token_call(&x, &t, &call, NULL, 0);  // 1. armarla para simulateTransaction
stl_tx_unsigned(&x);
// ...el nodo responde con transactionData y minResourceFee

t.fee = 100 + (uint32_t)min_resource_fee;
stl_tx_begin(&x, envelope, sizeof envelope, network_id);
stl_tx_token_call(&x, &t, &call, soroban_data, soroban_data_len);  // 2. armarla otra vez con esos datos
stl_tx_sign(&x, &CRYPTO, seed, public_key, hash);                  //    y firmarla en el aparato
```

## Documentación

| Guía | Descripción |
|---|---|
| [Referencia de la API](docs/es/api.md) | Cada función del núcleo y del cliente para ESP32 |
| [Ejemplo: pagar y confirmar](examples/esp32_pay_and_confirm/src/main.cpp) | Un programa completo: el aparato crea su llave, paga 1 XLM, espera la confirmación y encuentra el pago en los eventos de la red |
| [Ejemplo: aparato autónomo](examples/esp32_standalone_device/) | Un aparato IoT de pagos hecho con un solo microcontrolador, con pantalla, teclado y relé: cobra con QR y espera el pago, paga, y muestra su cuenta y su saldo |
| [API reference](docs/api.md) | La referencia de la API, en inglés |

Qué hay en el repositorio:

| Carpeta | Contenido |
|---|---|
| `src/` | El núcleo: `stellar_mcu.h` y `stellar_mcu.c` |
| `platforms/esp32-arduino/` | Cliente de Stellar RPC por HTTPS para ESP32 con Arduino |
| `third_party/monocypher/` | [Monocypher](https://monocypher.org), sin cambios |
| `examples/` | Ejemplos para ESP32: pagar y confirmar, y un aparato autónomo |
| `tests/` | Pruebas en el PC contra el SDK oficial de JavaScript |
| `docs/` | Referencia de la API, en inglés y en español (`docs/es/`) |

## Compatibilidad

**Stellar RPC.** El SDK trabaja sobre Stellar RPC, no sobre Horizon. Estos son los métodos para los
que el núcleo trae ayudas:

| Método de RPC | Qué te da el SDK |
|---|---|
| `getHealth` | Lee el último ledger y su hora de cierre, con la que se pone el vencimiento de una transacción sin reloj propio |
| `getLedgerEntries` | Arma la clave de una cuenta (`stl_account_key`) y lee su saldo y su secuencia (`stl_account_parse`) |
| `simulateTransaction` | Arma la llamada al contrato de token para simular, y la vuelve a armar con los datos que devuelve el nodo (`stl_tx_token_call`) |
| `sendTransaction` | El sobre firmado, listo para enviar en base64 (`stl_tx_envelope`) |
| `getTransaction` | Lee el estado de la transacción (`stl_json_string`) |
| `getEvents` | Arma los temas para filtrar (`stl_scval_symbol`, `stl_scval_account`) y lee el monto y el memo de cada pago (`stl_event_amount`) |

`stellar_rpc::call` recibe el nombre del método, así que cualquier otro se llama igual.

**SEP.**

| SEP | Qué soporta |
|---|---|
| [SEP-7](https://github.com/stellar/stellar-protocol/blob/master/ecosystem/sep-0007.md) | Pedidos `pay` con destino, monto, activo y memo numérico |
| [SEP-23](https://github.com/stellar/stellar-protocol/blob/master/ecosystem/sep-0023.md) | Direcciones de cuenta (`G...`), de semilla (`S...`) y de contrato (`C...`), codificadas y leídas con su suma de control |
| [SEP-41](https://github.com/stellar/stellar-protocol/blob/master/ecosystem/sep-0041.md) | Llamadas a `transfer` y `transfer_from`, y lectura de los eventos `transfer` en sus dos formatos: el monto solo o el mapa con el monto y `to_muxed_id` |

**Protocolo.** El núcleo arma los mismos bytes que `@stellar/stellar-sdk` 17.4.0, y sus
transacciones entraron en testnet con el protocolo 30 (21 de noviembre de 2026).

## Qué está comprobado

| Qué | Cómo se comprobó |
|---|---|
| El núcleo arma lo mismo que el SDK oficial de JavaScript, byte a byte | 23 comprobaciones en el PC, todas correctas. 16 comparan byte a byte con `@stellar/stellar-sdk` 17.4.0: direcciones, contratos de token, pagos, llamadas `transfer` y `transfer_from`, autorizaciones y filtros de eventos. Las otras 7 cubren la lectura de direcciones y eventos, el texto SEP-7 y los montos |
| Un microcontrolador paga solo en testnet | Un ESP32-S3 armó, firmó y envió un [pago clásico](https://stellar.expert/explorer/testnet/tx/ca284b2c462693e4b2b6cbf0169b1732d9f165d4fdfa5efa0cf512975c0d0569) y una [transferencia en el contrato del token](https://stellar.expert/explorer/testnet/tx/65894777de6cc74e886a70964700e123918b67fce4538dfe64441eda9fee8e06), y después los detectó en los eventos de la red (21 de noviembre de 2026) |
| Un terminal cobra con tarjeta firmando él mismo | El terminal de demostración de Paizap cobra con tarjeta NFC en testnet con `transfer_from`, armado y firmado en el aparato: [esta transacción](https://stellar.expert/explorer/testnet/tx/dab4fb6bd5882fc1ebc62faf4d17848e1db2ba4c8905a9ebdfbb8fab1b2cd09b) es uno de esos cobros, y su única firma es la del aparato (22 de noviembre de 2026) |
| Los ejemplos compilan | Para ESP32-S3 y ESP32 clásico, con PlatformIO |

Medidas en el ESP32-S3:

| Qué | Medida |
|---|---|
| Núcleo del SDK | 3,4 KB de flash, sin memoria fija |
| Firma Ed25519 | 18 KB de flash |
| Armar y firmar un pago | 7 milisegundos |
| De enviar un pago a verlo confirmado | 3 a 6 segundos |
| Una consulta al nodo, con la conexión abierta | 0,3 segundos |
| Memoria de la conexión segura | 49 KB |

Para un pago clásico alcanza un búfer de 1 KB; para una llamada a un contrato de token, reserva 2 KB.

## Pruebas

**Las pruebas, en el PC.** Solo hace falta un compilador de C:

```sh
sh tests/host/run.sh
```

Arman direcciones, pagos, llamadas a contratos, autorizaciones y filtros de eventos con el núcleo y
comparan cada resultado con lo que produce el SDK oficial de JavaScript
([`tests/vectors/vectors.mjs`](tests/vectors/vectors.mjs) genera los valores de referencia).

**El ejemplo, en un ESP32.** Con [PlatformIO](https://platformio.org):

1. Pon tu red Wi-Fi en `examples/esp32_pay_and_confirm/src/main.cpp`.
2. Graba el firmware: `pio run -d examples/esp32_pay_and_confirm -e s3 -t upload`
   (o `-e esp32` para un ESP32 clásico).
3. Abre el monitor serie. El aparato muestra su cuenta: dale fondos de prueba con
   `https://friendbot.stellar.org/?addr=<esa cuenta>`.
4. El aparato paga 1 XLM, espera la confirmación y muestra el enlace a la transacción.

## Por qué existe este SDK

Stellar tiene SDK para la web, el celular y los servidores. Faltaba el del mundo físico.

- **La lista oficial no tiene ningún SDK para microcontroladores.** La documentación lista SDK de
  cliente para JavaScript, Python, Rust, iOS, Flutter, PHP, Kotlin Multiplatform, Java, Go y C# .NET
  ([Stellar Docs](https://developers.stellar.org/docs/tools/sdks/client-sdks)).
- **Quien lo intentó lo escribió a mano o puso una computadora en el medio.** El autor de un
  dispensador de dulces con ESP8266 escribió que, como no conoce ningún SDK de Stellar para
  Arduino, programó por su cuenta la lectura del flujo de pagos de Horizon
  ([stellar-candyman](https://github.com/sui77/stellar-candyman)). La máquina de dulces de
  SatoshiPay corre Node.js en una Raspberry Pi
  ([GitHub](https://github.com/satoshipay/stellar-candy-dispenser)).
- **La librería en C que existe es para billeteras físicas,** y su README avisa que lleva un
  tiempo sin actualizarse ([libstellar](https://github.com/overcat/libstellar)).
- **Lo poco que hay lee Horizon.** La documentación dice que Horizon se acerca al fin de su vida
  útil y que RPC es la API recomendada para acceder a los datos de la red en tiempo real
  ([Stellar Docs](https://developers.stellar.org/docs/data/apis)). Este SDK trabaja sobre
  Stellar RPC.

## Hoja de ruta

Del 23 de noviembre al 22 de diciembre de 2026:

| Semana | Qué hacemos | Meta |
|---|---|---|
| 1 (23 al 29 nov) | Dejar la librería lista para un tercero: guía de inicio y los dos ejemplos | Una persona ajena al equipo compila un ejemplo y paga en testnet siguiendo solo la guía |
| 2 (30 nov al 6 dic) | Llevar el mismo núcleo a un segundo microcontrolador, el RP2040 con su chip de Wi-Fi. Empezar la postulación a Stellar Instawards, por medio del capítulo local de embajadores | El mismo ejemplo de pago corre en 2 microcontroladores |
| 3 (7 al 13 dic) | Un ejemplo nuevo: el cobro con número de pedido. La librería ya lee ese número en el pago; el ejemplo espera el pago de otra persona y lo reconoce por él | Ejemplo publicado y probado en testnet |
| 4 (14 al 22 dic) | Enviar la postulación a Stellar Instawards y compartir la librería con desarrolladores de Stellar | Postulación enviada; 3 desarrolladores externos la compilan y dejan sus comentarios en este repositorio |

El mes cuesta el tiempo de los dos y el hardware para probar el segundo microcontrolador: una placa
con RP2040 y Wi-Fi cuesta [USD 6](https://www.raspberrypi.com/news/raspberry-pi-pico-w-your-6-iot-platform/).
Lo primero que comprobamos es que alguien ajeno al equipo use la librería sin nuestra ayuda. Lo
que sigue se financia con [Stellar Instawards](https://stellar.org/blog/ecosystem/introducing-the-stellar-ambassador-program):
premios de hasta USD 15.000 para experimentar y hacer prototipos, que se piden por medio de los
capítulos de embajadores de Stellar.

## Comentarios y aportes

- [Reporta un error o pide un microcontrolador](https://github.com/CoKeFish/stellar-mcu-sdk/issues).
- Para sumar una familia de microcontroladores, agrega una carpeta en `platforms/` con su conexión al nodo. El núcleo no
  cambia.
- Corre `sh tests/host/run.sh` antes de mandar un pull request.

## Licencia

Apache 2.0. Mira [LICENSE](LICENSE). Monocypher conserva su propia licencia (BSD de 2 cláusulas o
CC0), indicada en sus archivos.

## Paizap

Paizap lleva pagos de Stellar a dispositivos IoT: torniquetes, dispensadores, máquinas
expendedoras. Mantiene este SDK y lo publica para que cualquiera construya con él. Su
demostración, un terminal de cobro con tarjeta NFC, corre sobre este SDK.

### Equipo

| Quién | Rol | Qué aporta |
|---|---|---|
| **Rodion Romanovich** ([rodion.com.co](https://rodion.com.co), [@CoKeFish](https://github.com/CoKeFish)) | Hardware y Stellar | Ingeniero electrónico. Responsable del diseño electrónico, del firmware y del desarrollo de Stellar MCU SDK |
| **Clyn Roje Quispe Villegas** | Negocio y producto | Responsable del desarrollo de negocio y del enfoque de producto. A cargo de la presentación del proyecto, la identidad de marca y la producción audiovisual |
