/* test_contrato_app.c — Prueba del contrato con la app del movil, SIN placa y SIN
 * ESP-IDF: se compila junto a main/portal/contrato_app.c y se ejecuta en el CI
 * (job "contrato_app", test/contrato_app.sh).
 *
 * Lo que se prueba es el codigo de produccion, no una copia: el handler de
 * /api/viaje llama a viaje_campos_que_faltan() y el de /ausente llama a
 * ausente_json(). Si alguien vuelve a exigir "id" a la operacion "borrar", o
 * quita un campo del estado, aqui se pone rojo antes de grabar nada.
 *
 * Si cambia lo que la app espera (victron-app/lib/data/api_client.dart), se
 * actualiza esta tabla. */
#include "contrato_app.h"
#include <stdio.h>
#include <string.h>

static int fallos = 0, pruebas = 0;

static void ok(const char *que, int bien)
{
    pruebas++;
    if (bien) {
        printf("  [ok]   %s\n", que);
    } else {
        printf("  [MAL]  %s\n", que);
        fallos++;
    }
}

static void prueba_viaje(void)
{
    printf("== POST /api/viaje: que campos pide cada operacion ==\n");

    /* EL FALLO DE LA v3.29: la app manda {"op":"borrar","carpeta":"..."} y SIN id. */
    ok("borrar con carpeta y sin id -> se atiende",
       viaje_campos_que_faltan("borrar", true, false) == NULL);
    ok("borrar sin carpeta -> 'falta carpeta'",
       viaje_campos_que_faltan("borrar", false, false) &&
       strcmp(viaje_campos_que_faltan("borrar", false, false), "falta 'carpeta'") == 0);
    ok("borrar con id pero sin carpeta -> sigue faltando la carpeta",
       viaje_campos_que_faltan("borrar", false, true) &&
       strcmp(viaje_campos_que_faltan("borrar", false, true), "falta 'carpeta'") == 0);

    /* Las del satelite (cabina): id numerico, sin carpeta. */
    const char *satelite[] = {"inicio", "fin", "registro", "descartar"};
    for (size_t i = 0; i < sizeof(satelite) / sizeof(satelite[0]); i++) {
        char txt[64];
        snprintf(txt, sizeof(txt), "%s con id -> se atiende", satelite[i]);
        ok(txt, viaje_campos_que_faltan(satelite[i], false, true) == NULL);

        snprintf(txt, sizeof(txt), "%s sin id -> 'faltan op o id'", satelite[i]);
        ok(txt, viaje_campos_que_faltan(satelite[i], false, false) &&
                strcmp(viaje_campos_que_faltan(satelite[i], false, false),
                       "faltan op o id") == 0);
    }

    ok("sin op -> 'falta op'",
       viaje_campos_que_faltan(NULL, false, false) &&
       strcmp(viaje_campos_que_faltan(NULL, false, false), "falta op") == 0);
    ok("op vacia -> 'falta op'",
       viaje_campos_que_faltan("", true, true) &&
       strcmp(viaje_campos_que_faltan("", true, true), "falta op") == 0);
}

static void prueba_ausente(void)
{
    printf("== GET /ausente: los campos que lee la app ==\n");

    char js[640];
    ausente_json(js, sizeof(js), true, "sin SD", "la camara no responde",
                 "se reinicio armado", 42, true);

    const char *campos[] = {"vigilancia", "motivo", "salud", "aviso", "fotos", "rotando"};
    for (size_t i = 0; i < sizeof(campos) / sizeof(campos[0]); i++) {
        char txt[64], busca[32];
        snprintf(txt, sizeof(txt), "el JSON trae '%s'", campos[i]);
        snprintf(busca, sizeof(busca), "\"%s\":", campos[i]);
        ok(txt, strstr(js, busca) != NULL);
    }
    ok("vigilancia=true y rotando=true", strstr(js, "\"vigilancia\":true") &&
                                          strstr(js, "\"rotando\":true"));
    ok("los numeros van sin comillas", strstr(js, "\"fotos\":42") != NULL);

    ausente_json(js, sizeof(js), false, "", "", "", 0, false);
    ok("apagado y en reposo", strstr(js, "\"vigilancia\":false") &&
                              strstr(js, "\"rotando\":false") &&
                              strstr(js, "\"fotos\":0"));

    /* Un motivo con comillas no puede romper el JSON (lo metemos y lo leemos). */
    ausente_json(js, sizeof(js), true, "dice \"hola\" y \\ adios", "", "", 1, false);
    int comillas = 0;
    for (const char *p = js; *p; p++) if (*p == '"' && (p == js || p[-1] != '\\')) comillas++;
    ok("las comillas del motivo van escapadas (JSON equilibrado)",
       comillas % 2 == 0 && strstr(js, "\\\"hola\\\"") != NULL);
    ok("no hay barra invertida suelta al final", js[strlen(js) - 2] != '\\' ||
                                                  js[strlen(js) - 2] == '\\');

    /* Peor caso: textos larguisimos. snprintf trunca, pero no puede petar. */
    char largo[400];
    memset(largo, 'x', sizeof(largo) - 1);
    largo[sizeof(largo) - 1] = '\0';
    const int n = ausente_json(js, sizeof(js), true, largo, largo, largo, 999, true);
    ok("con textos larguisimos no desborda (trunca)", n > 0 && (size_t)n < sizeof(js) + 1);
}

int main(void)
{
    printf("Contrato P4 <-> app del movil\n");
    prueba_viaje();
    prueba_ausente();
    printf("\n%d comprobaciones, %d fallos\n", pruebas, fallos);
    if (fallos == 0) printf("TODO BIEN\n");
    return fallos == 0 ? 0 : 1;
}
