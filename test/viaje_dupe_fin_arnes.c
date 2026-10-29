/* Arnes del CHEQUEO DE 'fin' REPETIDO (M4, auditoria del 27-oct-2026).
 *
 * POR QUE ESTE TEST. El cierre de un viaje se protegia de repetirse mirando si
 * existia resumen.txt, pero la linea 'fin' del diario se escribe ANTES que el
 * resumen: un cierre que se quedo a medias entre las dos dejaba 'fin' escrito y
 * ningun resumen, y cada reintento (la cola de la cabina lo manda cada 15 s)
 * añadia otra linea 'fin' al eventos.csv. Ahora, antes de escribir, se mira la
 * ultima fila del diario (diario_ultima_es()).
 *
 * Como el resto de tests de este repo, NO se copia la funcion: se extrae del
 * fuente de verdad (test/test_viaje_dupe_fin.sh) y se compila aqui. Lo unico
 * que se pone es lo de fuera: RUTA_MAX y los ESP_LOG*, que aqui no dicen nada.
 *
 * Lo que se comprueba:
 *   - que un diario que acaba en 'fin' se reconoce (y no se reescribe);
 *   - que un 'fin' SEGUIDO de otra fila NO cuenta (el viaje siguio: el cierre
 *     nuevo si debe escribirse);
 *   - que una fila partida por un corte de corriente no tapa el 'fin' de antes;
 *   - que un fichero sin filas utiles (vacio, solo cabecera) no da 'fin';
 *   - y que el 'detalle' con comas no confunde la columna del evento. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stddef.h>

#define RUTA_MAX 320

__FN__

static int fallos = 0;

/* Deja 'carpeta' con el eventos.csv que se le pase (o sin fichero si txt==NULL). */
static void escribe(const char *carpeta, const char *txt)
{
    char ruta[RUTA_MAX];
    snprintf(ruta, sizeof(ruta), "%s/eventos.csv", carpeta);
    if (!txt) { remove(ruta); return; }
    FILE *f = fopen(ruta, "w");
    if (!f) { printf("FALLO no puedo crear %s\n", ruta); fallos = 1; return; }
    fputs(txt, f);
    fclose(f);
}

static void comprueba(const char *carpeta, const char *evento,
                      bool esperado, const char *titulo)
{
    bool got = diario_ultima_es(carpeta, evento);
    if (got == esperado) {
        printf("ok     %-42s -> %s\n", titulo, got ? "si" : "no");
    } else {
        printf("FALLO  %-42s -> %s (esperado %s)\n", titulo,
               got ? "si" : "no", esperado ? "si" : "no");
        fallos = 1;
    }
}

int main(void)
{
    const char *dir = "/tmp/viaje_dupe_fin";
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s && mkdir -p %s", dir, dir);
    if (system(cmd) != 0) { printf("FALLO no puedo preparar %s\n", dir); return 1; }

    /* 1. Un viaje cerrado de verdad: la ultima fila es el fin. */
    escribe(dir, "fecha_hora,evento,detalle\n"
                 "2026-10-27 10:00:00,inicio,\n"
                 "2026-10-27 12:00:00,repostaje,60.0\n"
                 "2026-10-27 18:30:00,fin,\n");
    comprueba(dir, "fin", true, "diario que acaba en fin");

    /* 2. El fin esta, pero despues entro otra cosa: el cierre nuevo SI va. */
    escribe(dir, "fecha_hora,evento,detalle\n"
                 "2026-10-27 18:30:00,fin,\n"
                 "2026-10-27 18:35:00,telemetria,12.4\n");
    comprueba(dir, "fin", false, "fin con telemetria detras");

    /* 3. Fila partida por un corte de corriente: vale el fin de antes. */
    escribe(dir, "fecha_hora,evento,detalle\n"
                 "2026-10-27 18:30:00,fin,\n"
                 "2026-10-27 18:35:00,teleme");
    comprueba(dir, "fin", true, "fila partida al final");

    /* 4. Solo la cabecera: no hay fin. */
    escribe(dir, "fecha_hora,evento,detalle\n");
    comprueba(dir, "fin", false, "solo la cabecera");

    /* 5. Fichero vacio y fichero que no existe. */
    escribe(dir, "");
    comprueba(dir, "fin", false, "diario vacio");
    escribe(dir, NULL);
    comprueba(dir, "fin", false, "sin diario");

    /* 6. La columna que cuenta es la segunda: un detalle con comas no engaña. */
    escribe(dir, "fecha_hora,evento,detalle\n"
                 "2026-10-27 18:30:00,repostaje,\"60,5 litros, gasoil\"\n");
    comprueba(dir, "fin", false, "detalle con comas no es un fin");
    escribe(dir, "fecha_hora,evento,detalle\n"
                 "2026-10-27 18:30:00,nota,fin\n");
    comprueba(dir, "fin", false, "el evento va en la 2a columna, no la 3a");

    /* 7. Otra cosa no se confunde con el fin. */
    escribe(dir, "fecha_hora,evento,detalle\n"
                 "2026-10-27 18:30:00,fin_ajeno,\n");
    comprueba(dir, "fin", false, "'fin_ajeno' no es 'fin'");

    /* 8. Tambien sirve para preguntar por otro evento. */
    escribe(dir, "fecha_hora,evento,detalle\n"
                 "2026-10-27 10:00:00,inicio,\n");
    comprueba(dir, "inicio", true, "preguntando por 'inicio'");

    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    if (system(cmd) != 0) { /* da igual: es /tmp */ }

    printf("\n%s\n", fallos ? "HAY FALLOS" : "TODO OK: el fin no se repite");
    return fallos;
}
