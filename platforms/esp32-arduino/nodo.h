// Cliente del nodo RPC de Stellar para el terminal: una consulta JSON-RPC por HTTPS.
// Verifica el certificado del nodo contra la raíz de sdk/raiz.h y reutiliza la conexión: abrirla
// tarda de 2 a 4 segundos; con ella abierta, cada consulta tarda unos 0,3 s.
#pragma once
#include <Arduino.h>

namespace nodo {
// Hace la consulta y deja la respuesta en cuerpo(). `params` es el JSON de los parámetros o
// nullptr. Devuelve false si no hubo respuesta válida del nodo.
bool llamar(const char *metodo, const char *params);
const char *cuerpo();    // respuesta de la última consulta (solo lo que cabe en el búfer)
bool cortada();          // la respuesta no cupo entera
char *parametros();      // búfer para armar los parámetros de la siguiente consulta
size_t parametrosCap();
void cerrar();           // cierra la conexión y libera su memoria (unos 50 KB)
}  // namespace nodo
