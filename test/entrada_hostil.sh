#!/usr/bin/env bash
# ENTRADA HOSTIL — que pasa cuando lo que se teclea NO es lo que se espera.
#
# Prueba funciones REALES extraidas del fuente (ni una linea copiada a mano):
#   * en una cabina (cabina5 / 35cabina): main/net/apunte.c
#       apunte_cabecera / apunte_campo_txt / apunte_campo_num / apunte_cerrar
#     -> monta el cuerpo JSON con textos hostiles y lo valida con el parser de
#        Python: el JSON tiene que ser VALIDO y el texto tiene que volver
#        IDENTICO (byte a byte) al que se metio. Un texto que se pierde o se
#        corta es un dato perdido; un JSON invalido es un 400 de la P4 y la cola
#        atascada para siempre.
#   * en la P4 (victron): main/portal/config_server_viaje.c
#       destino_seguro()  -> el nombre de la CARPETA del viaje
#       csv_texto()       -> el valor que va a un CSV
#       csv_por_tipo()    -> la FILA y la CABECERA del CSV de cada tipo
#
# Uso:  bash test/entrada_hostil.sh          (desde la raiz del repo)
#       bash test/entrada_hostil.sh -v       (muestra la matriz entera)
#
# No necesita IDF ni placa: gcc + python3. El unico cJSON que usa es el del IDF
# para la parte de la P4; si no esta, esa parte se SALTA (y lo dice).
set -uo pipefail
cd "$(dirname "$0")/.." || exit 2
VERBOSE=0; [ "${1:-}" = "-v" ] && VERBOSE=1

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT

fallos=0; pruebas=0
rojo() { echo "FALLO  $*"; fallos=$((fallos+1)); }
ok()   { [ "$VERBOSE" = 1 ] && echo "ok     $*"; return 0; }
nota() { echo "  $*"; }

# ── piezas: extrae una funcion del fuente por su firma (llaves balanceadas) ──
extrae_py='
import re, sys, pathlib
COMILLA = chr(39)
def extrae(src, firma):
    """Extrae la funcion ENTERA por su firma. Las llaves se cuentan con un
    mini-lexer que salta cadenas, caracteres y comentarios: si no, las llaves
    de un formato JSON dentro de un literal ("{\"op\"...") descuadran la
    cuenta y la extraccion se lleva medio fichero (o se sale del array)."""
    # La firma puede venir partida en varias lineas: se compara por tokens.
    patron = r"\s*".join(re.escape(t) for t in firma.split()) + r"\s*\n\{"
    m = re.search(patron, src)
    if not m: raise SystemExit("no encuentro en el fuente: " + firma)
    i = m.start(); k = src.index("{", m.start()); n = 0
    while k < len(src):
        c = src[k]
        if c == "/" and src[k+1:k+2] == "/":
            k = src.find(chr(10), k)
            k = len(src) if k < 0 else k
            continue
        if c == "/" and src[k+1:k+2] == "*":
            k = src.find("*/", k)
            k = len(src) if k < 0 else k + 2
            continue
        if c == chr(34) or c == COMILLA:
            q = c
            k += 1
            while k < len(src) and src[k] != q:
                k += 2 if src[k] == chr(92) else 1
            k += 1
            continue
        if c == "{": n += 1
        elif c == "}":
            n -= 1
            if n == 0: return src[i:k+1]
        k += 1
    raise SystemExit("llaves sin cerrar extrayendo: " + firma)
'

# ══════════════════════════════════════════════════════════════════════════════
# PARTE 1 — CABINA: el cuerpo JSON del apunte
# ══════════════════════════════════════════════════════════════════════════════
if [ -f main/net/apunte.c ]; then
    echo "=== cabina: cuerpo JSON del apunte (main/net/apunte.c) ==="
    python3 - "$TMP" <<PY || exit 2
$extrae_py
import pathlib
v = pathlib.Path("main/net/apunte.c").read_text()
# El escapador: en el firmware grabado (v1.30 / v2.43) se llama json_str y solo
# escapa comilla y barra; el arreglo del 17-oct-2026 lo cambia por json_esc_en.
# El test vale para los dos: se extrae el que haya.
opcional = []
for firma in ("static size_t json_esc_en(char *out, size_t n, size_t j, const char *in)",
              "static void json_str(char *out, size_t n, const char *in)"):
    try:
        opcional.append(extrae(v, firma)); break
    except SystemExit:
        pass
# El escapador nuevo usa utf8_largo() para validar las secuencias: si esta en el
# fuente, se extrae tambien (si no, el enlazador se queja y el test no probaria
# nada). El firmware viejo no la tiene y este bucle no la encuentra: no pasa nada.
try:
    opcional.insert(0, extrae(v, "static size_t utf8_largo(const unsigned char *p)"))
except SystemExit:
    pass
piezas = opcional + [
    extrae(v, "size_t apunte_cabecera(char *out, size_t n, uint32_t id, const char *tipo)"),
    extrae(v, "size_t apunte_campo_txt(char *out, size_t n, size_t used, const char *clave, const char *valor)"),
    extrae(v, "size_t apunte_campo_num(char *out, size_t n, size_t used, const char *clave, long valor)"),
    extrae(v, "size_t apunte_cerrar(char *out, size_t n, size_t used, const char *resumen)"),
]
pathlib.Path("$TMP/piezas.c").write_text("\n\n".join(piezas) + "\n")
PY

    cat > "$TMP/harness.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/* Lo UNICO que se sustituye del entorno: el reloj (la hora del apunte). */
static bool reloj_ahora(uint32_t *ts) { *ts = 1700000000u; return true; }

/*__PIEZAS__*/

/* El tope real del cuerpo: el mismo que usa viaje_cola.h (896). Se lee de ahi
 * para que el test no pueda quedarse con un numero viejo. */
#include "viaje_cola_num.h"

struct caso { const char *que; const char *bytes; size_t len; };

static void escribe(const char *ruta, const char *b, size_t n)
{
    FILE *f = fopen(ruta, "wb");
    if (!f) { perror(ruta); exit(2); }
    fwrite(b, 1, n, f);
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    const char *dir = argv[1];
    char ruta_in[512], ruta_body[512];

    /* (1) TEXTO LIBRE: el campo "nota" del apunte. */
    /* 'esp' es lo que DEBE salir cuando el texto tiene bytes que no son UTF-8
     * validos: el escapador los sustituye por '?' (RFC 8259 exige UTF-8), asi
     * que la vuelta no puede ser identica al original y hay que decirlo aqui.
     * NULL = la vuelta tiene que ser identica. */
    static const struct { const char *que; const char *txt; const char *esp; } T[] = {
        {"texto normal",            "Cambio de filtro"},
        {"comilla doble",           "Cambio \"filtro\" y aceite"},
        {"barra invertida",         "C:\\ruta\\filtro"},
        {"las dos: \\ y \"",         "a\\b\"c"},
        {"salto de linea",          "linea1\nlinea2"},
        {"retorno de carro",        "a\rb"},
        {"tabulador",               "a\tb"},
        {"control 0x01",            "a\x01b"},
        {"control 0x1f",            "a\x1f" "b"},
        {"DEL 0x7f",                "a\x7f" "b"},
        {"acento y enie",           "C\xC3\xA1" "diz Catalu\xC3\xB1" "a"},
        {"signos raros",            "!\"$%&/()=?*+-.,;:|<>[]{}^~#'@`\\"},
        {"emoji 4 bytes",           "ok \xF0\x9F\x98\x80"},
        {"bandera (8 bytes)",       "\xF0\x9F\x87\xAA\xF0\x9F\x87\xB8"},
        {"CJK (ancho)",             "\xE6\x97\xA5\xE6\x9C\xAC"},
        {"UTF-8 truncado (0xC3)",   "abc\xC3",              "abc?"},
        {"UTF-8 truncado (0xE6 0x97)", "abc\xE6\x97",      "abc?"},
        {"byte invalido 0xFF",      "abc\xFF",              "abc?"},
        {"parece JSON",             "{\"a\":1}"},
        {"traversal /",             "../../etc/passwd"},
        {"traversal \\",            "..\\..\\windows"},
        {"comilla simple",          "d'Angelo"},
        {"solo espacios",           "   "},
        {"vacio",                   ""},
        {"40 caracteres (tope)",    "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"},
        {"60 caracteres",           "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"},
        {"61 caracteres",           "CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"},
        {"62 caracteres",           "DDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDDD"},
        {"muchas comillas (escape crece)", "\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\"\""},
        {"muchos controles (x6)",   "\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f"},
    };
    const int NT = (int)(sizeof(T) / sizeof(T[0]));
    for (int i = 0; i < NT; i++) {
        snprintf(ruta_in, sizeof(ruta_in), "%s/c%02d.in", dir, i);
        snprintf(ruta_body, sizeof(ruta_body), "%s/c%02d.body", dir, i);
        if (!T[i].txt) { escribe(ruta_in, "", 0); escribe(ruta_body, "", 0); continue; }
        char b[CUERPO_MAX];
        size_t u = apunte_cabecera(b, sizeof(b), 0x01000007u, "repostaje");
        u = apunte_campo_txt(b, sizeof(b), u, "nota", T[i].txt);
        u = apunte_campo_num(b, sizeof(b), u, "n", 7);
        u = apunte_cerrar(b, sizeof(b), u, T[i].txt);
        escribe(ruta_in, T[i].txt, strlen(T[i].txt));
        if (T[i].esp) {
            snprintf(ruta_body, sizeof(ruta_body), "%s/c%02d.esp", dir, i);
            escribe(ruta_body, T[i].esp, strlen(T[i].esp));
        }
        if (u == 0) { escribe(ruta_body, "", 0); continue; }   /* no entro */
        escribe(ruta_body, b, u);
    }
    /* manifiesto: que caso es cual */
    snprintf(ruta_body, sizeof(ruta_body), "%s/nombres.txt", dir);
    FILE *fn = fopen(ruta_body, "w");
    for (int i = 0; i < NT; i++) fprintf(fn, "%02d|%s\n", i, T[i].que);
    fclose(fn);

    /* (2) EL RESTO DE CAMPOS: numeros enormes, vacios, etc. (apunte_campo_num
     * y apunte_campo_txt con los topes REALES de los formularios). */
    static const char *const NOMBRES[] = {
        "vacio", "solo_punto", "punto_punto", "cero_cero", "cero_coma_coma",
        "diez_digitos", "nueve_digitos", "seis_digitos", "exponente", "menos_cero",
        "coma_decimal", "espacio_delante", "espacio_detras", "espacio_medio",
        "solo_espacios", "cero_delante", "punto_final", "cuatro_puntos",
    };
    static const char *const VALORES[] = {
        "", ".", "..", "00", "0.0", "9999999999", "999999999", "999999",
        "1e9", "-0", "12,50", " 12", "12 ", "1 2", "   ", "007", "5.", "1.2.3",
    };
    const int NV = (int)(sizeof(NOMBRES) / sizeof(NOMBRES[0]));
    for (int i = 0; i < NV; i++) {
        snprintf(ruta_in, sizeof(ruta_in), "%s/n%02d.in", dir, i);
        snprintf(ruta_body, sizeof(ruta_body), "%s/n%02d.body", dir, i);
        char b[CUERPO_MAX];
        size_t u = apunte_cabecera(b, sizeof(b), 0x01000007u, "repostaje");
        u = apunte_campo_txt(b, sizeof(b), u, "importe", VALORES[i]);
        u = apunte_cerrar(b, sizeof(b), u, VALORES[i]);
        escribe(ruta_in, VALORES[i], strlen(VALORES[i]));
        if (u == 0) { escribe(ruta_body, "", 0); continue; }
        escribe(ruta_body, b, u);
    }
    snprintf(ruta_body, sizeof(ruta_body), "%s/nombres_num.txt", dir);
    fn = fopen(ruta_body, "w");
    for (int i = 0; i < NV; i++) fprintf(fn, "%02d|%s\n", i, NOMBRES[i]);
    fclose(fn);

    /* (3) ROCE DE CUERPO_MAX: un texto largo de verdad, con caracteres que
     * crecen al escapar. Tiene que decir "no cabe" ANTES de escribir un cuerpo
     * cortado, o caber y seguir siendo JSON valido. */
    {
        static char grande[4096];
        size_t k = 0;
        for (int i = 0; i < 1200; i++) grande[k++] = (i % 7 == 0) ? '"' : 'x';
        grande[k] = 0;
        for (int rep = 0; rep < 3; rep++) {
            char b[CUERPO_MAX];
            size_t u = apunte_cabecera(b, sizeof(b), 0x01000007u, "pernocta");
            u = apunte_campo_txt(b, sizeof(b), u, "importe", "123.45");
            u = apunte_campo_txt(b, sizeof(b), u, "nota", grande);
            u = apunte_cerrar(b, sizeof(b), u, grande);
            snprintf(ruta_in, sizeof(ruta_in), "%s/g%02d.in", dir, rep);
            snprintf(ruta_body, sizeof(ruta_body), "%s/g%02d.body", dir, rep);
            escribe(ruta_in, grande, strlen(grande));
            if (u == 0) { escribe(ruta_body, "", 0); continue; }
            escribe(ruta_body, b, u);
        }
        snprintf(ruta_body, sizeof(ruta_body), "%s/nombres_grande.txt", dir);
        fn = fopen(ruta_body, "w");
        fprintf(fn, "00|texto de 1200 con comillas\n");
        fprintf(fn, "01|texto de 1200 con comillas (2)\n");
        fprintf(fn, "02|texto de 1200 con comillas (3)\n");
        fclose(fn);
    }
    return 0;
}
EOF

    # El tope del cuerpo, leido de su unico dueno (viaje_cola.h).
    awk '/#define +CUERPO_MAX/ {print "#define CUERPO_MAX " $3}' main/net/viaje_cola.h > "$TMP/viaje_cola_num.h"
    sed -i 's/#include "viaje_cola_num.h"/#include "viaje_cola_num.h"/' "$TMP/harness.c"
    sed 's|/\*__PIEZAS__\*/|#include "piezas.c"|' "$TMP/harness.c" > "$TMP/test.c"

    if ! gcc -std=c11 -Wall -Wextra -O1 -I"$TMP" -o "$TMP/t" "$TMP/test.c" 2> "$TMP/gcc.log"; then
        echo "no compila el arnes:"; sed -n '1,40p' "$TMP/gcc.log"; exit 2
    fi
    mkdir -p "$TMP/casos"
    ( cd "$TMP" && ./t "$TMP/casos" ) || { echo "el arnes fallo al ejecutarse"; exit 2; }

    # Validador: JSON valido + el texto vuelve IDENTICO.
    cat > "$TMP/valida.py" <<'EOF'
import json, sys, pathlib, glob
casos = pathlib.Path(sys.argv[1])
fallos = 0; pruebas = 0
def comprueba(nombres, prefijo, clave, campo):
    global fallos, pruebas
    for linea in pathlib.Path(casos / nombres).read_text().splitlines():
        if not linea.strip(): continue
        idx, que = linea.split("|", 1)
        ent = (casos / f"{prefijo}{idx}.in").read_bytes()
        cue = (casos / f"{prefijo}{idx}.body").read_bytes()
        pruebas += 1
        if cue == b"":
            # el fuente dice "no cabe": solo vale para textos de verdad grandes
            if len(ent) < 300:
                print(f"FALLO  {que}: el cuerpo NO se pudo montar y el texto era corto ({len(ent)} B)")
                fallos += 1
            else:
                print(f"ok     {que}: no cabe -> se dice (no se manda un JSON cortado)")
            continue
        # 1) el cuerpo tiene que ser UTF-8 valido
        try:
            texto = cue.decode("utf-8")
        except UnicodeDecodeError as e:
            print(f"FALLO  {que}: el cuerpo NO es UTF-8 valido ({e})")
            fallos += 1
            continue
        # 2) y JSON valido
        try:
            j = json.loads(texto)
        except Exception as e:
            print(f"FALLO  {que}: el cuerpo NO es JSON valido: {e}")
            print(f"        cuerpo: {texto[:160]!r}")
            fallos += 1
            continue
        # 3) el texto tiene que volver byte a byte
        try:
            if campo == "datos":
                vuelta = j["datos"][clave].encode("utf-8")
            else:
                vuelta = j[clave].encode("utf-8")
        except KeyError as e:
            print(f"FALLO  {que}: falta la clave {e} en el cuerpo")
            fallos += 1
            continue
        esp_f = casos / f"{prefijo}{idx}.esp"
        esperado = esp_f.read_bytes() if esp_f.exists() else ent
        if vuelta != esperado:
            print(f"FALLO  {que}: el texto NO vuelve igual ({len(ent)} B -> {len(vuelta)} B)")
            print(f"        metido: {ent[:80]!r}")
            print(f"        vuelto: {vuelta[:80]!r}")
            fallos += 1
        else:
            print(f"ok     {que}: JSON valido, texto intacto ({len(ent)} B)")
comprueba("nombres.txt", "c", "nota", "datos")
comprueba("nombres_num.txt", "n", "importe", "datos")
comprueba("nombres_grande.txt", "g", "nota", "datos")
print(f"\n{pruebas} pruebas, {fallos} fallos")
sys.exit(1 if fallos else 0)
EOF
    python3 "$TMP/valida.py" "$TMP/casos" | tee "$TMP/salida.txt"
    grep -q "^FALLO" "$TMP/salida.txt" && fallos=$((fallos+1))
    pruebas=$((pruebas+1))
    echo
fi

# ══════════════════════════════════════════════════════════════════════════════
# PARTE 2 — P4: el nombre de la carpeta y los CSV
# ══════════════════════════════════════════════════════════════════════════════
if [ -f main/portal/config_server_viaje.c ]; then
    echo "=== P4: nombre de carpeta y CSV (main/portal/config_server_viaje.c) ==="
    IDF="${IDF_PATH:-}"
    for c in "$IDF" "$HOME/.espressif/esp-idf-5.5" /opt/esp/idf; do
        [ -n "$c" ] && [ -f "$c/components/json/cJSON/cJSON.c" ] && IDF="$c" && break
        IDF=""
    done
    if [ -z "$IDF" ]; then
        echo "  SKIP: no encuentro el cJSON del IDF (define IDF_PATH). Esta parte NO se ha probado."
        fallos=$((fallos+1)); pruebas=$((pruebas+1))
    else
        python3 - "$TMP" "$IDF" <<PY || exit 2
$extrae_py
import pathlib, sys
v = pathlib.Path("main/portal/config_server_viaje.c").read_text()
piezas = [
    extrae(v, "static void destino_seguro(const char *in, char *out, size_t n)"),
    # La sustitucion de un caracter de CSV vive en csv_car() y la comparten la
    # FILA (csv_texto) y la CABECERA (csv_texto_a): se extraen las tres, o el
    # test estaria probando media sustitucion (D3, 17-oct-2026). Si alguien
    # vuelve a juntarlas en una sola funcion, esta lista se cae y hay que
    # ajustarla, que es justo lo que se quiere notar.
    extrae(v, "static char csv_car(char c)"),
    extrae(v, "static void csv_texto(FILE *f, const char *v)"),
    extrae(v, "static size_t csv_texto_a(char *out, size_t n, const char *v)"),
    extrae(v, "static bool csv_por_tipo(const char *carpeta, const char *tipo, const cJSON *datos, const char *cuando)"),
]
pathlib.Path("$TMP/piezas_p4.c").write_text("\n\n".join(piezas) + "\n")
PY
        # Las medidas de la P4, leidas de su fuente (no copiadas a mano).
        { grep -E "^#define (CARPETA_MAX|RUTA_MAX|DESTINO_MAX) " main/portal/config_server_viaje.c || true; } > "$TMP/medidas_p4.h"
        cat >> "$TMP/medidas_p4.h" <<'EOF2'
/*__FIN__*/
EOF2
        cat > "$TMP/p4.c" <<'EOF'
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "cJSON.h"
#include "medidas_p4.h"

static const char *TAG = "test";
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
static void anota_escritura_ok(void) {}

/*__PIEZAS__*/

static int fallos = 0, pruebas = 0;
static void carpeta(const char *que, const char *in, const char *esperado)
{
    char out[256];
    destino_seguro(in, out, DESTINO_MAX);   /* como op_inicio(), no con sizeof(out) */
    pruebas++;
    if (strcmp(out, esperado)) {
        printf("FALLO  %-34s %-24s -> '%s' (esperado '%s')\n", que, in, out, esperado);
        fallos++;
    } else {
        printf("ok     %-34s %-24s -> '%s'\n", que, in, out);
    }
}
static void csv(const char *que, const char *in, const char *esperado)
{
    char *buf = NULL; size_t n = 0;
    FILE *m = open_memstream(&buf, &n);
    csv_texto(m, in);
    fclose(m);
    pruebas++;
    if (!buf || strcmp(buf, esperado)) {
        printf("FALLO  csv_texto %-22s '%s' -> '%s' (esperado '%s')\n", que, in, buf ? buf : "(null)", esperado);
        fallos++;
    } else {
        printf("ok     csv_texto %-22s -> '%s'\n", que, buf);
    }
    free(buf);
}

/* csv_por_tipo() de verdad, sobre un fichero temporal: se comprueba que la
 * cabecera y la fila tengan las MISMAS columnas aunque la clave traiga una coma
 * o un salto de linea. */
static void csv_tipo(const char *que, const char *json, int columnas_esperadas, int filas_esperadas)
{
    char dir[] = "/tmp/entrada_csvXXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); exit(2); }
    cJSON *j = cJSON_Parse(json);
    if (!j) { printf("FALLO  %s: JSON de prueba invalido\n", que); fallos++; pruebas++; return; }
    const cJSON *datos = cJSON_GetObjectItemCaseSensitive(j, "datos");
    bool okw = csv_por_tipo(dir, "prueba", datos, "2026-10-17 10:00:00");
    char ruta[512]; snprintf(ruta, sizeof(ruta), "%s/pruebas.csv", dir);
    FILE *f = fopen(ruta, "r");
    pruebas++;
    if (!okw || !f) { printf("FALLO  csv_por_tipo %s: no escribio\n", que); fallos++; if (f) fclose(f); return; }
    char linea[1024]; int filas = 0; int cols_cab = -1, cols_fila = -1;
    while (fgets(linea, sizeof(linea), f)) {
        linea[strcspn(linea, "\n")] = 0;
        int c = 1; for (char *p = linea; *p; p++) if (*p == ',') c++;
        if (filas == 0) cols_cab = c; else cols_fila = c;
        if (filas < 3) printf("        [%d] %s\n", filas, linea);
        filas++;
    }
    fclose(f);
    if (filas != filas_esperadas || cols_cab != columnas_esperadas || cols_fila != cols_cab) {
        printf("FALLO  csv_por_tipo %-28s: %d filas, cabecera %d cols, fila %d cols (esperado %d filas, %d cols)\n",
               que, filas, cols_cab, cols_fila, filas_esperadas, columnas_esperadas);
        fallos++;
    } else {
        printf("ok     csv_por_tipo %-28s: %d fila(s), %d columnas cuadran\n", que, filas, cols_cab);
    }
    cJSON_Delete(j);
    char cmd[600]; snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir); if (system(cmd)) {}
}

int main(void)
{
    puts("-- destino_seguro(): el nombre de la CARPETA --");
    carpeta("texto normal",        "RIBADEO",            "RIBADEO");
    carpeta("espacio simple",      "Rio Tinto",          "Rio_Tinto");
    carpeta("espacio doble",       "Rio  Tinto",         "Rio_Tinto");
    /* EL GUION TAMBIEN ES '_' (D4, 17-oct-2026). El criterio bueno es el de la
     * cabina (cabina5/main/salida.c, sanear()): es ella la que compone el nombre
     * que ve el usuario y la que manda el destino al empezar el viaje, asi que la
     * P4 tiene que sacar el MISMO nombre de carpeta. Antes aqui se conservaba el
     * guion: el mismo viaje era "Casa_Beach" en la pantalla y "Casa-Beach" en la
     * tarjeta (10 de 21 entradas de la muestra divergian), y quedaban carpetas
     * acabadas en '-'. */
    carpeta("guion",               "Casa-Beach",         "Casa_Beach");
    carpeta("guion doble",         "A--B",               "A_B");
    carpeta("guion y espacio",     "Casa - Beach",       "Casa_Beach");
    carpeta("guion final",         "Casa-",              "Casa");
    carpeta("guion bajo final",    "Casa_",              "Casa");
    carpeta("solo guiones",        "---",                "viaje");
    carpeta("espacios alrededor",  "  Casa  ",           "Casa");
    carpeta("solo espacios",       "    ",               "viaje");
    carpeta("vacio",               "",                   "viaje");
    carpeta("acento",              "C\xC3\xA1" "diz",   "Cadiz");
    carpeta("enie",                "Catalu\xC3\xB1" "a", "Cataluna");
    carpeta("e acentuada (en tabla)",  "B\xC3\xA9" "ziers", "Beziers");
    carpeta("e cirfleja (fuera de tabla)", "B\xC3\xAA" "ziers", "Bziers");
    carpeta("emoji",               "Casa \xF0\x9F\x98\x80", "Casa");
    carpeta("solo emoji",          "\xF0\x9F\x98\x80",   "viaje");
    carpeta("CJK",                 "\xE6\x97\xA5\xE6\x9C\xAC", "viaje");
    carpeta("UTF-8 truncado",      "abc\xC3",            "abc");
    carpeta("byte suelto 0xFF",    "abc\xFF",            "abc");
    carpeta("barras y puntos",     "../../etc/passwd",   "etcpasswd");
    carpeta("barras invertidas",   "..\\..\\windows",    "windows");
    carpeta("caracteres FAT",      "a:b*c?d\"e<f>g|h",   "abcdefgh");
    carpeta("punto final",         "Casa.",              "Casa");
    carpeta("espacio final",       "Casa ",              "Casa");
    carpeta("reservado Windows",   "CON",                "CON");
    carpeta("reservado + punto",   "NUL.",               "NUL");
    carpeta("comilla simple",      "d'Angelo",           "dAngelo");
    carpeta("tilde y simbolos",    "A\xC2\xB0" "B",      "AB");
    carpeta("23 caracteres",       "12345678901234567890123", "12345678901234567890123");
    carpeta("24 caracteres",       "123456789012345678901234", "12345678901234567890123");

    puts("-- csv_texto(): lo que llega al CSV --");
    csv("texto normal",     "Cambio de filtro",              "Cambio de filtro");
    csv("coma",             "aceite, correa",                "aceite; correa");
    csv("comilla doble",    "filtro \"nuevo\"",              "filtro 'nuevo'");
    csv("salto de linea",   "aceite\ncorrea",                "aceite correa");
    csv("retorno",          "aceite\rcorrea",                "aceite correa");
    csv("punto y coma",     "aceite; correa",                "aceite; correa");
    csv("tabulador",        "a\tb",                          "a\tb");
    csv("acentos",          "C\xC3\xA1" "diz",            "C\xC3\xA1" "diz");

    puts("-- csv_por_tipo(): cabecera y fila --");
    csv_tipo("claves normales", "{\"datos\":{\"importe\":\"12.5\",\"km\":\"1234\"}}", 3, 2);
    csv_tipo("clave con COMA",  "{\"datos\":{\"a,b\":\"1\",\"c\":\"2\"}}",           3, 2);
    csv_tipo("clave con SALTO", "{\"datos\":{\"a\\nb\":\"1\",\"c\":\"2\"}}",         3, 2);
    csv_tipo("valor con coma",  "{\"datos\":{\"nota\":\"uno,dos\",\"c\":\"2\"}}",    3, 2);

    printf("\n%d pruebas, %d fallos\n", pruebas, fallos);
    return fallos ? 1 : 0;
}
EOF
        sed 's|/\*__PIEZAS__\*/|#include "piezas_p4.c"|' "$TMP/p4.c" > "$TMP/p4_test.c"
        if gcc -std=gnu11 -Wall -Wextra -O1 -I"$TMP" -I"$IDF/components/json/cJSON" \
               -o "$TMP/p4" "$TMP/p4_test.c" "$IDF/components/json/cJSON/cJSON.c" 2> "$TMP/gcc_p4.log"; then
            "$TMP/p4" | tee "$TMP/salida_p4.txt"
            grep -q "^FALLO" "$TMP/salida_p4.txt" && fallos=$((fallos+1))
            pruebas=$((pruebas+1))
        else
            echo "no compila el arnes de la P4:"; sed -n '1,30p' "$TMP/gcc_p4.log"
            fallos=$((fallos+1)); pruebas=$((pruebas+1))
        fi
    fi
    echo
fi

if [ "$pruebas" = 0 ]; then
    echo "No hay nada que probar aqui (no encuentro main/net/apunte.c ni main/portal/config_server_viaje.c)."
    exit 2
fi
if [ "$fallos" != 0 ]; then
    echo "ENTRADA HOSTIL: hay $fallos bloque(s) con fallos. Ver arriba."
    exit 1
fi
echo "ENTRADA HOSTIL: todo lo que se puede probar en host, correcto."
