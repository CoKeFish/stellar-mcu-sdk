#include "stellar_rpc.h"

#include <WiFiClientSecure.h>

#include "root_ca.h"

// Stellar RPC node the device connects to. Change it at build time, for example with
// -DSTELLAR_RPC_HOST=\"my-node.example.com\". Another node may need another certificate in
// root_ca.h.
#ifndef STELLAR_RPC_HOST
#define STELLAR_RPC_HOST "soroban-testnet.stellar.org"
#endif
// Path of the node inside that host. A provider whose address carries a path or a key needs
// it, for example -DSTELLAR_RPC_PATH=\"/my-key\".
#ifndef STELLAR_RPC_PATH
#define STELLAR_RPC_PATH "/"
#endif

namespace {

WiFiClientSecure tls;
char paramsBuf[3072], request[3400], response[6144];
size_t responseLen = 0;
bool responseTruncated = false;
uint8_t inBuf[1024];
size_t inLen = 0, inPos = 0;

bool openConnection() {
  uint32_t start = millis();
  tls.setCACert(STELLAR_RPC_ROOT_CA_PEM);
  tls.setHandshakeTimeout(20);
  bool ok = tls.connect(STELLAR_RPC_HOST, 443);
  Serial.printf("Node %s: secure connection %s in %u ms\n", STELLAR_RPC_HOST, ok ? "open" : "FAILED",
                (unsigned)(millis() - start));
  return ok;
}

// Next byte of the response; -1 if the connection was closed, -2 if it timed out.
int nextByte(uint32_t timeoutMs) {
  if (inPos < inLen) return inBuf[inPos++];
  uint32_t start = millis();
  for (;;) {
    int available = tls.available();
    if (available > 0) {
      int got = tls.read(inBuf, min((size_t)available, sizeof inBuf));
      if (got > 0) {
        inLen = (size_t)got;
        inPos = 1;
        return inBuf[0];
      }
    }
    if (!tls.connected()) return -1;
    if (millis() - start > timeoutMs) return -2;
    delay(1);
  }
}

int readLine(char *line, size_t cap, uint32_t timeoutMs) {
  size_t n = 0;
  for (;;) {
    int c = nextByte(timeoutMs);
    if (c < 0) return c;
    if (c == '\n') break;
    if (c != '\r' && n + 1 < cap) line[n++] = (char)c;
  }
  line[n] = 0;
  return (int)n;
}

// Reads n bytes of the body. Keeps what fits; the rest is discarded, but still read so the
// connection is ready for the next request.
bool readBody(size_t n) {
  for (size_t i = 0; i < n; i++) {
    int c = nextByte(15000);
    if (c < 0) return false;
    if (responseLen + 1 < sizeof response) response[responseLen++] = (char)c;
    else responseTruncated = true;
  }
  return true;
}

// One POST request with the JSON already built. If the kept connection was closed, it opens
// another one and retries once. Returns the HTTP status code, or 0 if there was no response.
int post(const char *json, size_t n) {
  for (int attempt = 0; attempt < 2; attempt++) {
    if (!tls.connected()) {
      tls.stop();
      if (!openConnection()) return 0;
    }
    inLen = inPos = 0;
    responseLen = 0;
    responseTruncated = false;
    char headers[224];
    int nh = snprintf(headers, sizeof headers,
                      "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
                      "Content-Length: %u\r\nConnection: keep-alive\r\n\r\n",
                      STELLAR_RPC_PATH, STELLAR_RPC_HOST, (unsigned)n);
    if (nh < 0 || (size_t)nh >= sizeof headers) {
      Serial.println("Node: host and path are too long for the request");
      return 0;
    }
    tls.write((const uint8_t *)headers, (size_t)nh);
    tls.write((const uint8_t *)json, n);

    char line[256];
    int l = readLine(line, sizeof line, 20000);
    if (l < 0) {
      tls.stop();
      continue;
    }
    int http = atoi(line + 9);
    long contentLength = -1;
    bool chunked = false, closeAfter = false;
    while ((l = readLine(line, sizeof line, 15000)) > 0) {
      if (!strncasecmp(line, "content-length:", 15)) contentLength = atol(line + 15);
      if (!strncasecmp(line, "transfer-encoding:", 18) && strstr(line, "chunked")) chunked = true;
      if (!strncasecmp(line, "connection:", 11) && strstr(line, "close")) closeAfter = true;
    }
    bool ok = l == 0;
    if (ok && chunked) {
      for (;;) {
        if (readLine(line, sizeof line, 15000) < 0) { ok = false; break; }
        size_t chunk = strtoul(line, NULL, 16);
        if (!chunk) {
          readLine(line, sizeof line, 2000);
          break;
        }
        if (!readBody(chunk)) { ok = false; break; }
        readLine(line, sizeof line, 5000);
      }
    } else if (ok && contentLength >= 0) {
      ok = readBody((size_t)contentLength);
    }
    response[responseLen] = 0;
    if (closeAfter || !ok) tls.stop();
    return ok ? http : 0;
  }
  return 0;
}

}  // namespace

namespace stellar_rpc {

bool call(const char *method, const char *paramsJson) {
  int n = snprintf(request, sizeof request, "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"%s\"%s%s}", method,
                   paramsJson ? ",\"params\":" : "", paramsJson ? paramsJson : "");
  if (n <= 0 || (size_t)n >= sizeof request) return false;
  uint32_t start = millis();
  int http = post(request, (size_t)n);
  Serial.printf("Node: %s -> %d, %u bytes, %u ms%s\n", method, http, (unsigned)responseLen,
                (unsigned)(millis() - start), responseTruncated ? " (truncated)" : "");
  return http == 200;
}

const char *body() { return response; }
bool truncated() { return responseTruncated; }
char *params() { return paramsBuf; }
size_t paramsCap() { return sizeof paramsBuf; }
void close() { tls.stop(); }

}  // namespace stellar_rpc
