#include "nodo.h"

#include <WiFiClientSecure.h>

#include "raiz.h"

// Nodo de Stellar RPC al que se conecta el aparato. Se cambia al compilar, por ejemplo con
// -DNODO_ANFITRION=\"mi-nodo.ejemplo.com\". Otro nodo puede necesitar otro certificado en raiz.h.
#ifndef NODO_ANFITRION
#define NODO_ANFITRION "soroban-testnet.stellar.org"
#endif

namespace {

WiFiClientSecure tls;
char params[3072], peticion[3400], respuesta[6144];
size_t respuestaN = 0;
bool respuestaCortada = false;
uint8_t entrada[1024];
size_t entradaN = 0, entradaI = 0;

bool conectar() {
  uint32_t inicio = millis();
  tls.setCACert(RAIZ_NODO_PEM);
  tls.setHandshakeTimeout(20);
  bool ok = tls.connect(NODO_ANFITRION, 443);
  Serial.printf("Nodo %s: conexion segura %s en %u ms\n", NODO_ANFITRION, ok ? "abierta" : "FALLIDA",
                (unsigned)(millis() - inicio));
  return ok;
}

// Siguiente byte de la respuesta; -1 si se cerró la conexión, -2 si se agotó el tiempo.
int byteSiguiente(uint32_t esperaMs) {
  if (entradaI < entradaN) return entrada[entradaI++];
  uint32_t inicio = millis();
  for (;;) {
    int disponibles = tls.available();
    if (disponibles > 0) {
      int leidos = tls.read(entrada, min((size_t)disponibles, sizeof entrada));
      if (leidos > 0) {
        entradaN = (size_t)leidos;
        entradaI = 1;
        return entrada[0];
      }
    }
    if (!tls.connected()) return -1;
    if (millis() - inicio > esperaMs) return -2;
    delay(1);
  }
}

int leerLinea(char *linea, size_t cap, uint32_t esperaMs) {
  size_t n = 0;
  for (;;) {
    int c = byteSiguiente(esperaMs);
    if (c < 0) return c;
    if (c == '\n') break;
    if (c != '\r' && n + 1 < cap) linea[n++] = (char)c;
  }
  linea[n] = 0;
  return (int)n;
}

// Lee n bytes del cuerpo. Guarda lo que quepa; el resto se descarta, pero se lee igual para que
// la conexión quede lista para la siguiente consulta.
bool leerCuerpo(size_t n) {
  for (size_t i = 0; i < n; i++) {
    int c = byteSiguiente(15000);
    if (c < 0) return false;
    if (respuestaN + 1 < sizeof respuesta) respuesta[respuestaN++] = (char)c;
    else respuestaCortada = true;
  }
  return true;
}

// Una petición POST con el JSON ya armado. Si la conexión guardada estaba cerrada, abre otra y
// repite una vez. Devuelve el código HTTP o 0 si no hubo respuesta.
int post(const char *json, size_t n) {
  for (int intento = 0; intento < 2; intento++) {
    if (!tls.connected()) {
      tls.stop();
      if (!conectar()) return 0;
    }
    entradaN = entradaI = 0;
    respuestaN = 0;
    respuestaCortada = false;
    char cabecera[224];
    int nc = snprintf(cabecera, sizeof cabecera,
                      "POST / HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
                      "Content-Length: %u\r\nConnection: keep-alive\r\n\r\n",
                      NODO_ANFITRION, (unsigned)n);
    tls.write((const uint8_t *)cabecera, (size_t)nc);
    tls.write((const uint8_t *)json, n);

    char linea[256];
    int l = leerLinea(linea, sizeof linea, 20000);
    if (l < 0) {
      tls.stop();
      continue;
    }
    int http = atoi(linea + 9);
    long largo = -1;
    bool porTrozos = false, cerrar = false;
    while ((l = leerLinea(linea, sizeof linea, 15000)) > 0) {
      if (!strncasecmp(linea, "content-length:", 15)) largo = atol(linea + 15);
      if (!strncasecmp(linea, "transfer-encoding:", 18) && strstr(linea, "chunked")) porTrozos = true;
      if (!strncasecmp(linea, "connection:", 11) && strstr(linea, "close")) cerrar = true;
    }
    bool ok = l == 0;
    if (ok && porTrozos) {
      for (;;) {
        if (leerLinea(linea, sizeof linea, 15000) < 0) { ok = false; break; }
        size_t trozo = strtoul(linea, NULL, 16);
        if (!trozo) {
          leerLinea(linea, sizeof linea, 2000);
          break;
        }
        if (!leerCuerpo(trozo)) { ok = false; break; }
        leerLinea(linea, sizeof linea, 5000);
      }
    } else if (ok && largo >= 0) {
      ok = leerCuerpo((size_t)largo);
    }
    respuesta[respuestaN] = 0;
    if (cerrar || !ok) tls.stop();
    return ok ? http : 0;
  }
  return 0;
}

}  // namespace

namespace nodo {

bool llamar(const char *metodo, const char *parametrosJson) {
  int n = snprintf(peticion, sizeof peticion, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\"%s%s}", metodo,
                   parametrosJson ? ",\"params\":" : "", parametrosJson ? parametrosJson : "");
  if (n <= 0 || (size_t)n >= sizeof peticion) return false;
  uint32_t inicio = millis();
  int http = post(peticion, (size_t)n);
  Serial.printf("Nodo: %s -> %d, %u bytes, %u ms%s\n", metodo, http, (unsigned)respuestaN,
                (unsigned)(millis() - inicio), respuestaCortada ? " (cortada)" : "");
  return http == 200;
}

const char *cuerpo() { return respuesta; }
bool cortada() { return respuestaCortada; }
char *parametros() { return params; }
size_t parametrosCap() { return sizeof params; }
void cerrar() { tls.stop(); }

}  // namespace nodo
