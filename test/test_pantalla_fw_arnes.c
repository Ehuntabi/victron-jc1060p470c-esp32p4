/* Arnes del test de host de la validacion del firmware de las pantallas y del
 * texto que pinta la pagina /pantallas.
 *
 * Las funciones (pantalla_fw_valido, pantalla_bytes_legible y
 * pantalla_estado_texto) NO se copian aqui: test/test_pantalla_fw.sh las extrae
 * del fuente de produccion (main/portal/ota_pantalla.c) y las pega en el hueco
 * marcado mas abajo, igual que test_viaje_dueno.sh hace con las reglas del
 * viaje. Asi lo que se prueba es el codigo que corre en la P4.
 *
 * Se construye un .bin de mentira: los primeros 0x70 bytes con el magic en
 * 0x20, project_name en 0x50 y version en 0x30. Los casos fijan el contrato:
 * acepta una cabecera buena y rechaza cada forma de romperla. Y aparte se
 * prueban los tamanos/estados que ve el dueno en el movil.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*__PIEZAS__*/

/* Mismo tope que el contrato (0x3F0000). El script comprueba ademas que el
 * #define de produccion sigue valiendo esto. */
#define MAX_CONTRATO 0x3F0000u

static int fallos = 0;
static int pruebas = 0;
static uint8_t cab[0x70];

/* Monta una cabecera "buena": magic 0xABCD5432 en el offset 0x20 del fichero
 * (little-endian, tal y como sale en los .bin reales), version en 0x30 y
 * project_name en 0x50. */
static void monta(const char *proyecto, const char *version)
{
    memset(cab, 0, sizeof(cab));
    cab[0x20] = 0x32;
    cab[0x21] = 0x54;
    cab[0x22] = 0xCD;
    cab[0x23] = 0xAB;
    snprintf((char *)cab + 0x30, 32, "%s", version);
    snprintf((char *)cab + 0x50, 32, "%s", proyecto);
}

static void caso(const char *que, bool esperado, const char *modelo,
                 size_t bytes, size_t cab_n, const char *ver_esperada)
{
    char ver[32] = {0};
    const bool ok = pantalla_fw_valido(cab, cab_n, bytes, MAX_CONTRATO,
                                       modelo, ver, sizeof(ver));
    pruebas++;
    bool bien = (ok == esperado);
    if (esperado && ok) bien = bien && (strcmp(ver, ver_esperada) == 0);
    if (!bien) {
        printf("FALLO  %-46s -> %s (esperado %s), ver='%s'\n",
               que, ok ? "ACEPTA" : "RECHAZA",
               esperado ? "ACEPTA" : "RECHAZA", ver);
        fallos++;
    } else {
        printf("ok     %-46s -> %s%s%s\n", que,
               ok ? "ACEPTA" : "RECHAZA",
               (esperado && ok) ? " ver=" : "",
               (esperado && ok) ? ver : "");
    }
}

/* ── Texto de /pantallas ───────────────────────────────────────────────────
 * Tambien es codigo de PRODUCCION (extraido del fuente): el tamano y el estado
 * que ve el dueno en el movil. Se prueban los dos .bin publicados y los bordes
 * de unidad, porque un "2,1 MB" donde pone "2,2" no rompe nada pero confunde. */
static void texto_caso(const char *que, uint64_t bytes, const char *esperado)
{
    char out[32];
    pantalla_bytes_legible(bytes, out, sizeof(out));
    pruebas++;
    if (strcmp(out, esperado) != 0) {
        printf("FALLO  %-46s -> '%s' (esperado '%s')\n", que, out, esperado);
        fallos++;
    } else {
        printf("ok     %-46s -> %s\n", que, out);
    }
}

static void estado_caso(const char *que, const char *ver, uint64_t bytes,
                        const char *esperado)
{
    char out[64];
    pantalla_estado_texto(ver, bytes, out, sizeof(out));
    pruebas++;
    if (strcmp(out, esperado) != 0) {
        printf("FALLO  %-46s -> '%s' (esperado '%s')\n", que, out, esperado);
        fallos++;
    } else {
        printf("ok     %-46s -> %s\n", que, out);
    }
}

int main(void)
{
    /* ── Cabeceras buenas de los dos modelos publicados ─────────────────── */
    monta("cabina5", "v1.30");
    caso("cabecera buena de cabina5 (v1.30)", true, "cabina5",
         2214608, sizeof(cab), "v1.30");

    monta("35cabina", "v2.43");
    caso("cabecera buena de 35cabina (v2.43)", true, "35cabina",
         1937440, sizeof(cab), "v2.43");

    /* ── project_name: el de cada modelo, exacto ────────────────────────── */
    monta("cabina5", "v1.30");
    caso("el .bin de cabina5 pedido como 35cabina", false, "35cabina",
         2214608, sizeof(cab), "");
    monta("35cabina", "v2.43");
    caso("el .bin de 35cabina pedido como cabina5", false, "cabina5",
         1937440, sizeof(cab), "");
    monta("cabina5x", "v1.30");
    caso("project_name con un sufijo de mas", false, "cabina5",
         2214608, sizeof(cab), "");
    monta("cabina5", "v1.30");
    memset(cab + 0x50, 'a', 32);
    caso("project_name sin NUL dentro de sus 32 B", false, "cabina5",
         2214608, sizeof(cab), "");

    /* ── magic de esp_app_desc_t en 0x20 ────────────────────────────────── */
    monta("cabina5", "v1.30");
    cab[0x20] = 0;
    cab[0x21] = 0;
    cab[0x22] = 0;
    cab[0x23] = 0;
    caso("magic a cero", false, "cabina5", 2214608, sizeof(cab), "");
    monta("cabina5", "v1.30");
    cab[0x23] = 0xAA;
    caso("magic con el byte alto cambiado", false, "cabina5",
         2214608, sizeof(cab), "");
    monta("cabina5", "v1.30");
    cab[0x1C] = 0x32; cab[0x1D] = 0x54; cab[0x1E] = 0xCD; cab[0x1F] = 0xAB;
    cab[0x20] = 0; cab[0x21] = 0; cab[0x22] = 0; cab[0x23] = 0;
    caso("magic en el offset equivocado (0x1C)", false, "cabina5",
         2214608, sizeof(cab), "");

    /* ── tamano ─────────────────────────────────────────────────────────── */
    monta("cabina5", "v1.30");
    caso("justo en el tope de 0x3F0000", true, "cabina5",
         MAX_CONTRATO, sizeof(cab), "v1.30");
    caso("un byte por encima del tope", false, "cabina5",
         MAX_CONTRATO + 1, sizeof(cab), "");
    caso("mas pequeno que la cabecera", false, "cabina5",
         0x6F, sizeof(cab), "");
    caso("buffer de cabecera corto (0x6F)", false, "cabina5",
         2214608, 0x6F, "");

    /* ── version ────────────────────────────────────────────────────────── */
    monta("cabina5", "1.30");
    caso("version sin la 'v' delante", false, "cabina5",
         2214608, sizeof(cab), "");
    monta("cabina5", "");
    caso("version vacia", false, "cabina5", 2214608, sizeof(cab), "");
    monta("cabina5", "v1.30");
    cab[0x31] = 0x01;
    caso("version con un byte de control", false, "cabina5",
         2214608, sizeof(cab), "");
    /* 17 caracteres: el NUL cae FUERA de los 16 que lee el contrato. La
     * validacion acepta y devuelve los 16 primeros (el cliente vera una
     * version distinta y decidira; no es un fichero corrupto). */
    monta("cabina5", "v1234567890123456");
    caso("version de 16 bytes sin NUL dentro", true, "cabina5",
         2214608, sizeof(cab), "v123456789012345");

    /* ── argumentos nulos: no deben petar (es una tarea del httpd) ──────── */
    char ver[32] = {0};
    pruebas++;
    if (!pantalla_fw_valido(NULL, sizeof(cab), 2214608, MAX_CONTRATO,
                            "cabina5", ver, sizeof(ver))) {
        printf("ok     %-46s -> RECHAZA\n", "cabecera NULL");
    } else {
        printf("FALLO  %-46s -> ACEPTA\n", "cabecera NULL");
        fallos++;
    }
    pruebas++;
    if (!pantalla_fw_valido(cab, sizeof(cab), 2214608, MAX_CONTRATO,
                            NULL, ver, sizeof(ver))) {
        printf("ok     %-46s -> RECHAZA\n", "modelo NULL");
    } else {
        printf("FALLO  %-46s -> ACEPTA\n", "modelo NULL");
        fallos++;
    }

    /* ── texto de /pantallas: tamano y estado ───────────────────────────── */
    texto_caso("tamano del cabina5 v1.31 (2.237.216 B)", 2237216, "2,2 MB");
    texto_caso("tamano del 35cabina v2.44 (1.960.688 B)", 1960688, "2,0 MB");
    texto_caso("tamano en el tope (4.128.768 B)", 4128768, "4,1 MB");
    texto_caso("justo 1 MB", 1000000, "1,0 MB");
    texto_caso("999.999 B redondea a 1,0 MB", 999999, "1,0 MB");
    texto_caso("999 B se queda en bytes", 999, "999 B");
    texto_caso("justo 1 KB", 1000, "1 KB");
    texto_caso("cero bytes", 0, "0 B");

    estado_caso("estado con fichero", "v1.31", 2237216, "v1.31 (2,2 MB)");
    estado_caso("estado sin version (NULL)", NULL, 0, "no hay fichero");
    estado_caso("estado sin version (cadena vacia)", "", 0, "no hay fichero");

    printf("\n%d pruebas, %d fallos\n", pruebas, fallos);
    return fallos ? 1 : 0;
}
