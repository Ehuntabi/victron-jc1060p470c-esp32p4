/* Tabla de verdad del TOKEN DE LAS PANTALLAS (tanda 1, 20-oct-2026), sobre el
 * codigo REAL extraido de config_server_viaje.c y config_server_auth.c: ni una
 * linea copiada a mano. Lo unico que no se prueba aqui es la NVS (token_de_
 * pantalla_lee/emite), que necesita la placa; lo que se prueba es la DECISION,
 * que es donde estan los fallos que importan:
 *
 *   · una pantalla sin emparejar (firmware viejo) sigue cerrando con su 'dev',
 *     como en la v4.56 -- si esto se rompe, la emergencia se queda sin salida;
 *   · una pantalla emparejada solo cierra con SU token, no con uno de otra MAC
 *     ni con uno inventado;
 *   · los tokens con forma rara (33 caracteres, "%s", 31...) no cuelan. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>

/* La funcion de verdad se extrae de config_server_auth.c; aqui solo su firma,
 * para que el orden de las piezas no importe. */
bool auth_equal_ct(const char *a, const char *b);

/*__PIEZAS__*/

static int fallos = 0, pruebas = 0;

static void fallo(const char *que, const char *detalle)
{
    printf("FALLO  %-52s %s\n", que, detalle);
    fallos++;
}

/* token_autoriza_forzar(): la decision del 'forzar' con 'dev'. */
static void caso(const char *que, const char *dev, const char *token_cuerpo,
                 const char *token_guardado, bool esperado)
{
    pruebas++;
    bool got = token_autoriza_forzar(dev, token_cuerpo, token_guardado);
    if (got != esperado) {
        char det[160];
        snprintf(det, sizeof(det), "dev=%s cuerpo=%s guardado=%s -> %s (esperado %s)",
                 dev ? dev : "(null)", token_cuerpo ? token_cuerpo : "(null)",
                 token_guardado ? token_guardado : "(null)",
                 got ? "ABIERTO" : "CLAVE", esperado ? "ABIERTO" : "CLAVE");
        fallo(que, det);
    } else {
        printf("ok     %-52s -> %s\n", que, got ? "nivel abierto" : "clave estricta");
    }
}

static void caso_normaliza(const char *que, const char *in, bool esperado, const char *esperado_out)
{
    pruebas++;
    char out[33] = {0};
    bool got = token_normaliza(in, out, sizeof(out));
    if (got != esperado || (esperado && strcmp(out, esperado_out) != 0)) {
        char det[160];
        snprintf(det, sizeof(det), "in=%s -> %s '%s' (esperado %s '%s')",
                 in ? in : "(null)", got ? "true" : "false", got ? out : "",
                 esperado ? "true" : "false", esperado ? esperado_out : "");
        fallo(que, det);
    } else {
        printf("ok     %-52s -> %s\n", que, got ? out : "rechazado");
    }
}

static void caso_clave(const char *que, const char *dev, const char *esperado)
{
    pruebas++;
    char clave[16] = {0};
    token_de_pantalla_clave(dev, clave, sizeof(clave));
    if (strcmp(clave, esperado) != 0) {
        char det[96];
        snprintf(det, sizeof(det), "dev=%s -> '%s' (esperado '%s')", dev, clave, esperado);
        fallo(que, det);
    } else {
        printf("ok     %-52s -> %s\n", que, clave);
    }
}

int main(void)
{
    const char *D1 = "aabbccddee01";      /* dos pantallas de este montaje */
    const char *D2 = "aabbccddee02";
    const char *T1 = "00112233445566778899aabbccddeeff";   /* secreto de D1 */
    const char *T2 = "ffeeddccbbaa99887766554433221100";   /* secreto de D2 */

    /* ── La clave de NVS: 15 caracteres como mucho, y las dos MACs distintas ── */
    caso_clave("clave de la 5\"",                 D1, "tk_ccddee01");
    caso_clave("clave de la 3,5\"",               D2, "tk_ccddee02");
    caso_clave("clave con la MAC en MAYUSCULAS",  "AABBCCDDEE01", "tk_ccddee01");
    caso_clave("clave de otra MAC cualquiera",    "112233445501", "tk_33445501");
    pruebas++;
    {
        char a[16], b[16];
        token_de_pantalla_clave(D1, a, sizeof(a));
        token_de_pantalla_clave(D2, b, sizeof(b));
        if (strcmp(a, b) == 0 || strlen(a) > 15) {
            fallo("las dos MACs dan la MISMA clave (o no cabe en la NVS)", a);
        } else {
            printf("ok     %-52s -> %s / %s (%zu car.)\n",
                   "claves distintas y dentro del tope de NVS", a, b, strlen(a));
        }
    }

    /* ── Forma del token ───────────────────────────────────────────────────── */
    caso_normaliza("32 hex en minusculas", T1, true, T1);
    caso_normaliza("32 hex en MAYUSCULAS", "00112233445566778899AABBCCDDEEFF", true, T1);
    caso_normaliza("31 caracteres", "00112233445566778899aabbccddee", false, NULL);
    caso_normaliza("33 caracteres", "00112233445566778899aabbccddeeff0", false, NULL);
    caso_normaliza("32 con una 'g'", "00112233445566778899aabbccddeefg", false, NULL);
    caso_normaliza("el formato de printf, '%s' x16", "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s", false, NULL);
    caso_normaliza("cadena vacia", "", false, NULL);
    caso_normaliza("NULL", NULL, false, NULL);

    /* ── LA DECISION DEL 'forzar' ──────────────────────────────────────────── */
    printf("\n-- pantalla SIN emparejar (firmware viejo, v4.56) --\n");
    caso("sin emparejar y sin token: vale el dev",        D1, NULL, "", true);
    caso("sin emparejar y con token basura: vale el dev", D1, "loquesea", "", true);
    caso("sin emparejar y con token de 32 hex",           D1, T1, "", true);

    printf("\n-- pantalla EMPAREJADA --\n");
    caso("token correcto",                                D1, T1, T1, true);
    caso("token correcto en MAYUSCULAS",                  D1, "00112233445566778899AABBCCDDEEFF", T1, true);
    caso("sin token (cabina vieja tras emparejar)",       D1, NULL, T1, false);
    caso("token vacio",                                   D1, "", T1, false);
    caso("el token de la OTRA pantalla",                  D1, T2, T1, false);
    caso("token correcto pero de la otra MAC",            D2, T1, T2, false);
    caso("token de 33 caracteres",                        D1, "00112233445566778899aabbccddeeff0", T1, false);
    caso("token de 31 caracteres",                        D1, "00112233445566778899aabbccddee", T1, false);
    caso("un hex inventado del mismo tamaño",             D1, "00000000000000000000000000000000", T1, false);
    caso("el formato de printf en el token",              D1, "%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s", T1, false);
    caso("token valido pero guardado corrupto",           D1, T1, "00112233", true);

    printf("\n-- el 'dev' que no es una MAC (web, app, POST a mano) --\n");
    caso("sin dev",                                       NULL, T1, "", false);
    caso("dev vacio",                                     "", NULL, "", false);
    caso("dev 'desconocido'",                             "desconocido", NULL, "", false);
    caso("dev de 11 hex",                                 "aabbccddee0", NULL, "", false);
    caso("dev de 13 hex",                                 "aabbccddee011", NULL, "", false);
    caso("dev con una 'g'",                               "aabbccddee0g", NULL, "", false);
    caso("dev de 12 hex y la clave guardada",              D1, "loquesea", T1, false);
    caso("dev en MAYUSCULAS con su token",                "AABBCCDDEE01", T1, T1, true);

    printf("\n%d pruebas, %d fallos\n", pruebas, fallos);
    return fallos ? 1 : 0;
}
