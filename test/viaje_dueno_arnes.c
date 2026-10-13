/* Tabla de verdad de la PROPIEDAD DEL VIAJE, sobre el codigo REAL extraido de
 * config_server_viaje.c (ni una linea cambiada): lo unico sustituido es
 * dev_abierto(), que lee la NVS, por una variable. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"

static void destino_seguro(const char *in, char *out, size_t n);
int viaje_dev_ajeno(const char *dev_pide, const char *dev_abierto);

static bool dev_es_mac(const char *dev);

static char g_dueno[24] = {0};
static void dev_abierto(char *out, size_t n)
{
    /* Lo unico que se cambia del original es la lectura de la NVS. La segunda
     * linea es la del fichero de verdad, tal cual (auditoria del 14-oct-2026):
     * un dueno apuntado que no es una MAC vale como "sin dueno". */
    snprintf(out, n, "%s", g_dueno);
    if (!dev_es_mac(out)) out[0] = 0;
}

/*__PIEZAS__*/

static int fallos = 0, pruebas = 0;
static void caso(const char *que, const char *dueno, const char *json, uint32_t id,
                 bool admite_forzar, bool esperado)
{
    snprintf(g_dueno, sizeof(g_dueno), "%s", dueno ? dueno : "");
    cJSON *j = cJSON_Parse(json);
    if (!j) { printf("JSON INVALIDO: %s\n", json); fallos++; return; }
    bool got = pide_otra_pantalla(j, id, admite_forzar);
    pruebas++;
    if (got != esperado) {
        printf("FALLO  %-42s dueno=%-13s id=%08lx forzar=%d -> %d (esperado %d)\n",
               que, dueno && dueno[0] ? dueno : "(ninguno)", (unsigned long)id,
               (int)admite_forzar, (int)got, (int)esperado);
        fallos++;
    } else {
        printf("ok     %-42s -> %s\n", que, got ? "AJENA" : "propia");
    }
    cJSON_Delete(j);
}

int main(void)
{
    const char *D1 = "aabbccddee01";
    uint32_t id1 = 0x01000007u, id2 = 0x02000007u;   /* byte alto = mac[5] */

    caso("fin de la duena",              D1, "{\"dev\":\"aabbccddee01\"}", id1, false, false);
    caso("fin de la OTRA pantalla",      D1, "{\"dev\":\"aabbccddee02\"}", id2, false, true);
    caso("descartar de la OTRA",         D1, "{\"dev\":\"aabbccddee02\"}", id2, true,  true);
    caso("apunte de la duena (id 0x01)", D1, "{\"tipo\":\"repostaje\"}",   id1, false, false);
    caso("apunte de la OTRA (id 0x02)",  D1, "{\"tipo\":\"repostaje\"}",   id2, false, true);
    caso("forzar:true -> se deja pasar", D1, "{\"dev\":\"aabbccddee02\",\"forzar\":true}", id2, true, false);
    caso("forzar:1 (numero) -> igual",   D1, "{\"dev\":\"aabbccddee02\",\"forzar\":1}",    id2, true, false);
    caso("forzar NO vale en op_registro",D1, "{\"forzar\":true}",          id2, false, true);
    caso("viaje sin dueno (firmware viejo)", "", "{\"dev\":\"aabbccddee02\"}", id2, true, false);
    caso("dueno 'desconocido'",          "desconocido", "{\"dev\":\"aabbccddee02\"}", id2, true, false);
    caso("cuerpo con dev no-MAC",        D1, "{\"dev\":\"desconocido\"}",  id2, true, true);
    caso("id 0 sin dev (no es id valido)",D1, "{\"tipo\":\"repostaje\"}",  0,   false, false);
    caso("id sin byte alto (mac[5]=0)",  D1, "{\"tipo\":\"repostaje\"}",   7,   false, false);
    caso("dev distinto (mismo ultimo byte)", "112233445501", "{\"dev\":\"aabbccddee01\"}", id1, true, true);
    caso("mismo dev pero id de otra",    D1, "{\"dev\":\"aabbccddee01\"}", id2, true, false);

    printf("\n%d pruebas, %d fallos\n", pruebas, fallos);
    return fallos ? 1 : 0;
}
