/* Arnes del CIERRE DEL VIAJE: prueba publicar_resumen(), que es el paso que
 * decide si un viaje queda cerrado en la tarjeta (D0, 17-oct-2026).
 *
 * POR QUE ESTE TEST. El fallo era que rename() de FatFs NO pisa el destino: si
 * resumen.txt ya existia, el rename del .tmp fallaba y el viaje se quedaba sin
 * poder cerrar PARA SIEMPRE (507 en cada reintento, ni desde la cabina ni desde
 * la web ni desde el .tar). En el PC, rename() SI pisa, asi que un test con el
 * rename de libc pasaria con el codigo roto: aqui se SUSTITUYE rename() por uno
 * que se comporta como el de FatFs (falla con EEXIST si el destino existe).
 *
 * Lo que NO se hace es copiar la funcion: se extrae del fuente de verdad (ver
 * test/test_viaje_cierre.sh), asi que si alguien la cambia, el test cambia con
 * ella. Lo unico que se pone aqui es el entorno (TAG, los ESP_LOG* y el rename
 * de FatFs). */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>

/* El registro no interesa para lo que se prueba (y ensuciaria la salida). */
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)

/* TAG solo se usa dentro de los ESP_LOG*, que aqui se compilan a nada. */
static const char *TAG __attribute__((unused)) = "test_cierre";

/* FatFs: rename() NO pisa. Se define ANTES del #define para que esta funcion
 * llame al rename() de libc, y el #define afecte solo al codigo extraido. */
static int fat_rename(const char *de, const char *a)
{
    struct stat st;
    if (stat(a, &st) == 0) { errno = EEXIST; return -1; }
    return rename(de, a);
}
#define rename(de, a) fat_rename((de), (a))

/*__PIEZAS__*/

static int fallos = 0, pruebas = 0;

static void escribe(const char *ruta, const char *txt)
{
    FILE *f = fopen(ruta, "w");
    if (!f) { perror(ruta); fallos++; return; }
    fputs(txt, f);
    fclose(f);
}

static bool existe(const char *ruta)
{
    struct stat st;
    return stat(ruta, &st) == 0;
}

/* Primer cierre: el destino NO existe todavia. */
static void caso_primero(const char *dir)
{
    char tmp[512], dst[512];
    snprintf(tmp, sizeof(tmp), "%s/resumen.txt.tmp", dir);
    snprintf(dst, sizeof(dst), "%s/resumen.txt", dir);
    escribe(tmp, "RESUMEN NUEVO\n");
    bool ok = publicar_resumen(tmp, dst);
    pruebas++;
    if (!ok || !existe(dst) || existe(tmp)) {
        printf("FALLO  primer cierre: publicar=%d resumen=%d tmp=%d\n",
               (int)ok, (int)existe(dst), (int)existe(tmp));
        fallos++;
        return;
    }
    printf("ok     primer cierre: el resumen queda publicado y el .tmp no sobra\n");
}

/* EL CASO DEL FALLO: el destino YA existe (un cierre anterior no llego a
 * limpiar el estado, o el bloque de "faltan apuntes" lo creo). El rename de
 * FatFs fallaria; publicar_resumen() tiene que apartarlo y publicar el nuevo,
 * sin dejar .bak. */
static void caso_repetido(const char *dir)
{
    char tmp[512], dst[512], bak[600];
    snprintf(tmp, sizeof(tmp), "%s/resumen.txt.tmp", dir);
    snprintf(dst, sizeof(dst), "%s/resumen.txt", dir);
    snprintf(bak, sizeof(bak), "%s.bak", dst);
    escribe(tmp, "RESUMEN SEGUNDO INTENTO\n");
    bool ok = publicar_resumen(tmp, dst);
    pruebas++;
    char leido[128] = {0};
    FILE *f = fopen(dst, "r");
    if (f) { if (!fgets(leido, sizeof(leido), f)) leido[0] = 0; fclose(f); }
    if (!ok || strcmp(leido, "RESUMEN SEGUNDO INTENTO\n") != 0 ||
        existe(tmp) || existe(bak)) {
        printf("FALLO  cierre repetido: publicar=%d contenido='%s' tmp=%d bak=%d\n",
               (int)ok, leido, (int)existe(tmp), (int)existe(bak));
        fallos++;
        return;
    }
    printf("ok     cierre repetido: pisa el resumen anterior (el rename de FatFs "
           "a solas habria fallado)\n");
    escribe(tmp, "TERCERO\n");
    ok = publicar_resumen(tmp, dst);
    pruebas++;
    if (!ok) {
        printf("FALLO  tercer cierre seguido: publicar=0 (tiene que ser idempotente)\n");
        fallos++;
        return;
    }
    printf("ok     tercer cierre seguido: idempotente\n");
}

/* Si el .tmp NO esta (o no se puede poner), el resumen VIEJO no se puede
 * perder: el viaje seguiria cerrado con las cuentas de antes, que es un estado
 * valido, y el cierre se reintenta. */
static void caso_tmp_que_no_esta(const char *dir)
{
    char tmp[512], dst[512], bak[600];
    snprintf(tmp, sizeof(tmp), "%s/resumen.txt.tmp", dir);
    snprintf(dst, sizeof(dst), "%s/resumen.txt", dir);
    snprintf(bak, sizeof(bak), "%s.bak", dst);
    escribe(dst, "RESUMEN BUENO\n");
    /* A proposito: el .tmp no se crea (simula el .tmp perdido/ilegible). */
    bool ok = publicar_resumen(tmp, dst);
    pruebas++;
    char leido[128] = {0};
    FILE *f = fopen(dst, "r");
    if (f) { if (!fgets(leido, sizeof(leido), f)) leido[0] = 0; fclose(f); }
    if (ok || strcmp(leido, "RESUMEN BUENO\n") != 0) {
        printf("FALLO  .tmp perdido: publicar=%d contenido='%s' (el viejo tiene "
               "que seguir ahi)\n", (int)ok, leido);
        fallos++;
        return;
    }
    if (existe(bak)) {
        printf("FALLO  .tmp perdido: se ha quedado un .bak en la carpeta del viaje\n");
        fallos++;
        return;
    }
    printf("ok     .tmp perdido: el resumen viejo sigue entero y no queda .bak\n");
}

int main(void)
{
    char dir[] = "/tmp/viaje_cierreXXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 2; }

    /* META-COMPROBACION: el rename() de este arnes tiene que fallar cuando el
     * destino existe (como el de FatFs) o el test entero no valdria nada: con el
     * rename() de libc, que pisa, pasaria igual con el codigo ROTO. */
    {
        char a[512], b[512];
        snprintf(a, sizeof(a), "%s/meta_a", dir);
        snprintf(b, sizeof(b), "%s/meta_b", dir);
        escribe(a, "a\n");
        escribe(b, "b\n");
        pruebas++;
        if (rename(a, b) == 0) {
            printf("FALLO  meta: el rename() del arnes PISA el destino (tiene que "
                   "fallar como en FatFs, o este test no prueba nada)\n");
            fallos++;
        } else {
            printf("ok     meta: el rename() del arnes se comporta como FatFs "
                   "(no pisa)\n");
        }
        remove(a);
        remove(b);
    }

    puts("-- publicar_resumen(): el cierre tiene que ser IDEMPOTENTE --");
    caso_primero(dir);
    caso_repetido(dir);
    caso_tmp_que_no_esta(dir);

    char cmd[600];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) { /* da igual: es /tmp */ }

    printf("\n%d pruebas, %d fallos\n", pruebas, fallos);
    return fallos ? 1 : 0;
}
