// Ejemplo: una ESP32 paga en la testnet de Stellar y comprueba ella misma que el pago llegó.
//
// Qué hace, sin ningún servidor propio en el medio:
//   1. Crea su llave la primera vez y la guarda. Muestra su cuenta para que le des fondos.
//   2. Arma un pago de 1 XLM con un número de pedido (memo), lo firma y lo envía a un nodo RPC.
//   3. Espera a que la red lo confirme.
//   4. Busca el pago en los eventos de la red y lee el monto y el número de pedido.
//
// Para usarlo: pon tu red Wi-Fi y una cuenta de destino abajo, graba la placa, abre el monitor
// serie y dale fondos de prueba a la cuenta que muestra:
//   https://friendbot.stellar.org/?addr=<la cuenta que muestra la placa>
#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <mbedtls/sha256.h>

#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "nodo.h"
#include "nucleo.h"

// ---------------------------------------------------------------- lo que tienes que poner
static const char WIFI_SSID[] = "tu-red";
static const char WIFI_CLAVE[] = "tu-clave";
// Cuenta que recibe el pago: tiene que existir en testnet. Esta es una cuenta de prueba que solo
// recibe; cámbiala por la tuya.
static const char DESTINO[] = "GDJBOTBQNWREAVDUUS5XN7EJRRMUFLBNMFEJXSEHUHLYZDFEI2BL747I";

static const char RED[] = "Test SDF Network ; September 2015";
static const int64_t MONTO = 10000000;      // 1 XLM, en unidades mínimas (7 decimales)
static const uint64_t NUMERO_PEDIDO = 4242;  // viaja como memo y vuelve en el evento
static const uint32_t VIGENCIA_S = 120;      // el pago entra a la red en este plazo o no entra

// ---------------------------------------------------------------- lo que aporta la placa
static void placa_sha256(const uint8_t *datos, size_t n, uint8_t salida[32]) { mbedtls_sha256_ret(datos, n, salida, 0); }

static void placa_firmar(const uint8_t semilla[32], const uint8_t publica[32], const uint8_t *mensaje, size_t n,
                         uint8_t firma[64]) {
  uint8_t llave[64];  // Monocypher guarda la llave secreta como semilla + llave pública
  memcpy(llave, semilla, 32);
  memcpy(llave + 32, publica, 32);
  crypto_ed25519_sign(firma, llave, mensaje, n);
  crypto_wipe(llave, sizeof llave);
}

static const stl_cripto CRIPTO = {placa_sha256, placa_firmar};

static uint8_t g_semilla[32], g_publica[32], g_destino[32], g_id_red[32], g_contrato[32];
static uint8_t g_sobre[1200];
static char g_b64[1700];

// ---------------------------------------------------------------- consultas al nodo
// Último ledger y su hora de cierre: con esa hora se pone la vigencia, sin reloj propio.
static bool estado_de_la_red(int64_t *ledger, int64_t *cierre) {
  return nodo::llamar("getHealth", nullptr) && stl_json_entero(nodo::cuerpo(), "latestLedger", ledger) &&
         stl_json_entero(nodo::cuerpo(), "latestLedgerCloseTime", cierre);
}

// Saldo y secuencia de una cuenta. false si la cuenta todavía no existe en la red.
static bool leer_cuenta(const uint8_t cuenta[32], int64_t *saldo, int64_t *secuencia) {
  uint8_t clave[40], entrada[200];
  char clave_b64[64];
  stl_clave_cuenta(clave, cuenta);
  stl_b64_codificar(clave, 40, clave_b64, sizeof clave_b64);
  snprintf(nodo::parametros(), nodo::parametrosCap(), "{\"keys\":[\"%s\"]}", clave_b64);
  if (!nodo::llamar("getLedgerEntries", nodo::parametros())) return false;
  size_t n = 0;
  const char *xdr = stl_json_texto(nodo::cuerpo(), "xdr", &n);
  if (!xdr) return false;
  int bytes = stl_b64_decodificar(xdr, n, entrada, sizeof entrada);
  return bytes > 0 && stl_cuenta_leer(entrada, (size_t)bytes, saldo, secuencia);
}

// Envía el sobre firmado y espera hasta 30 segundos a que la red lo confirme.
static bool enviar_y_esperar(const stl_xdr *x, const char *hash_hex) {
  if (x->desbordo || !stl_b64_codificar(stl_tx_sobre(x), stl_tx_sobre_n(x), g_b64, sizeof g_b64)) return false;
  snprintf(nodo::parametros(), nodo::parametrosCap(), "{\"transaction\":\"%s\"}", g_b64);
  if (!nodo::llamar("sendTransaction", nodo::parametros())) return false;
  size_t n = 0;
  const char *estado = stl_json_texto(nodo::cuerpo(), "status", &n);
  if (!estado || strncmp(estado, "PENDING", 7)) {
    Serial.printf("La red no aceptó el envío: %.400s\n", nodo::cuerpo());
    return false;
  }
  for (int consulta = 0; consulta < 30; consulta++) {
    delay(1000);
    snprintf(nodo::parametros(), nodo::parametrosCap(), "{\"hash\":\"%s\"}", hash_hex);
    if (!nodo::llamar("getTransaction", nodo::parametros())) continue;
    estado = stl_json_texto(nodo::cuerpo(), "status", &n);
    if (estado && !strncmp(estado, "SUCCESS", 7)) return true;
    if (estado && !strncmp(estado, "FAILED", 6)) return false;
  }
  return false;
}

// Busca en los eventos del contrato del token el «transfer» hacia `destino` que pertenece a la
// transacción, y lee su monto y su número de pedido.
static bool buscar_pago(int64_t desde_ledger, const char *hash_hex, int64_t *monto, uint64_t *pedido) {
  char contrato_txt[57], tema_transfer[32], tema_destino[72];
  uint8_t buf[64];
  stl_dir_codificar(STL_DIR_CONTRATO, g_contrato, contrato_txt);
  size_t n = stl_scval_simbolo(buf, sizeof buf, "transfer");
  stl_b64_codificar(buf, n, tema_transfer, sizeof tema_transfer);
  n = stl_scval_cuenta(buf, g_destino);
  stl_b64_codificar(buf, n, tema_destino, sizeof tema_destino);

  for (int intento = 0; intento < 12; intento++) {
    snprintf(nodo::parametros(), nodo::parametrosCap(),
             "{\"startLedger\":%lld,\"filters\":[{\"type\":\"contract\",\"contractIds\":[\"%s\"],"
             "\"topics\":[[\"%s\",\"*\",\"%s\",\"**\"]]}],\"pagination\":{\"limit\":10}}",
             (long long)desde_ledger, contrato_txt, tema_transfer, tema_destino);
    if (nodo::llamar("getEvents", nodo::parametros())) {
      // Cada evento empieza con {"type": ; se revisa uno por uno en una copia.
      const char *p = strstr(nodo::cuerpo(), "{\"type\":");
      while (p) {
        const char *siguiente = strstr(p + 8, "{\"type\":");
        static char evento[1200];
        size_t largo = siguiente ? (size_t)(siguiente - p) : strlen(p);
        if (largo >= sizeof evento) largo = sizeof evento - 1;
        memcpy(evento, p, largo);
        evento[largo] = 0;
        if (strstr(evento, hash_hex)) {
          size_t l = 0;
          const char *valor = stl_json_texto(evento, "value", &l);
          uint8_t crudo[160];
          bool con_memo = false;
          int bytes = valor ? stl_b64_decodificar(valor, l, crudo, sizeof crudo) : -1;
          *pedido = 0;
          return bytes > 0 && stl_evento_monto(crudo, (size_t)bytes, monto, &con_memo, pedido);
        }
        p = siguiente;
      }
    }
    delay(1500);
  }
  return false;
}

// ---------------------------------------------------------------- el ejemplo
static void detener(const char *motivo) {
  Serial.printf("Detenido: %s\n", motivo);
  for (;;) delay(1000);
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\nStellar MCU SDK: pagar y confirmar desde la placa");

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_CLAVE);
  for (int i = 0; i < 40 && WiFi.status() != WL_CONNECTED; i++) delay(500);
  if (WiFi.status() != WL_CONNECTED) detener("sin Wi-Fi: revisa WIFI_SSID y WIFI_CLAVE");

  // La llave del aparato: se crea una vez con el generador aleatorio del chip y se guarda.
  Preferences guardado;
  guardado.begin("stellar-mcu", false);
  if (guardado.getBytes("semilla", g_semilla, 32) != 32) {
    esp_fill_random(g_semilla, 32);  // con el Wi-Fi encendido el chip entrega aleatoriedad real
    guardado.putBytes("semilla", g_semilla, 32);
  }
  guardado.end();
  uint8_t copia[32], llave[64];
  memcpy(copia, g_semilla, 32);  // Monocypher borra la semilla que recibe
  crypto_ed25519_key_pair(llave, g_publica, copia);
  crypto_wipe(llave, sizeof llave);

  char cuenta[57];
  stl_dir_codificar(STL_DIR_CUENTA, g_publica, cuenta);
  Serial.printf("Cuenta de la placa: %s\n", cuenta);
  if (!stl_dir_decodificar(DESTINO, STL_DIR_CUENTA, g_destino)) detener("DESTINO no es una cuenta válida");

  // La red y el contrato del token (XLM): sus eventos son los que se consultan al confirmar.
  placa_sha256((const uint8_t *)RED, strlen(RED), g_id_red);
  stl_activo xlm = {true, "", {0}};
  stl_contrato_de_activo(&CRIPTO, g_id_red, &xlm, g_contrato);

  int64_t saldo = 0, secuencia = 0, ledger = 0, cierre = 0;
  for (int i = 0; !leer_cuenta(g_publica, &saldo, &secuencia); i++) {
    if (i == 0) Serial.printf("Dale fondos de prueba: https://friendbot.stellar.org/?addr=%s\n", cuenta);
    delay(5000);
  }
  char texto[32];
  stl_monto_texto(saldo, texto, sizeof texto);
  Serial.printf("Saldo: %s XLM\n", texto);
  if (!estado_de_la_red(&ledger, &cierre)) detener("el nodo no responde");

  // 1. Armar y firmar el pago.
  stl_tx t;
  memset(&t, 0, sizeof t);
  memcpy(t.origen, g_publica, 32);
  t.comision = 100;
  t.secuencia = secuencia + 1;
  t.valido_hasta = (uint64_t)cierre + VIGENCIA_S;  // la hora la da el nodo
  t.con_memo = true;
  t.memo = NUMERO_PEDIDO;

  stl_xdr x;
  uint8_t hash[32];
  char hash_hex[65];
  uint32_t inicio = micros();
  stl_tx_iniciar(&x, g_sobre, sizeof g_sobre, g_id_red);
  stl_tx_pago(&x, &t, g_destino, NULL, MONTO);
  stl_tx_firmar(&x, &CRIPTO, g_semilla, g_publica, hash);
  Serial.printf("Pago armado y firmado en %u microsegundos\n", (unsigned)(micros() - inicio));
  for (int i = 0; i < 32; i++) sprintf(hash_hex + 2 * i, "%02x", hash[i]);

  // 2. Enviarlo y esperar a la red.
  inicio = millis();
  if (!enviar_y_esperar(&x, hash_hex)) detener("la red no confirmó el pago");
  Serial.printf("Pago confirmado en %u ms\n", (unsigned)(millis() - inicio));
  Serial.printf("https://stellar.expert/explorer/testnet/tx/%s\n", hash_hex);

  // 3. Comprobarlo en los eventos de la red, como haría un aparato que cobra.
  int64_t monto = 0;
  uint64_t pedido = 0;
  if (buscar_pago(ledger, hash_hex, &monto, &pedido)) {
    stl_monto_texto(monto, texto, sizeof texto);
    Serial.printf("Pago visto en la red: %s XLM, pedido %llu\n", texto, (unsigned long long)pedido);
  } else {
    Serial.println("El pago se confirmó, pero no apareció en los eventos de este nodo.");
  }
  nodo::cerrar();
}

void loop() { delay(1000); }
