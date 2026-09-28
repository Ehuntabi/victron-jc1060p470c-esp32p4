/* contrato_app.c — Ver contrato_app.h. Sin dependencias a proposito (ni cJSON ni
 * ESP-IDF): asi el CI lo compila con el gcc de la maquina y prueba EXACTAMENTE el
 * codigo que corre en la placa, en un segundo y sin hardware. */
#include "contrato_app.h"
#include <stdio.h>
#include <string.h>

const char *viaje_campos_que_faltan(const char *op, bool hay_carpeta, bool hay_id)
{
    if (!op || !op[0]) return "falta op";

    /* "borrar" es la que pide la app del movil: lleva "carpeta" y NO lleva id. */
    if (strcmp(op, "borrar") == 0) {
        return hay_carpeta ? NULL : "falta 'carpeta'";
    }

    /* Las del satelite (inicio/fin/registro/descartar) llevan "id" numerico. */
    return hay_id ? NULL : "faltan op o id";
}

/* Copia un texto listo para meter entre comillas en JSON: saltos de linea a
 * espacios y se escapan la comilla y la barra invertida. Nunca deja una barra
 * suelta al final (si no cabe, corta antes de empezarla). */
static size_t json_texto(const char *in, char *out, size_t n)
{
    size_t j = 0;
    if (n == 0) return 0;
    for (size_t i = 0; in && in[i] && j + 2 < n; i++) {
        const char c = in[i];
        if (c == '\n' || c == '\r') { out[j++] = ' '; continue; }
        if (c == '"' || c == '\\') out[j++] = '\\';
        out[j++] = c;
    }
    out[j] = '\0';
    return j;
}

int ausente_json(char *out, size_t n, bool activo, const char *motivo,
                 const char *salud, const char *aviso, int fotos, bool rotando)
{
    char mot[220], sal[120], avi[120];
    json_texto(motivo ? motivo : "", mot, sizeof(mot));
    json_texto(salud  ? salud  : "", sal, sizeof(sal));
    json_texto(aviso  ? aviso  : "", avi, sizeof(avi));

    /* Campos que lee la app (victron-app/lib/data/api_client.dart,
     * vigilanciaEstado()): vigilancia, motivo, salud, aviso, fotos, rotando. */
    return snprintf(out, n,
                    "{\"vigilancia\":%s,\"motivo\":\"%s\",\"salud\":\"%s\","
                    "\"aviso\":\"%s\",\"fotos\":%d,\"rotando\":%s}",
                    activo ? "true" : "false", mot, sal, avi, fotos,
                    rotando ? "true" : "false");
}
