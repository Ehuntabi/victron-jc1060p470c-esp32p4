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
static bool id_rezagado(uint32_t id);

static char g_dueno[24] = {0};
static void dev_abierto(char *out, size_t n)
{
    /* Lo unico que se cambia del original es la lectura de la NVS. La segunda
     * linea es la del fichero de verdad, tal cual (auditoria del 14-oct-2026):
     * un dueno apuntado que no es una MAC vale como "sin dueno". */
    snprintf(out, n, "%s", g_dueno);
    if (!dev_es_mac(out)) out[0] = 0;
}

/* Y lo mismo con el id del inicio del viaje abierto, que tambien sale de la NVS:
 * la funcion de verdad (id_rezagado) se extrae del fuente y esta solo le da el
 * dato. 0 = no se sabe (sin viaje abierto, o viaje de un firmware que no lo
 * apuntaba), y en ese caso id_rezagado() tiene que decir que NO. */
static uint32_t g_ini;
static uint32_t viaje_ini_id(void) { return g_ini; }

/* El id con el que la WEB cierra un viaje (el del fuente, tal cual): fijo y con
 * el byte alto 0xFF, que es justo lo que lo hacia confundible con una pantalla
 * de MAC ...FF. Ver los casos de cierre web de main(). */
#define ID_CIERRE_WEB   0xFFFFFF01u

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

/* ID REZAGADO: un evento de la MISMA pantalla pero con numero ANTERIOR al inicio
 * del viaje abierto. Es lo que cierra el fallo del "fin" reintentado que cerraba
 * el viaje nuevo (16-oct-2026): la cola de la cabina reintenta la cabeza hasta
 * que la P4 contesta, y el anillo de dedupe se vacia al abrir viaje, asi que el
 * reintento no se reconocia como duplicado. */
static void caso_rezagado(const char *que, uint32_t ini, uint32_t id, bool esperado)
{
    g_ini = ini;
    bool got = id_rezagado(id);
    pruebas++;
    if (got != esperado) {
        printf("FALLO  %-46s ini=%08lx id=%08lx -> %d (esperado %d)\n",
               que, (unsigned long)ini, (unsigned long)id, (int)got, (int)esperado);
        fallos++;
    } else {
        printf("ok     %-46s -> %s\n", que, got ? "REZAGADO" : "del viaje de ahora");
    }
}

/* EL CIERRE DE LA WEB CON 'forzar' (16-oct-2026).
 *
 * El id de cierre de la web es fijo y su byte alto es 0xFF, que es el de una
 * MAC que acabe en 0xFF: con esa pantalla de dueño, el id de la web caia en la
 * comparacion de contadores y el cierre se tomaba por rezagado (200 y no
 * cerraba nada). Aqui se fija que NO es rezagado, y ademas que una operacion de
 * cierre sigue pasando por la guarda de dueño salvo que traiga 'forzar'. */
static void caso_cierre_web(const char *que, uint32_t ini, bool esperado_rezagado)
{
    caso_rezagado(que, ini, 0xFFFFFF01u, esperado_rezagado);
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

    /* ── id_rezagado(): el fin reintentado del viaje anterior ─────────────── */
    caso_rezagado("misma pantalla, numero anterior",      0x01000040u, 0x01000005u, true);
    caso_rezagado("el fin del viaje anterior (caso real)", 0x0A000028u, 0x0A000003u, true);
    caso_rezagado("misma pantalla, el de ahora",          0x01000040u, 0x01000041u, false);
    caso_rezagado("misma pantalla, el del inicio",        0x01000040u, 0x01000040u, false);
    caso_rezagado("OTRA pantalla",                        0x01000040u, 0x02000005u, false);
    caso_rezagado("sin id de inicio apuntado",            0u,         0x01000005u, false);
    caso_rezagado("id 0 (no es id valido)",               0x01000040u, 0u,          false);
    caso_rezagado("inicio sin byte de pantalla (web)",    0x00000040u, 0x00000005u, false);
    caso_rezagado("los dos sin byte de pantalla",         0x00000040u, 0x00000041u, false);

    /* ── El CONTADOR DE LA CABINA REINICIADO (16-oct-2026) ────────────────────
     *
     * Si al satelite se le borra la memoria (o la cabina es nueva y hereda la
     * MAC), su next_trip_seq() vuelve a empezar. Los apuntes del viaje nuevo
     * llevan numeros BAJOS con el mismo byte de pantalla, asi que la regla de
     * "numero menor que el del inicio" los tomaria por rezagados del viaje
     * anterior y los tiraria (un "fin" contestado "duplicado" y apuntes con
     * 409). La regla de verdad es "numero ANTERIOR AL INICIO", y si lo que hay
     * abierto empezo en el numero 5, un 9 es posterior: es del viaje de ahora.
     *
     * (El caso que SI es ambiguo -- contador que da la vuelta entera, 16
     * millones de eventos -- no se puede distinguir sin cambiar el protocolo:
     * se queda como estaba, y por eso la comparacion sigue siendo la misma.) */
    caso_rezagado("contador reiniciado: inicio 5, ahora 9", 0x01000005u, 0x01000009u, false);
    caso_rezagado("contador reiniciado: el inicio mismo",   0x01000001u, 0x01000001u, false);
    caso_rezagado("contador reiniciado: apunte anterior al inicio", 0x01000009u, 0x01000002u, true);
    caso_rezagado("contador alto y apunte bajo (viaje anterior)",   0x0100FF00u, 0x01000010u, true);

    /* ── El CIERRE DE LA WEB, que trae 'forzar' (16-oct-2026) ─────────────────
     * ID_CIERRE_WEB = 0xFFFFFF01. Su byte alto (0xFF) puede coincidir con el de
     * una MAC, y entonces el cierre se tomaba por rezagado: 200 y no cerraba
     * nada. No es un evento de pantalla: no puede ser rezagado de nada. */
    caso_cierre_web("cierre web con MAC ...FF y contador alto", 0xFF00FF00u, false);
    caso_cierre_web("cierre web con cualquier otro inicio",     0x01000040u, false);
    caso_cierre_web("cierre web sin viaje abierto",             0u,          false);

    /* Y "fin" con 'forzar' sigue siendo de la pantalla que lo manda cuando el
     * dueño coincide: la llave solo salta la guarda de dueño, no la de
     * rezagado (que es la misma pantalla). */
    caso("fin de la duena con forzar",   D1, "{\"dev\":\"aabbccddee01\",\"forzar\":true}", id1, true, false);
    caso("fin de OTRA con forzar (emergencia)", D1,
         "{\"dev\":\"aabbccddee02\",\"forzar\":true}", id2, true, false);

    printf("\n%d pruebas, %d fallos\n", pruebas, fallos);
    return fallos ? 1 : 0;
}
