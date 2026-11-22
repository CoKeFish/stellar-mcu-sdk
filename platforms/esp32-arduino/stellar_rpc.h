// Stellar RPC node client for the device: one JSON-RPC request over HTTPS.
// It verifies the node's certificate against the root in root_ca.h and reuses the connection:
// opening it takes 2 to 4 seconds; once open, each request takes about 0.3 s.
#pragma once
#include <Arduino.h>

namespace stellar_rpc {
// Makes the request and leaves the response in body(). `paramsJson` is the JSON of the parameters
// or nullptr. Returns false if there was no valid response from the node.
bool call(const char *method, const char *paramsJson);
const char *body();   // response to the last request (only what fits in the buffer)
bool truncated();     // the response did not fit entirely
char *params();       // buffer to build the parameters of the next request
size_t paramsCap();
void close();         // closes the connection and frees its memory (about 50 KB)
}  // namespace stellar_rpc
