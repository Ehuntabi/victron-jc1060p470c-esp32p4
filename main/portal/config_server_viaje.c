/* config_server_viaje.c — POST /api/viaje: los apuntes que manda el satelite 3.5"
 *
 * Diseño en 35cabina/docs/superpowers/specs/2026-08-22-viaje-con-nombre-y-carpeta.
 * Hechas las fases 1 (canal + inicio/fin) y 3 (los registros: repostaje, peaje,
 * bombona, mantenimiento y parada, cada uno a su CSV ademas del diario).
 *
 * Lo que cada apunte trae dentro y por que: el sello de tiempo lo pone EL
 * SATELITE, en el momento en que ocurrio, no la P4 al recibirlo -- un apunte
 * puede haber esperado dias en su cola.
 *
 * Por que HTTP y no UDP como la telemetria: por TCP se sabe con CERTEZA que se
 * entrego, y eso es lo que necesita una cola de pendientes. Con UDP habria que
 * reinventar confirmaciones y reintentos.
 *
 * Auth ESTRICTA: esto escribe en la tarjeta.
 *
 * Operaciones: inicio | fin | registro | descartar | borrar. Las cuatro primeras
 * llevan "id" numerico (el contador del satelite); "borrar" lleva "carpeta".
 *
 * "descartar" (23-ago-2026) APARTA el viaje abierto en vez de cerrarlo: su
 * carpeta pasa a DESCARTADO_<nombre> y deja de contar. Existe porque la 3.5"
 * puede encontrarse con que aqui hay un viaje abierto que ella no conoce, y el
 * unico aparato que puede cerrarlo es ella -- esta P4 vive en la parte de
 * atras de la autocaravana y no se conduce desde alli.
 */
#include "config_server_internal.h"
#include "config_server_auth.h"
#include "contrato_app.h"    /* viaje_campos_que_faltan: probado en el CI sin placa */
#include "camera.h"          /* camera_sd_bus_lock: serializar con el GDMA */
#include "datalogger.h"      /* datalogger_sd_montada: reparacion del viaje al arrancar */
#include "cJSON.h"
#include "data/dashboard_state.h"
#include "data/trip_computer.h"
#include "data/bombonas.h"
#include "gps/gps.h"
#include "frigo.h"
#include "ne185/ne185.h"
#include "watchdog.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>   /* rmdir: borrar la carpeta de un viaje; fsync, ftruncate */
#include <fcntl.h>    /* open(): deshacer una escritura a medias en un CSV */
#include <stdatomic.h>   /* _Atomic: la peticion de volcado desde otra tarea */

static const char *TAG = "viaje_srv";

/* Definida abajo, con el resto de la telemetria del viaje. Se declara aqui
 * porque el cierre del viaje (op_fin) tiene que bajar a la tarjeta los puntos
 * de ruta que aun estan en RAM antes de dar el viaje por terminado. */
static bool ruta_volcar(const char *carpeta);

/* Cierra la ruta del viaje: ultimo intento de volcado y, pase lo que pase, el
 * buffer de RAM se vacia (ver su comentario). Se declara aqui por lo mismo. */
static void ruta_cerrar(const char *carpeta);

/* Se llama al ABRIR viaje: ningun punto de un viaje anterior puede acabar en la
 * carpeta del nuevo. */
static void ruta_olvidar(void);

/* Atado a CUERPO_MAX de ~/joint/35cabina/main/net/viaje_cola.c (896): el
 * satelite no puede mandar mas de eso, con esto sobra margen. SI ALGUN DIA SE
 * SUBE ALLI, HAY QUE SUBIRLO AQUI TAMBIEN -- el job de CI "viaje_body_sync"
 * (.github/workflows/build.yml) falla si el satelite llega a igualar o
 * superar esto, para que no dependa de acordarse solo. */
#define VIAJE_BODY_MAX  1024

#define VIAJES_DIR      "/sdcard/viajes"
/* Donde van los apuntes que NO son de un viaje: el historial del vehiculo.
 * Repostajes de camino al taller, la ITV, una bombona... cosas que le pasan a
 * la autocaravana, no a un viaje concreto. */
#define VEHICULO_DIR    "/sdcard/vehiculo"
#define NVS_NS          "viaje_p4"
#define NVS_CARPETA     "carpeta"
/* Destino nuevo pedido a mitad de viaje, SIN aplicar todavia (ver op_destino).
 * Vacio o ausente = no se ha cambiado. */
#define NVS_DESTINO     "destino_nuevo"     /* ruta del viaje abierto, ausente si no hay */
/* Que pantalla empezo el viaje abierto: su MAC en hex. Vacio = no se sabe (una
 * cabina con firmware viejo, o un viaje empezado antes de este cambio). */
#define NVS_DEV         "v_dev"
#define NVS_IDS_RING    "ids_ring"    /* anillo de ids aplicados recientemente (idempotencia) */
#define NVS_IDS_CURSOR  "ids_cur"     /* proxima posicion libre del anillo */
#define IDS_RING_N      64            /* margen amplio sobre SALIDA_EVENTOS_MAX (4 en el
                                        * satelite): cubre eventos abiertos a la vez que se
                                        * cierran en cualquier orden. Ver id_ya_aplicado().
                                        *
                                        * Subido de 16 a 64 (08-sep-2026): con 16, un reintento
                                        * de una respuesta perdida que llegara tras >16 apuntes
                                        * aplicados de por medio ya no encontraba su id en el
                                        * anillo (evictado) y se volvia a aplicar como si fuera
                                        * nuevo -- doble conteo que ademas podia enmascarar la
                                        * perdida de OTRO apunte distinto en comprobar_completo().
                                        * Esquina muy rara (requiere un reintento retrasado mas
                                        * de 16 apuntes), pero el guard del satelite no la cubre.
                                        * 64 no lo hace estructuralmente imposible, solo mucho
                                        * mas improbable; el blob de NVS sigue siendo pequeno
                                        * (64 x uint32 = 256 B). */
/* Totales del viaje, acumulados SEGUN LLEGAN los apuntes en vez de sumando los
 * CSV al cerrar. Dos motivos: parsear CSV en el P4 seria bastante codigo para
 * algo que se puede ir sumando, y si el viaje se corta a lo bruto (bateria,
 * averia) los totales ya estan puestos al dia en vez de perderse. */
#define NVS_T_LITROS    "t_litros"
#define NVS_T_COMBUS    "t_combus"
#define NVS_T_PEAJE     "t_peaje"
#define NVS_T_BOMBONA   "t_bombona"
#define NVS_T_MANTEN    "t_manten"
#define NVS_T_AGUA      "t_agua"     /* vaciados y llenados de agua */
#define NVS_T_ALOJA     "t_aloja"    /* noches pagadas: areas y campings */
#define NVS_T_PARADAS   "t_paradas"
#define NVS_T_MONEDA    "t_moneda"    /* la primera vista */
#define NVS_T_VARIAS    "t_varias"    /* 1 = hubo mas de una moneda */
#define NVS_T_INICIO    "t_inicio"    /* epoch del inicio, para contar los dias */
#define NVS_T_APLIC     "t_aplic"     /* apuntes APLICADOS de este viaje */

/* Marca de que al viaje le faltan apuntes. Un fichero y no una linea dentro de
 * otro: la lista de descargas tiene que decidir el estado de cada viaje con un
 * stat(), sin abrir y parsear nada. */
#define MARCA_INCOMPLETO "INCOMPLETO.txt"
/* Delante del nombre de la carpeta de un viaje apartado. */
#define MARCA_DESCARTADO "DESCARTADO_"

/* El destino cabe en 20 caracteres (lo limita la 3.5"); la fecha son 10 y la
 * barra 1. Con 64 sobra y no hay que pensar en desbordes al componer rutas. */
#define CARPETA_MAX     64
#define RUTA_MAX        (CARPETA_MAX + 32)

/* Tope real que se acepta para destino_seguro() al componer la carpeta: deja
 * sitio de sobra dentro de CARPETA_MAX para VIAJES_DIR + "/" + fecha
 * (AAAA-MM-DD) + "_" + destino + el sufijo numerador "_NN" de op_inicio. Si
 * se truncara el destino no pasa nada (solo estetica); si se truncara el
 * sufijo numerador, dos intentos de desambiguar una colision colisionarian
 * a su vez en el MISMO nombre truncado -- la numeracion dejaria de servir
 * para nada. El destino real lo limita la 3.5" a 20 caracteres; esto solo
 * es la cota defensiva para cuando ese dato no es de fiar (ver el
 * comentario de destino_seguro). Detectado auditando el 08-sep-2026. */
#define DESTINO_MAX     24

/* ── Estado del viaje abierto, en NVS para sobrevivir a reinicios ─────────── */

static bool viaje_abierto(char *out, size_t n)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t len = n;
    esp_err_t e = nvs_get_str(h, NVS_CARPETA, out, &len);
    nvs_close(h);
    return e == ESP_OK && out[0];
}

/* Version PUBLICA: la necesitan los logs para decir A QUE VIAJE pertenecen
 * (ver log_capture.c) y el aviso de la OTA. Declarada en el .h. */
bool viaje_abierto_nombre(char *out, size_t n)
{
    return viaje_abierto(out, n);
}

/* Ids aplicados recientemente, en anillo -- no solo el mayor visto.
 *
 * Antes se guardaba un unico "ultimo id" y se rechazaba cualquier
 * id <= ese maximo. Pero el satelite reserva el id AL ABRIR el evento, no al
 * mandarlo (ver el comentario de salida_evento_abrir() en el satelite), y con
 * varios eventos abiertos a la vez (hasta SALIDA_EVENTOS_MAX) pueden cerrarse
 * y llegar en cualquier orden: un id mas bajo que otro ya aplicado puede
 * seguir siendo NUEVO de verdad. Con el maximo se descartaba como duplicado y
 * la parada se perdia. Detectado auditando el 07-sep-2026. */
static bool id_ya_aplicado(uint32_t id)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
    uint32_t ring[IDS_RING_N] = {0};
    size_t len = sizeof(ring);
    nvs_get_blob(h, NVS_IDS_RING, ring, &len);
    nvs_close(h);
    for (int i = 0; i < IDS_RING_N; i++) {
        if (ring[i] == id) return true;
    }
    return false;
}

/* Vacia el anillo. Se llama al abrir viaje (ver op_inicio): la numeracion
 * empieza de cero en cada viaje, y sin esto un id bajo del viaje nuevo podia
 * coincidir con uno que quedara aun en el anillo del viaje anterior. Usa el
 * handle YA ABIERTO de quien llama, sin comitear -- eso lo hace quien llama,
 * junto con lo demas que vaya a tocar en el mismo NVS. */
static void ids_ring_limpiar(nvs_handle_t h)
{
    uint32_t ring[IDS_RING_N] = {0};
    nvs_set_blob(h, NVS_IDS_RING, ring, sizeof(ring));
    nvs_set_u8(h, NVS_IDS_CURSOR, 0);
}

/* Version que opera sobre un handle YA ABIERTO, sin comitear ni cerrar --
 * mismo motivo que totales_sumar_en(). */
static void estado_set_en(nvs_handle_t h, const char *carpeta, uint32_t id)
{
    if (carpeta) nvs_set_str(h, NVS_CARPETA, carpeta);
    else         nvs_set_str(h, NVS_CARPETA, "");

    uint32_t ring[IDS_RING_N] = {0};
    size_t len = sizeof(ring);
    nvs_get_blob(h, NVS_IDS_RING, ring, &len);
    uint8_t cursor = 0;
    nvs_get_u8(h, NVS_IDS_CURSOR, &cursor);
    if (cursor >= IDS_RING_N) cursor = 0;
    ring[cursor] = id;
    nvs_set_blob(h, NVS_IDS_RING, ring, sizeof(ring));
    nvs_set_u8(h, NVS_IDS_CURSOR, (uint8_t)((cursor + 1) % IDS_RING_N));
}

/* Envoltorio para los llamadores que solo tocan esto (op_inicio/op_fin/
 * op_descartar): abre, marca, comitea. op_registro() NO usa este -- llama a
 * estado_set_en() sobre su propio handle, junto con totales y el contador.
 *
 * DEVUELVE el resultado (13-oct-2026): antes era void y se tiraba el del
 * nvs_commit, asi que un fallo al cerrar el viaje dejaba al usuario creyendo
 * que estaba cerrado mientras la P4 seguia con el abierto. */
static esp_err_t estado_set(const char *carpeta, uint32_t id)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return ESP_FAIL;
    /* Al CERRAR (carpeta == NULL) se borra tambien de que pantalla era: si no,
     * el siguiente viaje heredaria el dueno del anterior y una cabina distinta
     * se llevaria un 412 en vez del 409 de siempre. */
    if (!carpeta) nvs_erase_key(h, NVS_DEV);
    estado_set_en(h, carpeta, id);
    esp_err_t e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* ── Reparacion al arrancar (auditoria del 13-oct-2026) ───────────────────── */

/* Si la NVS dice que hay un viaje abierto pero su carpeta no esta en la
 * tarjeta, la P4 se queda en un callejon sin salida del que solo se sale
 * borrando su NVS a mano: el fin da 507 porque no puede escribir en una carpeta
 * que no existe, los apuntes de la cabina dan 507 y le atascan la cola entera,
 * un viaje nuevo da 409 y ni descartar ni borrar funcionan.
 *
 * Se llega ahi de tres maneras: sacando la tarjeta con el viaje abierto y
 * metiendo otra (o formateandola, o borrando la carpeta desde el PC), con un
 * corte de corriente en el hueco de un renombrado que ya no existe (ver op_fin y
 * op_descartar), o cambiando de tarjeta sin mas.
 *
 * Esto NO cierra el viaje -- sus datos no estan en esta tarjeta, y escribir un
 * cierre inventado seria mentir --: limpia el estado para que la P4 vuelva a
 * ser utilizable y lo deja bien claro en el log. Se llama SOLO con la SD
 * montada: si la tarjeta no esta, su ausencia no significa nada. */
static void marcar_huerfanas(const char *abierta);

void viaje_reparar_al_arrancar(void)
{
    if (!datalogger_sd_montada()) {
        ESP_LOGW(TAG, "SD sin montar: no compruebo el viaje abierto");
        return;
    }
    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) {
        marcar_huerfanas(NULL);      /* no hay viaje abierto: ninguna es "la de ahora" */
        return;
    }

    struct stat st;
    if (stat(carpeta, &st) == 0 && S_ISDIR(st.st_mode)) {
        marcar_huerfanas(carpeta);
        return;                      /* todo en su sitio */
    }

    const char *base = strrchr(carpeta, '/');
    base = base ? base + 1 : carpeta;

    /* Antes de dar el viaje por perdido: puede ser que SI se cerrara y lo unico
     * que quedo a medias fuera borrar NVS_CARPETA. Los dos renombrados posibles
     * son el de "apartar" (DESCARTADO_<nombre>) y el del cambio de destino
     * (<fecha>_<destino nuevo>, misma fecha, mismo guion). */
    char otra[RUTA_MAX];
    snprintf(otra, sizeof(otra), VIAJES_DIR "/" MARCA_DESCARTADO "%s", base);
    if (stat(otra, &st) == 0) {
        ESP_LOGW(TAG, "viaje abierto en NVS pero la carpeta se aparto (%s): "
                      "limpio el estado", otra);
    } else {
        /* Mismo dia, otro destino: "2026-10-11_arantzazu" -> "2026-10-11_BILBAO".
         * Se busca por el prefijo de la fecha. */
        char pref[16] = {0};
        const char *guion = strchr(base, '_');
        size_t np = guion ? (size_t)(guion - base) + 1 : 0;
        if (np >= sizeof(pref) || np == 0) np = 0;
        memcpy(pref, base, np);
        pref[np] = 0;

        bool hallado = false;
        if (pref[0]) {
            DIR *d = opendir(VIAJES_DIR);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) {
                    if (strncmp(e->d_name, pref, np) == 0 &&
                        strcmp(e->d_name, base) != 0) { hallado = true; break; }
                }
                closedir(d);
            }
        }
        if (hallado)
            ESP_LOGW(TAG, "viaje abierto en NVS (%s) pero hay una carpeta del "
                          "mismo dia con otro nombre: el cierre se quedo a medias "
                          "despues de renombrar. Limpio el estado", carpeta);
        else
            ESP_LOGE(TAG, "VIAJE ABIERTO SIN CARPETA: %s no esta en la tarjeta "
                          "(cambiada, formateada o borrada desde el PC). Limpio "
                          "el estado: los datos de ese viaje NO estan aqui",
                     carpeta);
    }
    estado_set(NULL, 0);
    /* Ya no hay viaje abierto: las demas carpetas sin cierre que haya en ESTA
     * tarjeta tampoco se podran cerrar nunca, asi que se marcan ahora (antes
     * esta rama se volvia sin mirarlas y se quedaban "En curso" hasta el
     * siguiente arranque). */
    marcar_huerfanas(NULL);
}

/* Marca las carpetas de viaje que NO se pueden cerrar ya.
 *
 * Una carpeta con "inicio" en el diario pero sin viaje abierto en NVS y sin
 * resumen.txt no es un viaje en curso: es un viaje que se quedo a medias al
 * EMPEZAR (corte de corriente entre crear la carpeta y apuntar el viaje, o
 * entre el diario y el commit). Nadie va a poder cerrarlo nunca, asi que
 * /data/viajes lo enseñaba para siempre como "En curso: finalizalo en la
 * cabina", sin enlace de descarga, y el usuario no tenia forma de bajarse unos
 * datos que SI estan en la tarjeta (auditoria del 13-oct-2026, hallazgo B3).
 *
 * Se le escribe INCOMPLETO.txt, que es lo que lo convierte en descargable "de
 * todos modos" y con el nombre gritando lo que es. NO se toca ninguna carpeta
 * que sea el viaje abierto ni las ya marcadas o cerradas. */
static void marcar_huerfanas(const char *abierta)
{
    const char *base_abierta = abierta ? strrchr(abierta, '/') : NULL;
    base_abierta = base_abierta ? base_abierta + 1 : abierta;

    if (!camera_sd_bus_lock(5000)) return;
    DIR *d = opendir(VIAJES_DIR);
    if (!d) { camera_sd_bus_unlock(); return; }
    int marcadas = 0;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        if (!strncmp(ent->d_name, MARCA_DESCARTADO, strlen(MARCA_DESCARTADO))) continue;
        if (base_abierta && !strcmp(ent->d_name, base_abierta)) continue;
        char ruta[RUTA_MAX];
        struct stat st;
        snprintf(ruta, sizeof(ruta), VIAJES_DIR "/%s/resumen.txt", ent->d_name);
        if (stat(ruta, &st) == 0) continue;                       /* cerrado */
        snprintf(ruta, sizeof(ruta), VIAJES_DIR "/%s/" MARCA_INCOMPLETO, ent->d_name);
        if (stat(ruta, &st) == 0) continue;                       /* ya marcado */
        snprintf(ruta, sizeof(ruta), VIAJES_DIR "/%s/eventos.csv", ent->d_name);
        if (stat(ruta, &st) != 0) continue;                       /* ni empezo */
        snprintf(ruta, sizeof(ruta), VIAJES_DIR "/%s/" MARCA_INCOMPLETO, ent->d_name);
        FILE *f = fopen(ruta, "w");
        if (!f) continue;
        fprintf(f, "Este viaje NO SE CERRO y ya no se puede cerrar.\n\n"
                   "Se quedo a medias al EMPEZAR: la carpeta llego a crearse pero la\n"
                   "P4 no llego a apuntar el viaje (un corte de corriente en ese\n"
                   "momento). No es el viaje abierto, asi que no hay forma de\n"
                   "finalizarlo desde la cabina.\n\n"
                   "Los datos que haya dentro son los que se llegaron a escribir.\n");
        if (fclose(f) == 0) marcadas++;
    }
    closedir(d);
    camera_sd_bus_unlock();
    if (marcadas) ESP_LOGW(TAG, "%d carpeta(s) de viaje que se quedaron sin poder "
                                "cerrarse: marcadas como INCOMPLETO", marcadas);
}

/* ── Utilidades ───────────────────────────────────────────────────────────── */

/* fecha_dias son dias desde 1970 EN HORA LOCAL, tal y como los calculo la P4 y
 * los recibio el satelite (ver epoch_local en mini_proto.h). Se convierten con
 * gmtime a proposito, NO con localtime: el desfase ya esta aplicado, y volver a
 * aplicarlo correria la fecha un dia en los extremos. */
static void fecha_de_dias(uint32_t dias, char *out, size_t n)
{
    time_t t = (time_t)dias * 86400;
    struct tm tm_u;
    gmtime_r(&t, &tm_u);
    strftime(out, n, "%Y-%m-%d", &tm_u);
}

/* El destino llega ya filtrado por la 3.5", pero eso es la palabra de OTRO
 * aparato sobre un dato que acaba siendo una RUTA. Se vuelve a filtrar aqui:
 * solo letras, digitos, guion y guion bajo; el espacio pasa a guion bajo. */
static void destino_seguro(const char *in, char *out, size_t n)
{
    size_t j = 0;
    for (size_t i = 0; in[i] && j + 1 < n; i++) {
        char c = in[i];
        if      ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '-' || c == '_') out[j++] = c;
        else if (c == ' ')                                       out[j++] = '_';
        /* cualquier otra cosa (/, \, :, *, acentos...) se tira */
    }
    out[j] = 0;
    if (!j) snprintf(out, n, "viaje");   /* nunca una carpeta sin nombre */
}

/* Un valor de TEXTO al CSV, con las comas y las comillas sustituidas.
 *
 * POR QUE SE SUSTITUYEN Y NO SE ENTRECOMILLA (auditoria del 13-oct-2026): el
 * texto libre de verdad lo teclea el usuario ("Cambio de filtro, aceite y
 * correa"), y una coma sin escapar mete una columna de mas: la fila entera se
 * desplaza y la hoja de calculo o el analizador del PC leen mal ese apunte. La
 * solucion "correcta" seria entrecomillar y doblar las comillas interiores
 * (RFC 4180), pero el analizador que ya existe en el PC parte eventos.csv a
 * mano por las dos primeras comas (ver read_eventos en la app de viajes): con
 * comillas, el usuario veria las comillas dentro del texto. Sustituir deja
 * TODOS los lectores actuales correctos, que es lo que hace falta aqui; lo que
 * se pierde es una coma tecleada, y el texto con comas sigue entero en
 * resumen.txt, que es donde se lee. */
static void csv_texto(FILE *f, const char *v)
{
    for (const char *p = v; *p; p++) {
        if      (*p == ',') fputc(';', f);
        else if (*p == '"') fputc('\'', f);
        else if (*p == '\n' || *p == '\r') fputc(' ', f);   /* una fila = una linea */
        else                fputc(*p, f);
    }
}

/* Tamano de un fichero, o 0 si no existe: la "marca de agua" para poder
 * deshacer una escritura que no ha llegado a cuajar. */
static off_t marca_de(const char *ruta)
{
    struct stat st;
    return (stat(ruta, &st) == 0) ? st.st_size : 0;
}

/* Deshace un append: deja el fichero como estaba antes de la marca.
 *
 * POR QUE (auditoria del 13-oct-2026): un apunte se escribe en DOS ficheros (el
 * diario y el CSV de su tipo) y despues se comitea en NVS. Si falla cualquiera
 * de las tres cosas, el satelite NO lo descarta: se le contesta 507 y lo
 * reintenta. El reintento volvia a escribir lo que YA habia entrado, asi que
 * quedaban lineas duplicadas en el diario y en el CSV -- y como el analizador
 * suma filas y no ids, esas dos lineas cuentan como dos repostajes. Con la
 * marca, lo que no cuaja se deshace y el reintento entra limpio. */
static void marca_deshacer(const char *ruta, off_t marca)
{
    int fd = open(ruta, O_WRONLY);
    if (fd < 0) return;                      /* ni se creo: nada que deshacer */
    if (ftruncate(fd, marca) != 0)
        ESP_LOGW(TAG, "no he podido deshacer la escritura en %s", ruta);
    close(fd);
}

/* Una linea al diario del viaje. eventos.csv es el que se lee para saber QUE
 * paso; los csv por tipo (fase 3) son para hacer cuentas. */
/* Devuelve si la linea ha llegado a la tarjeta. Antes no devolvia nada y el
 * apunte se contestaba con 200 igualmente: con la tarjeta llena o fallando, el
 * satelite borraba el apunte de su cola creyendolo guardado. Perdido en
 * silencio, y ademas confirmado (auditoria del 24-ago-2026). */
static bool diario_en(const char *carpeta, const char *cuando,
                      const char *que, const char *detalle)
{
    char ruta[RUTA_MAX];
    snprintf(ruta, sizeof(ruta), "%s/eventos.csv", carpeta);

    bool nuevo = true;
    struct stat st;
    if (stat(ruta, &st) == 0 && st.st_size > 0) nuevo = false;

    FILE *f = fopen(ruta, "a");
    if (!f) { ESP_LOGW(TAG, "no puedo escribir %s", ruta); return false; }
    if (nuevo) fprintf(f, "fecha_hora,evento,detalle\n");

    fprintf(f, "%s,", cuando);
    csv_texto(f, que);
    fputc(',', f);
    csv_texto(f, detalle ? detalle : "");
    fputc('\n', f);
    /* fclose devuelve error si el volcado a la tarjeta fallo (disco lleno):
     * hasta aqui todo eran fprintf a un buffer que puede no haber bajado.
     * Mismo motivo que escribir_resumen() -- antes solo se comprobaba el
     * fopen, y un disco lleno a mitad de escritura devolvia 200 igual. */
    /* Y fsync ANTES de fclose (auditoria del 13-oct-2026): fclose vacia a la
     * VFS, pero el dato puede quedarse en la cache interna de la tarjeta y
     * perderse con un corte de corriente DESPUES de haberle contestado 200 al
     * satelite, que para entonces ya lo ha borrado de su cola. No habia ni un
     * fsync en todo el firmware. */
    if (fsync(fileno(f)) != 0) {
        ESP_LOGW(TAG, "fsync fallo en %s (disco lleno?)", ruta);
        fclose(f);
        return false;
    }
    return fclose(f) == 0;
}

/* Inicio y fin los fecha la P4: ocurren con ella delante (el inicio la exige, y
 * el fin llega por la cola pero su hora real la lleva dentro el propio evento
 * de cierre del satelite... que hoy no la manda, asi que de momento es la de
 * recepcion. Se afina en la fase 4, junto con el resumen). */
/* Devuelve si la linea ha entrado; ver la nota de diario_en(). */
static bool diario(const char *carpeta, const char *que, const char *detalle)
{
    time_t now = time(NULL);
    struct tm tm_l;
    localtime_r(&now, &tm_l);
    char cuando[20];
    strftime(cuando, sizeof(cuando), "%Y-%m-%d %H:%M:%S", &tm_l);
    return diario_en(carpeta, cuando, que, detalle);
}

/* ── Totales del viaje ────────────────────────────────────────────────────── */

static double nvs_get_d(nvs_handle_t h, const char *k)
{
    /* NVS no guarda double: se mete el patron de bits en un u64. Es exacto y
     * evita redondeos que en dinero se notan. */
    uint64_t raw = 0;
    if (nvs_get_u64(h, k, &raw) != ESP_OK) return 0.0;
    double v; memcpy(&v, &raw, sizeof(v));
    return v;
}

static void nvs_set_d(nvs_handle_t h, const char *k, double v)
{
    uint64_t raw; memcpy(&raw, &v, sizeof(raw));
    nvs_set_u64(h, k, raw);
}

/* Suma lo que trae el apunte a los totales del viaje.
 *
 * OJO CON LAS MONEDAS: se suman los numeros tal cual, sin convertir. Un viaje
 * que cruza a Suiza mezclaria euros y francos en el mismo total y el resultado
 * no significaria nada. No se convierte (haria falta un cambio, que este
 * aparato no tiene y quedaria desfasado), pero SI se detecta: si aparece mas de
 * una moneda, el resumen lo dice en vez de dar un total que parece bueno. */
/* Version que opera sobre un handle YA ABIERTO, sin comitear ni cerrar: la
 * usa op_registro() para que esto, el contador de aplicados y estado_set()
 * (el anillo de dedupe) se comiteen TODOS JUNTOS en un solo nvs_commit al
 * final -- ver el comentario de op_registro sobre por que separarlos en
 * commits independientes era peligroso. Detectado el 09-sep-2026. */
static void totales_sumar_en(nvs_handle_t h, const char *tipo, const cJSON *datos)
{
    if (!datos) return;

    const cJSON *jm = cJSON_GetObjectItem(datos, "moneda");
    if (cJSON_IsString(jm) && jm->valuestring[0]) {
        char vista[8] = {0};
        size_t n = sizeof(vista);
        if (nvs_get_str(h, NVS_T_MONEDA, vista, &n) != ESP_OK || !vista[0]) {
            nvs_set_str(h, NVS_T_MONEDA, jm->valuestring);
        } else if (strcmp(vista, jm->valuestring) != 0) {
            nvs_set_u8(h, NVS_T_VARIAS, 1);
        }
    }

    /* atof de un campo de texto; los importes viajan como cadena porque es lo
     * que hay tecleado, con su coma o su punto tal cual. */
    const cJSON *ji;
    #define IMPORTE_DE(campo) ( (ji = cJSON_GetObjectItem(datos, campo)) && \
                                cJSON_IsString(ji) ? atof(ji->valuestring) : 0.0 )

    if (!strcmp(tipo, "repostaje")) {
        nvs_set_d(h, NVS_T_LITROS, nvs_get_d(h, NVS_T_LITROS) + IMPORTE_DE("litros"));
        nvs_set_d(h, NVS_T_COMBUS, nvs_get_d(h, NVS_T_COMBUS) + IMPORTE_DE("importe"));
    } else if (!strcmp(tipo, "peaje")) {
        nvs_set_d(h, NVS_T_PEAJE, nvs_get_d(h, NVS_T_PEAJE) + IMPORTE_DE("importe"));
    } else if (!strcmp(tipo, "bombona")) {
        nvs_set_d(h, NVS_T_BOMBONA, nvs_get_d(h, NVS_T_BOMBONA) + IMPORTE_DE("precio"));
    } else if (!strcmp(tipo, "mantenimiento")) {
        nvs_set_d(h, NVS_T_MANTEN, nvs_get_d(h, NVS_T_MANTEN) + IMPORTE_DE("coste"));
    } else if (!strcmp(tipo, "itv")) {
        /* Va al saco del mantenimiento y no a uno propio: es un gasto del
         * vehiculo, sale una vez al ano y una linea suya en el resumen estaria
         * a cero en todos los viajes menos uno. */
        nvs_set_d(h, NVS_T_MANTEN, nvs_get_d(h, NVS_T_MANTEN) + IMPORTE_DE("precio"));
    } else if (!strcmp(tipo, "agua")) {
        /* Los TRES importes al mismo total: en el resumen interesa lo que se ha
         * ido en aguas, y el desglose de cual fue el vaciado y cual el llenado
         * ya esta en aguas.csv. */
        /* De uno en uno y NO en la misma suma: IMPORTE_DE asigna a 'ji', y tres
         * asignaciones a la misma variable dentro de una expresion no tienen
         * orden definido (el compilador lo para en seco: -Wsequence-point). */
        double t = nvs_get_d(h, NVS_T_AGUA);
        t += IMPORTE_DE("precio_grises");
        t += IMPORTE_DE("precio_wc");
        t += IMPORTE_DE("precio_agua");
        nvs_set_d(h, NVS_T_AGUA, t);
    } else if (!strcmp(tipo, "parada")) {
        uint32_t np = 0;
        nvs_get_u32(h, NVS_T_PARADAS, &np);
        nvs_set_u32(h, NVS_T_PARADAS, np + 1);
    } else if (!strcmp(tipo, "pernocta")) {
        /* Cuenta como parada -- lo es -- y ademas paga. */
        uint32_t np = 0;
        nvs_get_u32(h, NVS_T_PARADAS, &np);
        nvs_set_u32(h, NVS_T_PARADAS, np + 1);

        /* El precio que manda el satelite es POR NOCHE (o por 24 h), que es lo
         * que se anuncia a la entrada. Lo que cuesta la estancia hay que
         * multiplicarlo aqui. */
        double precio = IMPORTE_DE("precio");
        const cJSON *jn = cJSON_GetObjectItem(datos, "noches");
        double noches = cJSON_IsNumber(jn) ? jn->valuedouble : 1.0;
        /* Llegar y marcharse el mismo dia son cero noches de calendario, pero
         * el area se cobra igual: por debajo de una no se baja. */
        if (noches < 1.0) noches = 1.0;
        /* Tope de cordura, igual que en la cabina (salida_noches): un reloj que
         * salta puede mandar decenas de "noches" y aqui se multiplica por el
         * precio. Si llega algo absurdo, se acota y se dice. */
        if (noches > 60.0) {
            ESP_LOGW(TAG, "noches fuera de lo creible (%.0f): el reloj de la "
                          "cabina ha dado un salto. Se cuentan 60", noches);
            noches = 60.0;
        }

        /* Y lo que se pago por los servicios, que en un area se cobran aparte:
         * el agua 1 euro, la luz 2. Van al mismo saco que la noche -- es lo que
         * costo dormir ahi -- pero el DESGLOSE queda entero en pernoctas.csv,
         * que es donde se mira para decidir si volver.
         *
         * De uno en uno y no en la misma suma: IMPORTE_DE asigna a 'ji' (ver la
         * nota de las aguas). */
        double extras = 0;
        extras += IMPORTE_DE("precio_agua");
        extras += IMPORTE_DE("precio_grises");
        extras += IMPORTE_DE("precio_wc");
        extras += IMPORTE_DE("precio_luz");
        extras += IMPORTE_DE("precio_duchas");
        extras += IMPORTE_DE("precio_basura");

        nvs_set_d(h, NVS_T_ALOJA,
                  nvs_get_d(h, NVS_T_ALOJA) + precio * noches + extras);
    }
    #undef IMPORTE_DE
}

static void totales_borrar(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_T_LITROS);  nvs_erase_key(h, NVS_T_COMBUS);
    nvs_erase_key(h, NVS_T_PEAJE);   nvs_erase_key(h, NVS_T_BOMBONA);
    nvs_erase_key(h, NVS_T_MANTEN);  nvs_erase_key(h, NVS_T_PARADAS);
    nvs_erase_key(h, NVS_T_AGUA);   nvs_erase_key(h, NVS_T_ALOJA);
    nvs_erase_key(h, NVS_T_MONEDA);  nvs_erase_key(h, NVS_T_VARIAS);
    nvs_commit(h);
    nvs_close(h);
}

/* ── Las operaciones ──────────────────────────────────────────────────────── */

static esp_err_t op_inicio(httpd_req_t *req, const cJSON *j, uint32_t id)
{
    const cJSON *jd = cJSON_GetObjectItem(j, "destino");
    const cJSON *jf = cJSON_GetObjectItem(j, "fecha_dias");
    if (!cJSON_IsString(jd) || !cJSON_IsNumber(jf)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "faltan destino o fecha_dias");
        return ESP_OK;
    }

    char abierto[CARPETA_MAX];
    if (viaje_abierto(abierto, sizeof(abierto))) {
        /* No se pisa un viaje abierto: seria perder la carpeta anterior sin que
         * nadie se enterara. El satelite tiene que cerrarlo primero.
         *
         * Y SI EL VIAJE ABIERTO LO EMPEZO OTRA PANTALLA SE DICE APARTE (412, no
         * 409): es el caso que la auditoria de comunicacion del 13-oct-2026 vio
         * peligroso -- la segunda pantalla ofrece "apartarlo (era una prueba)" y
         * acaba renombrando la carpeta del viaje REAL de la primera, que sigue
         * grabando, y sus apuntes se van a /sdcard/vehiculo. Con un codigo
         * distinto, la cabina avisa de eso SIN leer el cuerpo de la respuesta
         * (que hoy no llega a la pantalla).
         *
         * 'dev' es la MAC de la cabina en hex. Si no viene (firmware viejo) o no
         * hay ninguna apuntada, se contesta 409 como siempre: no se puede
         * afirmar que sea de otra. */
        const cJSON *jdev = cJSON_GetObjectItemCaseSensitive(j, "dev");
        char dev_pide[24] = {0};
        if (cJSON_IsString(jdev)) destino_seguro(jdev->valuestring, dev_pide, sizeof(dev_pide));
        char dev_abierto[24] = {0};
        {
            nvs_handle_t hd;
            if (nvs_open(NVS_NS, NVS_READONLY, &hd) == ESP_OK) {
                size_t n = sizeof(dev_abierto);
                if (nvs_get_str(hd, NVS_DEV, dev_abierto, &n) != ESP_OK) dev_abierto[0] = 0;
                nvs_close(hd);
            }
        }
        bool ajena = viaje_dev_ajeno(dev_pide, dev_abierto) != 0;
        ESP_LOGW(TAG, "inicio con viaje ya abierto en %s (%s)", abierto,
                 ajena ? "lo empezo OTRA pantalla" : "misma pantalla o no se sabe");
        httpd_resp_set_status(req, ajena ? "412 Precondition Failed" : "409 Conflict");
        /* CON EL NOMBRE DEL VIAJE ABIERTO (auditoria del 13-oct-2026). La
         * pantalla de cabina no podia decir CUAL era, y con dos pantallas (o con
         * el cierre del viaje anterior todavia en la cola) el usuario no tenia
         * forma de saber si ese "viaje abierto" era suyo, de la otra cabina, o
         * un resto de una prueba -- y la salida que se le ofrecia, "apartarlo",
         * renombra la carpeta del viaje de verdad. Una cabina antigua lo ignora
         * (solo mira el codigo); las nuevas lo enseñan. */
        char resp[192];
        snprintf(resp, sizeof(resp), "ya hay un viaje abierto: %s%s", abierto,
                 ajena ? " (lo empezo otra pantalla)" : "");
        httpd_resp_sendstr(req, resp);
        return ESP_OK;
    }

    char destino[CARPETA_MAX], fecha[12];
    destino_seguro(jd->valuestring, destino, DESTINO_MAX);
    fecha_de_dias((uint32_t)jf->valuedouble, fecha, sizeof(fecha));

    char carpeta[CARPETA_MAX];
    snprintf(carpeta, sizeof(carpeta), VIAJES_DIR "/%s_%s", fecha, destino);

    /* El destino apuntado de un cambio era del viaje ANTERIOR: se borra para no
     * renombrar este con el nombre del de antes. */
    nvs_handle_t hd;
    if (nvs_open(NVS_NS, NVS_READWRITE, &hd) == ESP_OK) {
        nvs_set_str(hd, NVS_DESTINO, "");
        nvs_commit(hd);
        nvs_close(hd);
    }

    if (!camera_sd_bus_lock(3000)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "tarjeta ocupada");
        return ESP_OK;
    }
    mkdir(VIAJES_DIR, 0777);
    int mk = mkdir(carpeta, 0777);
    /* Colision: ya existe una carpeta fecha_destino (dos viajes distintos al
     * mismo sitio el mismo dia -- un reintento por respuesta perdida del
     * MISMO viaje no llega aqui, se corta antes con el 409 de arriba porque
     * viaje_abierto() seguiria en true). Antes se reusaba sin avisar: el
     * "inicio" del segundo viaje se mezclaba en el diario del primero, y
     * cualquier apunte suyo pisaba los ficheros del viaje anterior. Numerar
     * en vez de reusar. Detectado auditando el 08-sep-2026. */
    if (mk != 0 && errno == EEXIST) {
        char base[CARPETA_MAX];
        strncpy(base, carpeta, sizeof(base) - 1);
        base[sizeof(base) - 1] = 0;
        for (int n = 2; n <= 20; n++) {
            snprintf(carpeta, sizeof(carpeta), "%s_%d", base, n);
            mk = mkdir(carpeta, 0777);
            if (mk == 0 || errno != EEXIST) break;
        }
    }
    /* mk == 0 es la UNICA senal de exito valida: la carpeta la acabamos de
     * crear nosotros. Antes, si se agotaban los 20 numeros sin encontrar uno
     * libre, un stat(carpeta,...) de respaldo daba "hay" por bueno porque el
     * ULTIMO nombre probado (el "_20") tambien existia -- justo el reuso
     * silencioso de una carpeta ajena que esta numeracion se creo para
     * evitar, solo que aplazado hasta el intento 20 en vez del primero.
     * Detectado auditando el 08-sep-2026. */
    bool hay = (mk == 0);
    bool inicio_ok = hay ? diario(carpeta, "inicio", destino) : false;
    camera_sd_bus_unlock();

    if (!hay) {
        ESP_LOGE(TAG, "no puedo crear %s (ni con sufijo numerador, colisiones agotadas)", carpeta);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "no puedo crear la carpeta");
        return ESP_OK;
    }
    if (!inicio_ok) {
        /* La carpeta se acaba de crear y esta vacia o con media cabecera: se
         * deshace, para no dejar una carpeta huerfana que /data/viajes
         * enseñaria como "en curso" para siempre y que nadie podria cerrar. */
        ESP_LOGE(TAG, "no puedo escribir el inicio de %s (tarjeta llena?): "
                      "deshago la carpeta", carpeta);
        if (camera_sd_bus_lock(3000)) {
            char ev[RUTA_MAX];
            snprintf(ev, sizeof(ev), "%s/eventos.csv", carpeta);
            remove(ev);
            rmdir(carpeta);
            camera_sd_bus_unlock();
        }
        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_sendstr(req, "no puedo escribir el inicio del viaje");
        return ESP_OK;
    }

    totales_borrar();
    /* Contadores de energia a cero: el apartado ENERGIA del resumen.txt sale de
     * aqui. Hasta el 24-ago-2026 solo se ponian a cero con el boton de
     * Ajustes -> Autocaravana, en la pantalla de atras, asi que un viaje en el
     * que no se hubiera pulsado escribia en SU resumen la energia acumulada
     * desde vete a saber cuando -- a veces de tres viajes antes -- y con toda
     * la pinta de ser suya. El viaje se declara en la cabina; los contadores
     * se enteran aqui. */
    trip_computer_reset();
    ruta_olvidar();   /* ningun punto de un viaje anterior puede caer en este */

    /* ── Dejar constancia del viaje: TODO O NADA (auditoria del 13-oct-2026) ──
     *
     * Antes esto eran tres bloques sueltos que no miraban un solo resultado: ni
     * los nvs_set, ni los dos nvs_commit, ni estado_set(). Si la NVS fallaba
     * justo aqui (particion llena o gastada), la carpeta se creaba y la cabina
     * recibia 200 y abria el viaje... pero la P4 no quedaba enterada: TODOS los
     * apuntes de ese viaje acababan en /sdcard/vehiculo/ con 200 porque sin
     * viaje abierto op_registro escribe ahi, el fin contestaba 200 "no habia
     * viaje abierto" y nadie escribia resumen.txt. El usuario bajaba su viaje y
     * encontraba una carpeta con una linea "inicio": el viaje entero fuera de su
     * sitio, sin un solo aviso.
     *
     * Ahora es UN handle y UN commit, comprobado. Si no entra, el viaje NO se da
     * por abierto y la carpeta se deshace. */
    nvs_handle_t h;
    esp_err_t ne = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (ne == ESP_OK) {
        /* Numeracion nueva en cada viaje: ver el comentario de id_ya_aplicado()
         * -- se vacia el anillo entero, no solo se pone a 0 un maximo, para que
         * no queden ids sueltos del viaje anterior que puedan chocar con ids
         * bajos de este. */
        ids_ring_limpiar(h);
        /* El epoch del inicio, para contar los dias al cerrar. Se guarda en vez
         * de deducirlo del nombre de la carpeta: ahi solo esta la fecha, y un
         * viaje de una noche saldria como "0 dias". */
        nvs_set_u64(h, NVS_T_INICIO, (uint64_t)time(NULL));
        nvs_set_u32(h, NVS_T_APLIC, 1);   /* el inicio ya cuenta */
        /* Que pantalla empieza el viaje, en el MISMO commit que el resto del
         * estado: si no, un corte entre los dos deja un viaje sin dueno. */
        {
            const cJSON *jd2 = cJSON_GetObjectItemCaseSensitive(j, "dev");
            char dev[24] = {0};
            if (cJSON_IsString(jd2)) destino_seguro(jd2->valuestring, dev, sizeof(dev));
            nvs_set_str(h, NVS_DEV, dev);
        }
        estado_set_en(h, carpeta, id);
        ne = nvs_commit(h);
        nvs_close(h);
    }
    if (ne != ESP_OK) {
        ESP_LOGE(TAG, "la NVS no acepta el viaje %s (%s): lo dejo sin abrir y "
                      "deshago la carpeta", carpeta, esp_err_to_name(ne));
        if (camera_sd_bus_lock(3000)) {
            char ev[RUTA_MAX];
            snprintf(ev, sizeof(ev), "%s/eventos.csv", carpeta);
            remove(ev);
            rmdir(carpeta);
            camera_sd_bus_unlock();
        }
        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_sendstr(req, "no puedo guardar el viaje en la NVS");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "VIAJE ABIERTO: %s", carpeta);
    httpd_resp_sendstr(req, carpeta);
    return ESP_OK;
}

/* resumen.txt: lo que uno quiere saber al volver, en castellano llano y no en
 * columnas. Los CSV ya estan ahi para las cuentas finas. */
/* Devuelve si el resumen ha llegado a la tarjeta. Cerrar un viaje SIN resumen y
 * contestar que si es dejar al usuario con un viaje que cree cerrado y sin las
 * cuentas -- y sin enterarse hasta que lo descargue meses despues. */
static bool escribir_resumen(const char *carpeta)
{
    nvs_handle_t h;
    double litros = 0, combus = 0, peaje = 0, bombona = 0, manten = 0, agua = 0;
    double aloja = 0;
    uint32_t paradas = 0;
    uint64_t inicio = 0;
    uint8_t varias = 0;
    char moneda[8] = "EUR";
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        litros  = nvs_get_d(h, NVS_T_LITROS);
        combus  = nvs_get_d(h, NVS_T_COMBUS);
        peaje   = nvs_get_d(h, NVS_T_PEAJE);
        bombona = nvs_get_d(h, NVS_T_BOMBONA);
        manten  = nvs_get_d(h, NVS_T_MANTEN);
        agua    = nvs_get_d(h, NVS_T_AGUA);
        aloja   = nvs_get_d(h, NVS_T_ALOJA);
        nvs_get_u32(h, NVS_T_PARADAS, &paradas);
        nvs_get_u64(h, NVS_T_INICIO, &inicio);
        nvs_get_u8(h, NVS_T_VARIAS, &varias);
        size_t n = sizeof(moneda);
        nvs_get_str(h, NVS_T_MONEDA, moneda, &n);
        nvs_close(h);
    }

    char ruta[RUTA_MAX];
    snprintf(ruta, sizeof(ruta), "%s/resumen.txt", carpeta);
    FILE *f = fopen(ruta, "w");
    if (!f) { ESP_LOGE(TAG, "no puedo escribir %s", ruta); return false; }

    time_t now = time(NULL);
    struct tm tm_l;
    localtime_r(&now, &tm_l);
    char cuando[20];
    strftime(cuando, sizeof(cuando), "%Y-%m-%d %H:%M", &tm_l);

    fprintf(f, "RESUMEN DEL VIAJE\n");
    fprintf(f, "=================\n\n");
    fprintf(f, "Cerrado el %s\n", cuando);
    if (inicio > 1000000000ULL) {
        /* Dias de calendario, no periodos de 24 h: salir un viernes y volver el
         * domingo son tres dias de viaje, aunque no sean 72 horas. */
        long dias = (long)((now / 86400) - ((time_t)inicio / 86400)) + 1;
        fprintf(f, "Duracion: %ld dia%s\n", dias, dias == 1 ? "" : "s");
    }
    fprintf(f, "\nGASTOS (%s)\n", moneda);
    fprintf(f, "  Combustible ... %8.2f   (%.1f litros)\n", combus, litros);
    fprintf(f, "  Peajes ........ %8.2f\n", peaje);
    fprintf(f, "  Gas ........... %8.2f\n", bombona);
    fprintf(f, "  Mantenimiento . %8.2f\n", manten);
    fprintf(f, "  Aguas ......... %8.2f\n", agua);
    fprintf(f, "  Alojamiento ... %8.2f\n", aloja);
    fprintf(f, "  ----------------------\n");
    fprintf(f, "  TOTAL ......... %8.2f\n",
            combus + peaje + bombona + manten + agua + aloja);
    if (varias) {
        /* Sumar euros con francos da un numero que parece bueno y no lo es. */
        fprintf(f, "\n  OJO: hubo mas de una moneda en este viaje.\n");
        fprintf(f, "  Los totales estan SUMADOS SIN CONVERTIR y no valen.\n");
        fprintf(f, "  Mira los csv de cada tipo, que llevan su moneda.\n");
    }
    if (litros > 0.01 && combus > 0.01) {
        fprintf(f, "\n  Precio medio del litro: %.3f %s\n", combus / litros, moneda);
    }
    fprintf(f, "\nParadas anotadas: %lu\n", (unsigned long)paradas);

    trip_computer_t t;
    trip_computer_get(&t);
    fprintf(f, "\nENERGIA\n");
    fprintf(f, "  Cargados ...... %8.0f Wh\n", t.wh_charged);
    fprintf(f, "  Gastados ...... %8.0f Wh\n", t.wh_discharged);
    fprintf(f, "  De la placa ... %8.0f Wh   (%.1f h de sol)\n",
            t.wh_solar, (double)t.solar_seconds / 3600.0);

    /* La bombona, en el resumen: al volver de viaje es cuando uno se pregunta
     * si le dio de si. Se dice ademas si se cambio DURANTE este viaje, que es
     * el dato que de verdad se olvida. */
    {
        bombonas_t bo;
        bombonas_get(&bo);
        if (bo.n > 0) {
            fprintf(f, "\nBOMBONA\n");
            int dias = bombonas_dias_actual();
            char puesta[24];
            time_t tb = (time_t)bo.cambios[bo.n - 1];
            struct tm tmb;
            localtime_r(&tb, &tmb);
            strftime(puesta, sizeof(puesta), "%d/%m/%Y", &tmb);
            if (dias >= 0) fprintf(f, "  Puesta el %s (lleva %d dias)\n", puesta, dias);
            else           fprintf(f, "  Puesta el %s\n", puesta);

            /* Cambios caidos entre el inicio del viaje y ahora. */
            int durante = 0;
            for (int i = 0; i < bo.n; i++) {
                if (inicio && bo.cambios[i] >= (uint32_t)inicio) durante++;
            }
            if (durante > 0)
                fprintf(f, "  Cambiada %d ve%s durante este viaje\n",
                        durante, durante == 1 ? "z" : "ces");

            float med = bombonas_media_dias();
            if (med > 0.0f) {
                fprintf(f, "  Duran %.0f dias de media\n", (double)med);
                if (bombonas_pasada())
                    fprintf(f, "  OJO: la actual ya dura mas que las anteriores.\n");
            }
        }
    }

    /* Kilometros por GPS desde el 24-ago-2026 (NEO-M9N montado y con fix). Si el
     * viaje entero fue sin posicion se DICE, en vez de escribir un "0.0 km" que
     * se leeria como que no te moviste. */
    if (t.km >= 0.1) {
        fprintf(f, "\nRecorrido: %.1f km\n", t.km);
        if (t.km > 0.5 && (combus + peaje) > 0.01) {
            fprintf(f, "  Combustible y peajes: %.3f %s por km\n",
                    (combus + peaje) / t.km, moneda);
        }
    } else {
        fprintf(f, "\nRecorrido: no se ha podido medir. El GPS no llego a tener\n"
                   "posicion durante el viaje (antena tapada o pantalla apagada).\n");
    }
    /* fclose devuelve error si el volcado a la tarjeta fallo (disco lleno):
     * hasta aqui todo eran fprintf a un buffer que puede no haber bajado. Y
     * fsync antes: este es EL fichero que dice si un viaje esta cerrado, y no
     * puede quedarse en la cache de la tarjeta despues de haber contestado que
     * si (auditoria del 13-oct-2026). */
    if (fsync(fileno(f)) != 0) {
        ESP_LOGW(TAG, "fsync fallo en %s (disco lleno?)", ruta);
        fclose(f);
        return false;
    }
    return fclose(f) == 0;
}

/* Compara lo que dice el satelite que genero con lo que esta P4 ha aplicado.
 *
 * Sin esto, un viaje al que le falte un repostaje se descargaria con pinta de
 * estar entero y el analizador del PC se lo tragaria como bueno. Es justo el
 * fallo que mas cuesta detectar despues: no falta el fichero, falta UNA LINEA.
 *
 * Devuelve cuantos faltan (0 = entero). */
static uint32_t comprobar_completo(const cJSON *j, const char *carpeta)
{
    const cJSON *je = cJSON_GetObjectItem(j, "eventos");
    char ruta[RUTA_MAX];
    snprintf(ruta, sizeof(ruta), "%s/" MARCA_INCOMPLETO, carpeta);

    if (!cJSON_IsNumber(je)) {
        /* SIN 'eventos' NO SE PUEDE SABER, y decir "no falta nada" es mentir
         * (auditoria del 13-oct-2026). Este es el camino del cierre "ajeno":
         * una cabina nueva, o con la NVS borrada, cierra un viaje que no
         * empezo ella (pantalla "VIAJE EN LA P4" -> "GUARDARLO") y su fin no
         * lleva contador. Antes se devolvia 0 = "completo", asi que el viaje
         * salia Listo, sin marca y descargable aunque le faltaran apuntes que
         * SI se generaron -- justo lo que INCOMPLETO.txt existe para evitar.
         * Ahora se deja dicho que no se pudo comprobar: no es lo mismo un viaje
         * verificado que uno sin verificar. */
        FILE *f = fopen(ruta, "w");
        if (f) {
            fprintf(f, "Este viaje se cerro SIN PODER COMPROBARLO.\n\n"
                       "El cierre lo mando una pantalla que no llevaba la cuenta de\n"
                       "los apuntes generados (se cambiaria la cabina, o se le borro\n"
                       "la memoria a mitad de viaje), asi que la P4 no puede saber si\n"
                       "falta algo.\n\n"
                       "Es probable que este entero, pero NO esta verificado: comparalo\n"
                       "con lo que recuerdes antes de darlo por bueno.\n");
            if (fsync(fileno(f)) != 0)
                ESP_LOGW(TAG, "fsync fallo en %s (disco lleno?)", ruta);
            if (fclose(f) != 0)
                ESP_LOGW(TAG, "%s no se escribio bien (disco lleno?)", ruta);
        }
        ESP_LOGW(TAG, "cierre SIN VERIFICAR de %s: el fin no traia contador de "
                      "eventos", carpeta);
        return 0;                            /* no se puede afirmar que falten */
    }

    uint32_t esperados = (uint32_t)je->valuedouble;
    uint32_t aplicados = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_T_APLIC, &aplicados);
        nvs_close(h);
    }
    aplicados++;                            /* este mismo fin */

    if (aplicados >= esperados) return 0;
    uint32_t faltan = esperados - aplicados;

    FILE *f = fopen(ruta, "w");
    if (f) {
        fprintf(f, "A este viaje le faltan %lu apunte%s.\n\n",
                (unsigned long)faltan, faltan == 1 ? "" : "s");
        fprintf(f, "La pantalla de la cabina dice haber generado %lu y aqui han\n"
                   "llegado %lu. Se perdieron por el camino: la cola se lleno, o\n"
                   "hubo un reinicio en mal momento.\n\n",
                (unsigned long)esperados, (unsigned long)aplicados);
        fprintf(f, "Lo demas es correcto. Este aviso esta para que no des el\n"
                   "viaje por entero al analizarlo.\n");
        if (fclose(f) != 0) {
            ESP_LOGW(TAG, "%s no se escribio bien (disco lleno?)", ruta);
        }
    }
    ESP_LOGW(TAG, "viaje INCOMPLETO: esperados %lu, aplicados %lu",
             (unsigned long)esperados, (unsigned long)aplicados);
    return faltan;
}

/* ── APUNTAR EL DESTINO NUEVO, SIN RENOMBRAR NADA (13-oct-2026) ──────────────
 *
 * POR QUE EXISTE, teniendo ya el "destino" del fin: el cambio tiene que
 * sobrevivir a que se apague CUALQUIERA de las tres placas. Guardarlo solo en
 * la cabina deja un hueco: si esa cabina se queda sin NVS (o se cambia por
 * otra), el nombre nuevo se pierde y el viaje se cierra con el viejo.
 *
 * Asi que la cabina avisa EN CUANTO cambia el nombre y la P4 lo APUNTA. No
 * renombra nada: la carpeta se sigue llamando igual y los ficheros que se estan
 * escribiendo no se tocan -- el renombrado sigue siendo cosa del cierre, que es
 * lo que se decidio el 12-oct. Esto solo deja el nombre a salvo en la P4.
 *
 * El fin manda el suyo si lo trae; si no, se usa este. Y al empezar un viaje se
 * borra, porque el cambio era del viaje anterior. */
static esp_err_t op_destino(httpd_req_t *req, const cJSON *j, uint32_t id)
{
    (void)id;
    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) {
        /* Sin viaje abierto no hay a que cambiarle el nombre. 409 y no 200: la
         * cabina lo reintenta por su cola, y si el viaje ya se cerro, el fin
         * que venga detras lo resolvera. */
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "no hay viaje abierto");
        return ESP_OK;
    }

    const cJSON *jd = cJSON_GetObjectItemCaseSensitive(j, "destino");
    if (!cJSON_IsString(jd)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "falta 'destino'");
        return ESP_OK;
    }
    char nd[DESTINO_MAX];
    destino_seguro(jd->valuestring, nd, sizeof(nd));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "no puedo guardar el destino");
        return ESP_OK;
    }
    esp_err_t e = nvs_set_str(h, NVS_DESTINO, nd);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "no puedo guardar el destino");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "destino nuevo APUNTADO (sin renombrar): %s -> %s", carpeta, nd);
    httpd_resp_sendstr(req, nd);
    return ESP_OK;
}

static esp_err_t op_fin(httpd_req_t *req, const cJSON *j, uint32_t id)
{
    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) {
        /* Sin viaje abierto el fin ya no tiene nada que cerrar. Se responde OK
         * y no error: casi siempre sera un reintento de un fin que SI se
         * aplico, y devolver error dejaria al satelite atascado reintentando
         * algo que ya esta hecho. */
        ESP_LOGW(TAG, "fin sin viaje abierto: lo doy por hecho");
        httpd_resp_sendstr(req, "no habia viaje abierto");
        return ESP_OK;
    }

    if (!camera_sd_bus_lock(3000)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "tarjeta ocupada");
        return ESP_OK;
    }
    /* La carpeta del viaje abierto puede NO ESTAR: se saco la tarjeta y se metio
     * otra, se formateo, o se borro desde el PC. Antes esto se descubria por las
     * malas -- diario() fallaba, salia un 507 generico y el viaje se quedaba
     * abierto apuntando al vacio para siempre --. Se dice lo que pasa y se deja
     * que lo arregle la reparacion de arranque (viaje_reparar_al_arrancar). */
    struct stat st_carpeta;
    if (stat(carpeta, &st_carpeta) != 0) {
        camera_sd_bus_unlock();
        ESP_LOGE(TAG, "el viaje abierto %s NO ESTA en la tarjeta: no puedo "
                      "cerrarlo. Si has cambiado o formateado la tarjeta, "
                      "reinicia la P4 y su estado se limpia solo", carpeta);
        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_sendstr(req, "la carpeta del viaje no esta en la tarjeta");
        return ESP_OK;
    }
    bool escrito = diario(carpeta, "fin", "");
    /* Lo que quede de ruta en memoria, a la tarjeta ANTES de cerrar. No condiciona
     * el cierre: si esto falla se pierden como mucho los ultimos minutos de traza,
     * y dejar el viaje abierto por eso seria peor que el mal que evita. */
    ruta_cerrar(carpeta);
    uint32_t faltan = comprobar_completo(j, carpeta);
    escrito = escribir_resumen(carpeta) && escrito;
    if (faltan) {
        FILE *rf; char rr[RUTA_MAX];
        snprintf(rr, sizeof(rr), "%s/resumen.txt", carpeta);
        rf = fopen(rr, "a");
        if (rf) {
            fprintf(rf, "\n*** VIAJE INCOMPLETO: faltan %lu apuntes. ***\n",
                    (unsigned long)faltan);
            fprintf(rf, "Los totales de arriba NO los incluyen. Ver "
                        MARCA_INCOMPLETO ".\n");
            fclose(rf);
        }
    }

    if (!escrito) {
        /* El viaje NO se cierra: sin resumen no esta cerrado del todo, y
         * cerrarlo aqui dejaria al usuario creyendo que si. 507 es 5xx, asi que
         * el satelite reintentara el fin; mientras tanto el viaje sigue abierto
         * en la P4 y, si se empieza otro, sale la pantalla de "la P4 tiene un
         * viaje abierto" para decidir.
         *
         * EL RENOMBRADO DEL CAMBIO DE DESTINO VA DESPUES DE ESTO, no antes
         * (auditoria del 13-oct-2026). Antes se renombraba la carpeta aqui
         * arriba y solo despues se miraba si el cierre habia entrado: con la
         * tarjeta llena, la P4 renombraba la carpeta, contestaba 507 y el viaje
         * seguia ABIERTO en NVS apuntando a la ruta vieja, que ya no existia.
         * A partir de ahi no habia salida: los reintentos del fin escribian en
         * una carpeta inexistente (507 otra vez), los apuntes de la cabina
         * tambien (la cola se atascaba entera), un viaje nuevo daba 409, y ni
         * descartar ni borrar funcionaban. Solo se arreglaba borrando la NVS de
         * la P4 a mano. */
        camera_sd_bus_unlock();
        ESP_LOGE(TAG, "NO he podido escribir el cierre de %s: la tarjeta esta "
                      "llena o falla. El viaje sigue ABIERTO", carpeta);
        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_sendstr(req, "no se ha podido escribir el cierre");
        return ESP_OK;
    }

    /* El cierre YA ESTA en la tarjeta. El estado se limpia ANTES de renombrar:
     * asi el nombre de la carpeta y lo que la P4 cree que hay abierto no pueden
     * quedar desalineados, ni siquiera si se corta la corriente en medio. Si el
     * corte llega entre el commit y el rename, el viaje queda cerrado con el
     * nombre viejo, que es un estado perfectamente valido (los datos y el
     * resumen estan dentro). */
    if (estado_set(NULL, id) != ESP_OK) {
        camera_sd_bus_unlock();
        ESP_LOGE(TAG, "cierre de %s escrito, pero no he podido olvidarlo en NVS: "
                      "el viaje sigue abierto y habra que reintentar", carpeta);
        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_sendstr(req, "no se ha podido cerrar el viaje en la NVS");
        return ESP_OK;
    }

    /* ── CAMBIO DE DESTINO APLAZADO AL CIERRE (12-oct-2026) ─────────────────
     *
     * El satelite deja cambiar el destino a mitad de viaje, pero NO renombra
     * nada mientras corre: guarda el destino nuevo y lo manda AQUI, al cerrar.
     * Es a proposito y esta razonado en cabina5/docs/RELEVO_2026-10-12.md:
     * mientras el viaje esta abierto hay ficheros escribiendose (ruta,
     * telemetria) y renombrar la carpeta por debajo obligaria a cerrarlos,
     * renombrar y reabrirlos, dependiendo ademas de que la P4 contestara en ese
     * momento. Al cerrar, los ficheros ya se cierran de todas formas y el
     * resumen ya esta escrito: renombrar aqui es un solo rename() y no hay
     * nada dentro de la carpeta que apunte a su nombre (todo va por fecha_hora).
     *
     * La FECHA del nombre se conserva: el viaje empezo ese dia, aunque el
     * destino se cambiara despues. Solo cambia lo que va detras del guion. */
    const cJSON *jd = cJSON_GetObjectItemCaseSensitive(j, "destino");
    char nd[DESTINO_MAX] = {0};
    if (cJSON_IsString(jd) && jd->valuestring[0]) {
        destino_seguro(jd->valuestring, nd, sizeof(nd));
    } else {
        /* El fin no lo trae: se usa el que la cabina apunto al cambiarlo. Es el
         * camino que cubre que la cabina se haya quedado sin su NVS. */
        nvs_handle_t hn;
        if (nvs_open(NVS_NS, NVS_READONLY, &hn) == ESP_OK) {
            size_t nn = sizeof(nd);
            if (nvs_get_str(hn, NVS_DESTINO, nd, &nn) != ESP_OK) nd[0] = 0;
            nvs_close(hn);
        }
        if (nd[0]) ESP_LOGI(TAG, "fin sin destino: uso el apuntado antes (%s)", nd);
    }
    if (nd[0]) {

        char fecha[12] = {0};
        const char *base = strrchr(carpeta, '/');
        base = base ? base + 1 : carpeta;          /* "2026-10-11_arantzazu" */
        const char *guion = strchr(base, '_');
        size_t nf = guion ? (size_t)(guion - base) : 0;
        if (nf >= sizeof(fecha)) nf = sizeof(fecha) - 1;
        memcpy(fecha, base, nf);
        fecha[nf] = 0;

        char nueva[RUTA_MAX];
        int nn = snprintf(nueva, sizeof(nueva), VIAJES_DIR "/%s_%s", fecha, nd);
        /* Si el destino no ha cambiado de verdad, no se toca nada. */
        if (nn > 0 && nn < (int)sizeof(nueva) && strcmp(nueva, carpeta) != 0) {
            /* rename() falla si el destino ya existe: se numera, igual que al
             * apartar un viaje (op_borrar). */
            struct stat st;
            for (int k = 2; k < 100 && stat(nueva, &st) == 0; k++) {
                nn = snprintf(nueva, sizeof(nueva), VIAJES_DIR "/%s_%s_%d",
                              fecha, nd, k);
                if (nn < 0 || nn >= (int)sizeof(nueva)) break;
            }
            if (rename(carpeta, nueva) == 0) {
                ESP_LOGI(TAG, "DESTINO CAMBIADO: %s -> %s", carpeta, nueva);
            } else {
                /* El viaje se cierra igual: el resumen y los datos estan bien,
                 * solo se queda con el nombre viejo. Perder el cierre por no
                 * poder renombrar seria mucho peor. */
                ESP_LOGW(TAG, "no he podido renombrar %s a %s: se queda con el "
                              "nombre viejo", carpeta, nueva);
            }
        }
    }
    camera_sd_bus_unlock();

    /* La energia ya esta volcada en el resumen de arriba. Se guarda en NVS y se
     * cierra, para que los contadores no sigan sumando a cuenta de un viaje que
     * ya termino. */
    trip_computer_end();
    ESP_LOGI(TAG, "VIAJE CERRADO: %s", carpeta);
    httpd_resp_sendstr(req, carpeta);
    return ESP_OK;
}

/* APARTAR el viaje abierto, sin cerrarlo como bueno.
 *
 * Para cuando se empezo un viaje por error o de prueba: la carpeta pasa a
 * llamarse DESCARTADO_<nombre> y deja de contar como viaje. NO se borra --
 * un viaje entero perdido por un dedazo no se recupera, y renombrar cuesta lo
 * mismo. Quien quiera el hueco de la tarjeta, que lo borre a mano.
 *
 * Lo pide la 3.5" desde la cabina: cuando va a empezar un viaje y esta pantalla
 * responde 409, ofrece guardarlo o apartarlo ahi mismo. La P4 vive en la parte
 * de atras y levantarse del asiento del conductor para pulsar un boton no es
 * una opcion. */
/* Borra un arbol de ficheros/carpetas (profundidad acotada). Devuelve cuantos
 * fallos hubo. La SD se borra con opendir/readdir + remove/rmdir porque un
 * 'remove' no borra una carpeta con contenido. */
static int borrar_arbol(const char *ruta, int nivel)
{
    if (nivel > 4) return 1;
    DIR *d = opendir(ruta);
    if (!d) return (remove(ruta) == 0) ? 0 : 1;        /* era un fichero */
    int fallos = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char hijo[RUTA_MAX + 192];
        int n = snprintf(hijo, sizeof(hijo), "%s/%s", ruta, e->d_name);
        if (n < 0 || n >= (int)sizeof(hijo)) { fallos++; continue; }
        fallos += borrar_arbol(hijo, nivel + 1);
    }
    closedir(d);
    if (rmdir(ruta) != 0) fallos++;
    return fallos;
}

/* op_borrar: borra un viaje entero de la tarjeta. Lo pide la app del movil
 * (peticion del usuario, 28-sep-2026: "que los viajes tengan la posibilidad de
 * borrarse desde la apk").
 *
 * Cuerpo: {"op":"borrar","carpeta":"2026-09-27_RIBADEO"}
 *
 * Cuidados:
 *  - el nombre tiene que ser UNA sola componente (sin '/', sin '..', sin
 *    caracteres raros): lo que llega por HTTP no puede salirse de /sdcard/viajes;
 *  - el viaje EN CURSO no se borra (se esta escribiendo ahora): 409 y que se
 *    cierre antes (op_fin);
 *  - se toma el cerrojo del bus de la SD, que lo comparte con la camara. */
static esp_err_t op_borrar(httpd_req_t *req, const cJSON *j, uint32_t id)
{
    const cJSON *jc = cJSON_GetObjectItemCaseSensitive(j, "carpeta");
    if (!cJSON_IsString(jc) || !jc->valuestring[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "falta 'carpeta'");
        return ESP_OK;
    }
    const char *nombre = jc->valuestring;
    if (strlen(nombre) > 96 || strchr(nombre, '/') || strstr(nombre, "..")) {
        ESP_LOGW(TAG, "borrar: nombre rechazado (%.40s)", nombre);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "nombre de carpeta no valido");
        return ESP_OK;
    }
    char abierto[CARPETA_MAX];
    if (viaje_abierto(abierto, sizeof(abierto))) {
        const char *base = strrchr(abierto, '/');
        base = base ? base + 1 : abierto;
        if (!strcmp(base, nombre)) {
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_sendstr(req, "ese viaje esta en curso: cierralo antes de borrarlo");
            return ESP_OK;
        }
    }
    char ruta[RUTA_MAX + 64];
    int n = snprintf(ruta, sizeof(ruta), VIAJES_DIR "/%s", nombre);
    if (n < 0 || n >= (int)sizeof(ruta)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "nombre demasiado largo");
        return ESP_OK;
    }
    if (!camera_sd_bus_lock(3000)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "tarjeta ocupada, reintenta");
        return ESP_OK;
    }
    struct stat st;
    if (stat(ruta, &st) != 0) {
        camera_sd_bus_unlock();
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr(req, "ese viaje no esta en la tarjeta");
        return ESP_OK;
    }
    const int fallos = borrar_arbol(ruta, 0);
    camera_sd_bus_unlock();
    if (fallos) {
        ESP_LOGW(TAG, "borrar viaje '%s': %d fallo(s)", nombre, fallos);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "no se pudo borrar todo (tarjeta ocupada?)");
        return ESP_OK;
    }
    ESP_LOGW(TAG, "viaje borrado desde la app: %s", nombre);
    httpd_resp_sendstr(req, "viaje borrado");
    return ESP_OK;
}

static esp_err_t op_descartar(httpd_req_t *req, const cJSON *j, uint32_t id)
{
    (void)j;
    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) {
        /* Mismo criterio que op_fin: casi siempre es un reintento de algo que
         * ya se aplico, y un error dejaria al satelite atascado. */
        ESP_LOGW(TAG, "descartar sin viaje abierto: lo doy por hecho");
        httpd_resp_sendstr(req, "no habia viaje abierto");
        return ESP_OK;
    }

    const char *nombre = strrchr(carpeta, '/');
    nombre = nombre ? nombre + 1 : carpeta;

    if (!camera_sd_bus_lock(3000)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "tarjeta ocupada");
        return ESP_OK;
    }

    char destino[RUTA_MAX + 32];
    /* 'nombre' sale de la carpeta del viaje (64 max), asi que no puede no caber,
     * pero se comprueba: si algun dia no cupiera, un snprintf recortado
     * renombraria a OTRA ruta y aqui se estan moviendo carpetas de la SD. */
    int n_dst = snprintf(destino, sizeof(destino),
                         VIAJES_DIR "/" MARCA_DESCARTADO "%s", nombre);
    if (n_dst < 0 || n_dst >= (int)sizeof(destino)) {
        camera_sd_bus_unlock();
        ESP_LOGE(TAG, "nombre de viaje demasiado largo para descartar: %.32s", nombre);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "nombre demasiado largo");
        return ESP_OK;
    }
    /* rename() falla si el destino ya existe, y con dos pruebas del mismo dia
     * eso pasa a la primera. Se numera en vez de dar error. */
    struct stat st;
    for (int k = 2; k < 100 && stat(destino, &st) == 0; k++) {
        n_dst = snprintf(destino, sizeof(destino),
                         VIAJES_DIR "/" MARCA_DESCARTADO "%s_%d", nombre, k);
        if (n_dst < 0 || n_dst >= (int)sizeof(destino)) break;
    }

    diario(carpeta, "descartado", "");
    int r = rename(carpeta, destino);

    if (r != 0) {
        camera_sd_bus_unlock();
        ESP_LOGE(TAG, "no puedo apartar %s -> %s", carpeta, destino);
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "no puedo apartar la carpeta");
        return ESP_OK;
    }

    /* El estado se limpia DESPUES del rename (aqui si, al contrario que en
     * op_fin) porque un rename fallido tiene que dejar el viaje como estaba,
     * para poder reintentarlo. Pero si el commit de la NVS falla, se DESHACE el
     * rename: dejarlo renombrado con el viaje aun abierto es exactamente el
     * estado sin salida de la auditoria del 13-oct-2026 (NVS apuntando a una
     * carpeta que ya no existe). */
    if (estado_set(NULL, id) != ESP_OK) {
        if (rename(destino, carpeta) != 0)
            ESP_LOGE(TAG, "no he podido deshacer el renombrado de %s", destino);
        camera_sd_bus_unlock();
        ESP_LOGE(TAG, "no puedo apartar %s: la NVS no acepta el cambio", carpeta);
        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_sendstr(req, "no puedo apartar el viaje");
        return ESP_OK;
    }
    camera_sd_bus_unlock();

    ESP_LOGW(TAG, "VIAJE DESCARTADO: %s -> %s", carpeta, destino);
    httpd_resp_sendstr(req, destino);
    return ESP_OK;
}

/* Una fila al CSV del tipo. Se escribe ADEMAS del diario porque sirven para
 * cosas distintas: eventos.csv se lee para saber que paso, y estos para hacer
 * cuentas en una hoja de calculo.
 *
 * La cabecera sale de las CLAVES que manda el satelite, no de una lista fija
 * aqui: tenerla en los dos sitios seria garantizar que algun dia dejan de
 * coincidir, y las columnas se desalinearian en silencio. Por eso, al añadir,
 * se relee la primera linea y se compara: si el firmware del satelite ha
 * cambiado los campos, se escribe una cabecera nueva en vez de meter los
 * valores bajo las columnas de antes. Cuesta leer ~200 bytes por apunte, y los
 * apuntes son unos pocos al dia. */
/* Devuelve si la fila ha llegado a la tarjeta; ver la nota de diario_en(). */
static bool csv_por_tipo(const char *carpeta, const char *tipo,
                         const cJSON *datos, const char *cuando)
{
    char ruta[RUTA_MAX];
    snprintf(ruta, sizeof(ruta), "%s/%ss.csv", carpeta, tipo);

    /* Cabecera esperada a partir de las claves recibidas. */
    char cab[512];
    int p = snprintf(cab, sizeof(cab), "fecha_hora");
    for (const cJSON *it = datos ? datos->child : NULL; it; it = it->next) {
        if (!it->string) continue;
        p += snprintf(cab + p, sizeof(cab) - p, ",%s", it->string);
        if (p >= (int)sizeof(cab) - 1) break;
    }

    bool poner_cab = true;
    FILE *f = fopen(ruta, "r");
    if (f) {
        char primera[512] = {0};
        if (fgets(primera, sizeof(primera), f)) {
            primera[strcspn(primera, "\r\n")] = 0;
            if (strcmp(primera, cab) == 0) poner_cab = false;
        }
        fclose(f);
    }

    f = fopen(ruta, "a");
    if (!f) { ESP_LOGW(TAG, "no puedo escribir %s", ruta); return false; }
    if (poner_cab) fprintf(f, "%s\n", cab);

    fprintf(f, "%s", cuando);
    for (const cJSON *it = datos ? datos->child : NULL; it; it = it->next) {
        fputc(',', f);
        /* El texto pasa por csv_texto(): una coma en el campo libre "otros" del
         * mantenimiento metia una columna de mas y desplazaba la fila entera
         * (auditoria del 13-oct-2026). */
        if      (cJSON_IsString(it)) csv_texto(f, it->valuestring);
        else if (cJSON_IsNumber(it)) fprintf(f, "%g", it->valuedouble);
    }
    fputc('\n', f);
    /* Mismo motivo que diario_en(): fclose es quien de verdad delata un
     * disco lleno, los fprintf de arriba solo llenan un buffer. Y fsync antes,
     * para que no se quede en la cache de la tarjeta despues de haberle dicho
     * al satelite que estaba guardado. */
    if (fsync(fileno(f)) != 0) {
        ESP_LOGW(TAG, "fsync fallo en %s (disco lleno?)", ruta);
        fclose(f);
        return false;
    }
    return fclose(f) == 0;
}

/* El sello de tiempo lo pone el SATELITE, en el momento en que ocurrio, y no
 * la P4 al recibirlo: un apunte puede haber esperado dias en su cola. Solo si
 * viene marcado como aproximado (el satelite nunca vio la hora) se usa la de
 * recepcion, y queda dicho en la columna 'hora_aprox'. */
static esp_err_t op_registro(httpd_req_t *req, const cJSON *j, uint32_t id)
{
    const cJSON *jt = cJSON_GetObjectItem(j, "tipo");
    const cJSON *jd = cJSON_GetObjectItem(j, "datos");
    if (!cJSON_IsString(jt)) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "falta tipo");
        return ESP_OK;
    }

    /* Sin viaje abierto el apunte NO se rechaza: se apunta al historial del
     * vehiculo en vez de perderse.
     *
     * Antes esto era un 409 "no hay viaje abierto", pensado para cuando el
     * inicio del viaje seguia en la cola del satelite por delante de este
     * apunte. Con el cuaderno reorganizado por SALIDAS dejo de valer para las
     * puntuales de entonces (sin inicio propio), y el 409 dejaba a la cola
     * reintentando el mismo apunte para siempre (viaje_cola.c reintenta los
     * 409 a proposito) y detras se atascaba todo lo demas.
     *
     * Desde que la 35cabina trata una salida puntual como un viaje de una
     * sola parada (con su propio op_inicio antes de declarar nada, ver
     * puntual_declarar_cb en view_registro.c), en_viaje sera casi siempre
     * true tambien para una puntual -- VEHICULO_DIR queda como red de
     * seguridad para un apunte que de verdad llegue sin viaje abierto
     * (satelite mas antiguo, o el inicio se perdio por el camino), no como
     * el destino habitual de las puntuales. */
    char carpeta[CARPETA_MAX];
    bool en_viaje = viaje_abierto(carpeta, sizeof(carpeta));
    if (!en_viaje) snprintf(carpeta, sizeof(carpeta), VEHICULO_DIR);

    /* Cuando ocurrio. */
    const cJSON *jts = cJSON_GetObjectItem(j, "ts");
    const cJSON *ja  = cJSON_GetObjectItem(j, "aprox");
    bool aprox = cJSON_IsTrue(ja) || !cJSON_IsNumber(jts) || jts->valuedouble < 1e9;
    time_t t = aprox ? time(NULL) : (time_t)jts->valuedouble;
    struct tm tm_l;
    /* El ts del satelite ya viene en hora LOCAL (deriva de epoch_local), asi
     * que gmtime; la hora de recepcion, en cambio, es un epoch de verdad y
     * necesita localtime. Mezclarlos correria una de las dos dos horas. */
    if (aprox) localtime_r(&t, &tm_l); else gmtime_r(&t, &tm_l);
    char cuando[20];
    strftime(cuando, sizeof(cuando), "%Y-%m-%d %H:%M:%S", &tm_l);

    const cJSON *jr = cJSON_GetObjectItem(j, "resumen");
    char detalle[128];
    snprintf(detalle, sizeof(detalle), "%s%s",
             cJSON_IsString(jr) ? jr->valuestring : "",
             aprox ? " (hora aproximada)" : "");

    if (!camera_sd_bus_lock(3000)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, "tarjeta ocupada");
        return ESP_OK;
    }
    if (!en_viaje) mkdir(VEHICULO_DIR, 0777);
    /* MARCA DE AGUA de los dos ficheros ANTES de tocarlos: si algo falla
     * despues (la tarjeta o la NVS), se deshace lo escrito y el reintento del
     * satelite entra limpio, sin duplicar la linea (auditoria del 13-oct-2026:
     * antes el reintento volvia a escribir lo que ya habia entrado, y el
     * analizador suma filas, no ids). */
    char ruta_ev[RUTA_MAX], ruta_tipo[RUTA_MAX];
    snprintf(ruta_ev, sizeof(ruta_ev), "%s/eventos.csv", carpeta);
    snprintf(ruta_tipo, sizeof(ruta_tipo), "%s/%ss.csv", carpeta, jt->valuestring);
    off_t marca_ev = marca_de(ruta_ev), marca_tipo = marca_de(ruta_tipo);

    bool escrito = diario_en(carpeta, cuando, jt->valuestring, detalle);
    escrito = csv_por_tipo(carpeta, jt->valuestring, jd, cuando) && escrito;
    if (!escrito) {
        marca_deshacer(ruta_ev, marca_ev);
        marca_deshacer(ruta_tipo, marca_tipo);
    }
    camera_sd_bus_unlock();

    if (!escrito) {
        /* 507 y no 200: es 5xx, asi que el satelite NO lo descarta y lo vuelve
         * a intentar. Tampoco se cuenta ni se suma a los totales ni se marca el
         * id como aplicado, para que el reintento entre de verdad. */
        ESP_LOGE(TAG, "NO he podido escribir el apunte '%s' en %s: la tarjeta "
                      "esta llena o falla. Devuelvo 507 para que lo reintente",
                 jt->valuestring, carpeta);
        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_sendstr(req, "no se ha podido escribir en la tarjeta");
        return ESP_OK;
    }

    /* Los totales, el contador de aplicados y la marca de dedupe (el
     * anillo de estado_set) van AHORA en el MISMO handle y un unico
     * nvs_commit, no en tres aperturas independientes como antes. Con tres
     * commits separados, un apagon justo entre el de totales y el del
     * anillo dejaba el importe ya sumado y el contador ya incrementado
     * pero el id SIN marcar como aplicado: el satelite nunca recibio el
     * 200 (se corto antes), reintenta el mismo id, id_ya_aplicado() no lo
     * encuentra en el anillo (nunca se comiteo) y lo vuelve a aplicar
     * ENTERO -- importe duplicado en el resumen, en silencio, porque
     * aplicados sigue >= esperados (los dos contadores suben juntos). Con
     * un solo commit, el mismo apagon deja TODO sin aplicar (el handle
     * nunca llega a comitearse) y el reintento entra limpio, sin duplicar
     * nada -- todo o nada, no "unos si y el id no". Detectado el
     * 09-sep-2026 (mismo criterio que salida/trip_blob en el satelite). */
    nvs_handle_t h;
    esp_err_t commit_err = ESP_FAIL;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        /* Los totales y el contador son DEL VIAJE: un repostaje del
         * historial del vehiculo no cuenta en el resumen de ningun viaje,
         * y sumarlo al contador haria que el viaje siguiente pareciese
         * completo con un apunte de menos. */
        if (en_viaje) {
            totales_sumar_en(h, jt->valuestring, jd);
            uint32_t n = 0;
            nvs_get_u32(h, NVS_T_APLIC, &n);
            nvs_set_u32(h, NVS_T_APLIC, n + 1);
        }
        /* NULL y no 'carpeta' si no hay viaje: esto guarda cual es el
         * viaje ABIERTO, y el historial del vehiculo no lo es. Pasarlo
         * abriria un viaje fantasma en /sdcard/vehiculo. El id si se
         * guarda siempre: es la idempotencia. */
        estado_set_en(h, en_viaje ? carpeta : NULL, id);
        commit_err = nvs_commit(h);
        nvs_close(h);
    }

    if (commit_err != ESP_OK) {
        /* ANTES ESTO CONTESTABA 200 (auditoria del 13-oct-2026). La fila ya
         * estaba en la tarjeta, asi que el satelite la daba por entregada y la
         * borraba de su cola... pero los totales del resumen, el contador de
         * aplicados y el anillo de dedupe NO habian quedado guardados: el
         * resumen salia por debajo de lo que dicen los CSV y, encima,
         * comprobar_completo() marcaba el viaje INCOMPLETO culpando a "apuntes
         * perdidos por el camino", cuando el dato estaba en la tarjeta.
         *
         * Ahora se DESHACE la fila (marca_deshacer) y se contesta 507: el
         * satelite lo reintenta y entra entero, una sola vez. */
        ESP_LOGE(TAG, "nvs_commit fallo para totales/contador/dedupe de '%s' "
                      "(%s): deshago la fila de la SD y devuelvo 507 para que "
                      "el reintento lo aplique ENTERO",
                 jt->valuestring, esp_err_to_name(commit_err));
        if (camera_sd_bus_lock(3000)) {
            marca_deshacer(ruta_ev, marca_ev);
            marca_deshacer(ruta_tipo, marca_tipo);
            camera_sd_bus_unlock();
        }
        httpd_resp_set_status(req, "507 Insufficient Storage");
        httpd_resp_sendstr(req, "no se ha podido guardar el apunte en la NVS");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "apunte '%s' guardado en %s", jt->valuestring, carpeta);
    httpd_resp_sendstr(req, "ok");
    return ESP_OK;
}

/* ── Telemetria y contadores mientras dura el viaje ───────────────────────── */

/* Cada 5 min una fila de telemetria; cada hora una de contadores. Cinco minutos
 * dan 288 filas al dia, que es lo mismo que ya usa el historico del frigo y
 * pinta una grafica decente sin llenar la tarjeta. */
#define TELEMETRIA_MIN   5
#define CONTADORES_MIN  60
/* La ruta se apunta mucho mas a menudo que la telemetria porque son cosas
 * distintas: la telemetria describe un ESTADO que cambia despacio (bateria,
 * temperaturas), y la ruta describe un CAMINO. A 5 min y 100 km/h habria 8 km
 * entre punto y punto: el mapa saldria cortando las curvas y los pueblos, mas
 * una cuerda floja que un recorrido. */
#define RUTA_SEG        30
/* Metros que hay que haberse movido para apuntar otro punto. Parado no se
 * apunta NADA: si no, una noche de camping deja 2.800 filas identicas. */
#define RUTA_SALTO_KM   0.02

static void fila_telemetria(const char *carpeta, const struct tm *tm_l)
{
    char ruta[RUTA_MAX];
    char dia[12];
    strftime(dia, sizeof(dia), "%Y-%m-%d", tm_l);
    snprintf(ruta, sizeof(ruta), "%s/telemetria_%s.csv", carpeta, dia);

    struct stat st;
    bool nuevo = !(stat(ruta, &st) == 0 && st.st_size > 0);

    FILE *f = fopen(ruta, "a");
    if (!f) {
        /* Antes era un 'return' mudo: la telemetria del viaje se quedaba
         * congelada sin una linea en el log (auditoria del 13-oct-2026). */
        ESP_LOGW(TAG, "no puedo escribir %s (disco lleno?)", ruta);
        return;
    }
    if (nuevo) fprintf(f, "hora,soc,bateria_v,solar_w,frigo_c,exterior_c,fan_pct,agua_limpia,agua_grises\n");

    dashboard_snapshot_t d;
    dashboard_state_snapshot(&d);
    frigo_state_t fr;
    frigo_get_state_copy(&fr);
    ne185_data_t ne;
    ne185_get(&ne);

    char hora[9];
    strftime(hora, sizeof(hora), "%H:%M:%S", tm_l);
    fprintf(f, "%s,", hora);
    /* bat_fresh y solar_fresh, NO bat_has/solar_has. Esto se escribe cada 5
     * minutos EN EL FICHERO DEL VIAJE, y "has" solo dice que alguna vez hubo
     * dato: con el SmartShunt apagado, el CSV se llenaria del ultimo porcentaje
     * repetido hora tras hora. Un hueco vacio se entiende; una lectura
     * inventada se cree, y encima meses despues (auditoria del 24-ago-2026). */
    if (d.bat_fresh) fprintf(f, "%u.%u,%u.%02u,", d.soc_deci / 10, d.soc_deci % 10,
                             d.bat_v_centi / 100, d.bat_v_centi % 100);
    else             fprintf(f, ",,");
    if (d.solar_fresh) fprintf(f, "%u,", d.pv_w); else fprintf(f, ",");
    fprintf(f, "%.1f,%.1f,%u,", fr.T_Congelador, fr.T_Exterior, fr.fan_percent);
    if (ne.fresh) fprintf(f, "%u,%u\n", ne.s1, ne.r1); else fprintf(f, ",\n");
    /* fsync antes de fclose: sin el, un corte de corriente en los segundos
     * siguientes deja la fila en la cache de la tarjeta (auditoria del
     * 13-oct-2026: no habia ni un fsync en el firmware). */
    if (fsync(fileno(f)) != 0) ESP_LOGW(TAG, "fsync fallo en %s (disco lleno?)", ruta);
    if (fclose(f) != 0) ESP_LOGW(TAG, "%s no se escribio bien (disco lleno?)", ruta);
}

static void fila_contadores(const char *carpeta, const struct tm *tm_l)
{
    char ruta[RUTA_MAX];
    snprintf(ruta, sizeof(ruta), "%s/contadores.csv", carpeta);

    struct stat st;
    bool nuevo = !(stat(ruta, &st) == 0 && st.st_size > 0);

    FILE *f = fopen(ruta, "a");
    if (!f) {
        /* Antes esto era un 'return' mudo: la telemetria del viaje se congelaba
         * sin una sola linea en el log y nadie se enteraba (auditoria del
         * 13-oct-2026). */
        ESP_LOGW(TAG, "no puedo escribir %s (disco lleno?)", ruta);
        return;
    }
    /* "km" va LA ULTIMA, como dejo dicho la version anterior: asi los ficheros
     * de viajes ya cerrados se siguen leyendo igual y quien tenga un analizador
     * hecho no tiene que tocarlo.
     *
     * Ojo con el unico caso raro: un viaje que estuviera ABIERTO justo al
     * actualizar ya tiene su cabecera escrita sin la columna, y las filas
     * nuevas llevaran un campo de mas. Se asume: no se reescribe una cabecera a
     * mitad de fichero, que seria peor. */
    if (nuevo) fprintf(f, "fecha_hora,horas_activo,wh_cargados,wh_gastados,wh_solar,horas_sol,km\n");

    trip_computer_t t;
    trip_computer_get(&t);
    char cuando[20];
    strftime(cuando, sizeof(cuando), "%Y-%m-%d %H:%M:%S", tm_l);
    fprintf(f, "%s,%.2f,%.0f,%.0f,%.0f,%.2f,%.1f\n", cuando,
            (double)t.seconds_running / 3600.0, t.wh_charged, t.wh_discharged,
            t.wh_solar, (double)t.solar_seconds / 3600.0, t.km);
    /* fsync antes de fclose: sin el, un corte de corriente en los segundos
     * siguientes deja la fila en la cache de la tarjeta (auditoria del
     * 13-oct-2026: no habia ni un fsync en el firmware). */
    if (fsync(fileno(f)) != 0) ESP_LOGW(TAG, "fsync fallo en %s (disco lleno?)", ruta);
    if (fclose(f) != 0) ESP_LOGW(TAG, "%s no se escribio bien (disco lleno?)", ruta);
}

/* Los puntos de la ruta se juntan en RAM y bajan a la tarjeta de golpe.
 *
 * Cada apertura de un fichero en FAT reescribe ademas la tabla de asignacion y
 * la entrada del directorio, SIEMPRE en los mismos sectores. Escribir un punto
 * cada 30 s serian unas mil aperturas en un dia de carretera, y encima seria el
 * unico sitio del proyecto que va a pelo: el datalogger y el historico de
 * bateria acumulan y vuelcan una vez por minuto, y el ne185_vlog aguanta hasta
 * diez. Con el buffer, un dia de carretera son ~100 aperturas en vez de ~1000.
 *
 * Lo que se arriesga a cambio: un corte de corriente a lo bruto se lleva los
 * puntos que aun no han bajado, como mucho los ultimos RUTA_BUFFER_N. Es la
 * misma apuesta que ya hace la telemetria, y perder cinco minutos de traza no
 * estropea el recorrido. */
#define RUTA_BUFFER_N   10      /* 10 x 30 s = 5 min de traza en RAM (~560 bytes) */

typedef struct {
    char   cuando[20];
    double lat, lon;
    float  alt;
    double km;
} ruta_punto_t;

static ruta_punto_t s_ruta[RUTA_BUFFER_N];
static int          s_ruta_n;

/* Baja a la tarjeta lo que haya pendiente. El caller tiene que traer ya tomado
 * el cerrojo de la SD. Devuelve si ha quedado todo escrito. */
static bool ruta_volcar(const char *carpeta)
{
    if (s_ruta_n == 0) return true;

    char ruta[RUTA_MAX];
    snprintf(ruta, sizeof(ruta), "%s/ruta.csv", carpeta);
    struct stat st;
    bool nuevo = !(stat(ruta, &st) == 0 && st.st_size > 0);

    FILE *f = fopen(ruta, "a");
    if (!f) return false;                  /* se reintenta en el siguiente tick */
    if (nuevo) fprintf(f, "fecha_hora,latitud,longitud,altitud_m,km\n");
    for (int i = 0; i < s_ruta_n; i++) {
        /* Seis decimales son ~11 cm: de sobra, y no engorda el fichero. */
        fprintf(f, "%s,%.6f,%.6f,%.0f,%.2f\n", s_ruta[i].cuando,
                s_ruta[i].lat, s_ruta[i].lon, (double)s_ruta[i].alt, s_ruta[i].km);
    }
    if (fsync(fileno(f)) != 0) { fclose(f); return false; }   /* disco lleno */
    if (fclose(f) != 0) return false;      /* tarjeta llena: NO se tiran los puntos */
    s_ruta_n = 0;
    return true;
}

/* Deja escrito en el viaje que el reloj ha dado un salto (lo llama gps.c). */
void viaje_nota_hora_corregida(int64_t delta_s)
{
    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) return;
    char txt[96];
    snprintf(txt, sizeof(txt),
             "el reloj se ha movido %lld s (lo puso el GPS); los apuntes "
             "anteriores llevan la hora vieja",
             (long long)delta_s);
    if (!camera_sd_bus_lock(3000)) return;
    diario(carpeta, "hora_corregida", txt);
    camera_sd_bus_unlock();
}

/* ── Volcado antes de un reinicio (auditoria del 13-oct-2026) ────────────────
 *
 * La P4 se reinicia sola por varias vias (la escalera del AP, una OTA, el boton
 * de Ajustes, y el reinicio por watchdog). En todas, lo que hubiera en RAM se
 * pierde: hasta 5 minutos de ruta.csv (10 puntos) y lo que no haya caido de la
 * telemetria. El viaje sobrevive en NVS y sigue donde estaba, asi que lo unico
 * que hay que hacer antes de reiniciar es BAJAR LA RUTA.
 *
 * NO se escribe resumen.txt: eso marcaria el viaje como cerrado en /data/viajes
 * (estado_de() lo da por Listo en cuanto existe) cuando en realidad sigue
 * abierto. Aqui solo se vuelca y se deja constancia en el diario.
 *
 * El reinicio por watchdog (watchdog.c) NO llama a esto a proposito: alli el
 * sistema ya viene de un cuelgue y no se puede meter I/O de tarjeta en ese
 * camino. */
void viaje_volcar_ahora(const char *motivo)
{
    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) return;
    if (!camera_sd_bus_lock(3000)) {
        ESP_LOGW(TAG, "viaje abierto y la tarjeta ocupada: no puedo volcar la "
                      "ruta antes de reiniciar");
        return;
    }
    if (!ruta_volcar(carpeta))
        ESP_LOGW(TAG, "quedaban puntos de ruta y no se han podido escribir");
    diario(carpeta, "volcado", motivo ? motivo : "volcado a peticion");
    camera_sd_bus_unlock();
    ESP_LOGW(TAG, "viaje abierto en %s: ruta volcada (%s)", carpeta,
             motivo ? motivo : "a peticion");
}

/* Lo llama quien va a reiniciar la placa (ver el comentario de arriba). */
void viaje_flush_antes_de_reiniciar(void)
{
    viaje_volcar_ahora("la P4 se reinicia con el viaje abierto");
}

/* CIERRE de la ruta: ultimo intento de bajar lo que quede en RAM y, pase lo que
 * pase, el buffer se vacia.
 * Vaciarlo SIEMPRE es el arreglo de la auditoria del 13-oct-2026. Antes, si al
 * cerrar la tarjeta estaba llena, los hasta 10 puntos (5 min de traza) se
 * quedaban en RAM; el viaje se cerraba igual y, al empezar el viaje siguiente,
 * en cuanto se llenaba el buffer otra vez, ruta_volcar() los escribia en la
 * carpeta NUEVA: puntos con la fecha, las coordenadas y los km del viaje
 * anterior dentro del viaje de hoy, y el mapa del nuevo arrancando donde acabo
 * el viejo. Perder cinco minutos de traza es mucho menos malo que mezclar dos
 * viajes. */
static void ruta_cerrar(const char *carpeta)
{
    if (s_ruta_n == 0) return;
    if (!ruta_volcar(carpeta)) {
        ESP_LOGW(TAG, "quedan %d puntos de ruta sin escribir (tarjeta llena?): "
                      "se descartan para no meterlos en el viaje siguiente",
                 s_ruta_n);
        s_ruta_n = 0;
    }
    /* El contador de km de fila_ruta() se re-ancla solo (km_ultimo vuelve a -1
     * en cuanto trip_computer se pone a cero), asi que aqui no hay nada mas. */
}

/* Al abrir un viaje: a la basura lo que hubiera quedado de uno anterior. Cubre
 * el caso que ruta_cerrar() no puede cubrir -- un viaje que no se cerro por
 * aqui (se aparto, se cayo la P4) -- y garantiza que la carpeta nueva no pueda
 * heredar ni un punto de la vieja. */
static void ruta_olvidar(void)
{
    if (s_ruta_n) ESP_LOGW(TAG, "tiro %d puntos de ruta de un viaje anterior",
                           s_ruta_n);
    s_ruta_n = 0;
}

/* Anota un punto de la ruta EN MEMORIA. Vuelca cuando se llena el buffer.
 *
 * NO decide por su cuenta si el vehiculo se mueve: se fia de los kilometros que
 * ya lleva contados trip_computer, que es quien tiene los filtros (satelites
 * minimos, deriva estando parado, saltos imposibles, re-anclaje al salir de un
 * tunel). Repetir aqui ese criterio seria tener dos versiones de la verdad que
 * acabarian discrepando. */
static void fila_ruta(const char *carpeta, const struct tm *tm_l)
{
    static double km_ultimo = -1.0;

    gps_data_t g;
    gps_get(&g);
    if (!g.hay_fix) return;

    trip_computer_t t;
    trip_computer_get(&t);
    if (t.km < km_ultimo) km_ultimo = -1.0;   /* viaje nuevo: el contador se reseteo */
    /* Primer punto del viaje: se apunta siempre, que es de donde saliste. */
    if (km_ultimo >= 0.0 && (t.km - km_ultimo) < RUTA_SALTO_KM) return;
    km_ultimo = t.km;

    if (s_ruta_n < RUTA_BUFFER_N) {
        ruta_punto_t *p = &s_ruta[s_ruta_n++];
        strftime(p->cuando, sizeof(p->cuando), "%Y-%m-%d %H:%M:%S", tm_l);
        p->lat = g.lat; p->lon = g.lon; p->alt = g.altitud_m; p->km = t.km;
    }
    if (s_ruta_n >= RUTA_BUFFER_N) ruta_volcar(carpeta);
}

/* Volcado PEDIDO desde fuera, para no meter I/O de tarjeta en la tarea de quien
 * lo pide. Lo usa la alarma de bateria baja: su evaluador corre en un lv_timer
 * (tarea de LVGL) y ahi un camera_sd_bus_lock(3000) congelaria la pantalla. */
static _Atomic bool s_volcado_pedido;
static _Atomic int  s_volcado_motivo;   /* 0 = bateria, 1 = otro */

void viaje_pedir_volcado(int motivo)
{
    s_volcado_motivo = motivo;
    s_volcado_pedido = true;
}

/* Corre desde un esp_timer, no desde la tarea del portal: la telemetria tiene
 * que seguir cayendo aunque nadie toque el portal en todo el viaje. */
static void tick_viaje_cb(void *arg)
{
    (void)arg;
    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) return;

    /* Lo pedido desde fuera va ANTES de los relojes: si la bateria se esta
     * cayendo, no se puede esperar a que la hora sea buena. */
    if (s_volcado_pedido) {
        s_volcado_pedido = false;
        viaje_volcar_ahora(s_volcado_motivo == 0
                           ? "bateria baja: puede cortarse la corriente"
                           : "volcado pedido desde la pantalla");
    }

    time_t now = time(NULL);
    if (now < 1000000000L) return;          /* sin hora buena no se apunta nada */
    struct tm tm_l;
    localtime_r(&now, &tm_l);

    static int64_t ultima_tel = 0, ultimo_cont = 0, ultima_ruta = 0;
    int64_t ahora_us = esp_timer_get_time();

    bool toca_tel  = (ultima_tel  == 0) || (ahora_us - ultima_tel  >= (int64_t)TELEMETRIA_MIN * 60 * 1000000LL);
    bool toca_cont = (ultimo_cont == 0) || (ahora_us - ultimo_cont >= (int64_t)CONTADORES_MIN * 60 * 1000000LL);
    bool toca_ruta = (ultima_ruta == 0) || (ahora_us - ultima_ruta >= (int64_t)RUTA_SEG * 1000000LL);
    if (!toca_tel && !toca_cont && !toca_ruta) return;

    if (!camera_sd_bus_lock(3000)) return;   /* ya se apuntara al siguiente */
    /* La carpeta se vuelve a leer YA CON EL CERROJO (auditoria del 13-oct-2026):
     * op_fin cierra y RENOMBRA la carpeta precisamente con este cerrojo, asi
     * que entre la lectura de arriba y esta linea el viaje puede haberse
     * cerrado. Escribir entonces iba a una ruta que ya no existia y el fopen
     * fallaba en silencio: se perdia una fila de telemetria sin dejar rastro. */
    char carpeta_ahora[CARPETA_MAX];
    if (!viaje_abierto(carpeta_ahora, sizeof(carpeta_ahora)) ||
        strcmp(carpeta_ahora, carpeta) != 0) {
        camera_sd_bus_unlock();
        return;
    }
    if (toca_tel)  { fila_telemetria(carpeta, &tm_l); ultima_tel  = ahora_us; }
    if (toca_cont) { fila_contadores(carpeta, &tm_l); ultimo_cont = ahora_us; }
    /* El reloj de la ruta se adelanta toque o no: si estas parado, fila_ruta no
     * apunta nada y no hay por que volver a preguntarlo antes de 30 s. */
    if (toca_ruta) { fila_ruta(carpeta, &tm_l);       ultima_ruta = ahora_us; }
    /* Y se vuelca con la telemetria aunque el buffer no este lleno: asi un
     * trayecto corto no se queda esperando en RAM a que arranques otra vez, y
     * lo que se puede perder en un corte esta acotado a los mismos 5 minutos
     * que ya arriesga la telemetria. Va aqui dentro para aprovechar el cerrojo
     * que ya esta tomado. */
    if (toca_tel && !ruta_volcar(carpeta)) {
        ESP_LOGW(TAG, "ruta.csv no se ha podido volcar; los puntos siguen en RAM");
    }
    camera_sd_bus_unlock();
}

/* tick_viaje_cb hace I/O de SD real (fila_telemetria/fila_contadores/
 * ruta_volcar, con camera_sd_bus_lock(3000) -- 3s de margen) cada vez que le
 * toca. La tarea de esp_timer es COMPARTIDA por todo el firmware: una SD
 * colgada a nivel hardware ahi bloquearia tambien el heap log, GPS, RTC,
 * modo noche y el feed solar del frigo, no solo esta telemetria. Mismo
 * arreglo que ya se hizo en datalogger.c y battery_history.c: el timer solo
 * NOTIFICA a una tarea dedicada, que es la unica que toca la SD de verdad.
 * Detectado auditando el 07-sep-2026 (se nos habia quedado fuera de aquel
 * arreglo). */
static TaskHandle_t s_viaje_tick_task_handle;

static void viaje_tick_task(void *arg)
{
    (void)arg;
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        watchdog_heartbeat(WD_TASK_VIAJE_TICK);
        tick_viaje_cb(NULL);
    }
}

static void viaje_tick_timer_cb(void *arg)
{
    (void)arg;
    if (s_viaje_tick_task_handle) xTaskNotifyGive(s_viaje_tick_task_handle);
}

TaskHandle_t viaje_tick_task_handle(void)
{
    return s_viaje_tick_task_handle;
}

void viaje_telemetria_start(void)
{
    if (s_viaje_tick_task_handle) {
        ESP_LOGW(TAG, "viaje_telemetria_start() ya estaba arrancado, ignorado");
        return;
    }

    /* 3072 causo un bootloop real en bh_flush_task (mismo patron, misma SD)
     * el 08-sep-2026: fopen/fprintf/fclose es de lo mas hambriento de pila de
     * ESP-IDF. Esta es la mas profunda de las 5 tareas gemelas: escribe
     * %.6f (coordenadas GPS de ruta.csv) ademas de varios %.1f/%.2f/%.3f/%.0f
     * mas (resumen.txt) -- el formateo de float en newlib es lo que mas pila
     * gasta de toda la cadena. Se queda en el tope (6144) en vez de bajar
     * como las otras. */
    if (xTaskCreate(viaje_tick_task, "viaje_tick_task", 6144, NULL,
                     tskIDLE_PRIORITY + 2, &s_viaje_tick_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "No se pudo crear la tarea de telemetria del viaje");
        return;
    }
    static esp_timer_handle_t t;
    const esp_timer_create_args_t args = { .callback = viaje_tick_timer_cb, .name = "viaje_tick" };
    if (esp_timer_create(&args, &t) == ESP_OK) {
        /* Se despierta cada 30 s -- el paso mas corto, el de la ruta -- y el
         * propio callback decide que toca. Antes era cada minuto; se bajo al
         * anadir ruta.csv. Los otros dos siguen igual porque no miran el tick
         * sino el tiempo transcurrido. */
        esp_timer_start_periodic(t, (uint64_t)RUTA_SEG * 1000000ULL);
        ESP_LOGI(TAG, "telemetria del viaje armada (cada %d min, contadores cada %d, ruta cada %d s)",
                 TELEMETRIA_MIN, CONTADORES_MIN, RUTA_SEG);
    } else {
        /* Sin timer la tarea se queda parada para siempre (nadie la
         * notifica): borrarla y limpiar el handle para que el guard de
         * arriba no bloquee un reintento futuro con telemetria muerta hasta
         * el proximo reboot. */
        ESP_LOGE(TAG, "No se pudo crear el timer de telemetria del viaje");
        vTaskDelete(s_viaje_tick_task_handle);
        s_viaje_tick_task_handle = NULL;
    }
}

/* ── El handler ───────────────────────────────────────────────────────────── */

esp_err_t handle_api_viaje(httpd_req_t *req)
{
    REQUIRE_AUTH_STRICT(req);

    /* 1 KB (VIAJE_BODY_MAX, definida arriba). El comentario de antes decia
     * "unos 90 bytes, cuatro numeros": eso era la fase 1. Hoy el cuerpo mas
     * grande es la PERNOCTA -- sitio, horas, noches, precio, seis servicios
     * con su importe, valoracion, pegas e inclinacion -- y son 693 bytes
     * MEDIDOS.
     *
     * Y si aun asi llega algo mas grande, se rechaza ENTERO y se dice. Antes se
     * recortaba en silencio: el JSON quedaba cortado a medias, cJSON no lo
     * parseaba, se devolvia "400 json invalido" y el satelite lo tomaba por un
     * apunte invalido y lo DESCARTABA. O sea, un dato perdido en silencio
     * disfrazado de dato malo. */
    char body[VIAJE_BODY_MAX];
    if ((int)req->content_len >= (int)sizeof(body)) {
        ESP_LOGE(TAG, "apunte de %d bytes: NO CABE en %d, se rechaza entero",
                 (int)req->content_len, (int)sizeof(body));
        httpd_resp_set_status(req, "413 Payload Too Large");
        httpd_resp_sendstr(req, "apunte demasiado largo");
        return ESP_OK;
    }
    int total = req->content_len < (int)sizeof(body) - 1
              ? req->content_len : (int)sizeof(body) - 1;
    /* Mismo patron que /save (config_server.c): tope de esperas SEGUIDAS mas
     * un plazo total absoluto. Antes este bucle no tenia ninguno de los dos
     * -- un solo timeout de socket ya lo cortaba entero (sin margen ante un
     * hipo real de Wi-Fi), y a la vez un cliente goteando bytes justo por
     * debajo del timeout del socket podia tener ocupado indefinidamente al
     * unico worker del httpd. El cuerpo cabe en VIAJE_BODY_MAX (~1 KB), asi
     * que 15s de plazo total sobra para cualquier satelite real en la misma
     * red. Detectado auditando el 08-sep-2026. */
    const int MAX_ESPERAS = 4;
    const int64_t plazo_us = 15LL * 1000000LL;
    const int64_t t0_us = esp_timer_get_time();
    int esperas = 0;
    int got = 0;
    while (got < total) {
        if (esp_timer_get_time() - t0_us > plazo_us) {
            ESP_LOGE(TAG, "/api/viaje: plazo total agotado con %d/%d bytes, se corta", got, total);
            got = 0;   /* cuerpo a medias: mejor un JSON vacio que uno truncado */
            break;
        }
        int r = httpd_req_recv(req, body + got, total - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++esperas <= MAX_ESPERAS) continue;
        if (r <= 0) break;
        esperas = 0;
        got += r;
    }
    body[got] = 0;

    httpd_resp_set_type(req, "text/plain; charset=utf-8");

    cJSON *j = cJSON_Parse(body);
    if (!j) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "json invalido");
        return ESP_OK;
    }

    const cJSON *jop = cJSON_GetObjectItem(j, "op");
    const cJSON *jid = cJSON_GetObjectItem(j, "id");
    const cJSON *jc  = cJSON_GetObjectItemCaseSensitive(j, "carpeta");
    /* Que campos pide cada operacion lo decide contrato_app.c, que se prueba sin
     * placa en el CI (job contrato_app). Aqui nacio el fallo del 27-sep-2026:
     * esta guarda exigia "id" a TODAS las ops, y "borrar" (la app) lleva
     * "carpeta" y no lleva id -> 400 "faltan op o id" y el borrado no funcionaba.
     * "borrar" necesita "carpeta"; las del satelite (inicio/fin/registro/
     * descartar), "id" numerico. */
    const char *falta = viaje_campos_que_faltan(
        cJSON_IsString(jop) ? jop->valuestring : "",
        cJSON_IsString(jc) && jc->valuestring[0],
        cJSON_IsNumber(jid));
    if (falta) {
        cJSON_Delete(j);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, falta);
        return ESP_OK;
    }
    uint32_t id = cJSON_IsNumber(jid) ? (uint32_t)jid->valuedouble : 0;

    /* ID 0 = NO HAY ID (revision del 13-oct-2026). La cabina puede llegar aqui
     * con 0 si su NVS falla al repartir el numero (ver next_trip_seq), y con id
     * 0 la comprobacion de duplicados de mas abajo NO se aplica (mira solo ids
     * != 0): si se perdia el 200, el reintento se aplicaba otra vez y el importe
     * se sumaba DOS veces. Los ids de verdad empiezan en 1, asi que se rechaza
     * con 400 y el satelite lo dice en pantalla ("la P4 rechaza el apunte").
     * 'borrar' (la app) no lleva id a proposito: esa se deja pasar. */
    if (id == 0 && strcmp(jop->valuestring, "borrar") != 0) {
        cJSON_Delete(j);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "id invalido (0)");
        return ESP_OK;
    }

    /* IDEMPOTENCIA. Con reintentos lo mismo puede llegar dos veces (se entrego
     * pero se perdio la respuesta). Se responde OK igualmente para que el
     * satelite lo de por entregado y no se atasque reintentando para siempre.
     *
     * EL INICIO SE EXCLUYE, y no es un descuido. El contador vive en la NVS del
     * satelite: si esa NVS se borra (un reflasheo con borrado, un cambio de
     * placa) su numeracion vuelve a 1 mientras esta P4 sigue recordando que iba
     * por el 50. Con el inicio dentro de esta comprobacion, TODO lo que llegara
     * despues se descartaria como duplicado respondiendo 200, el satelite
     * vaciaria su cola creyendolo entregado y se perderia el viaje entero sin un
     * solo mensaje de error.
     *
     * Empezar un viaje es, por definicion, empezar una numeracion nueva: el
     * inicio pasa siempre y pone el contador a cero. Un inicio repetido de
     * verdad no cuela igualmente -- op_inicio devuelve 409 si ya hay uno
     * abierto. Detectado auditando el 22-ago-2026. */
    bool es_inicio = !strcmp(jop->valuestring, "inicio");
    if (!es_inicio && id != 0 && id_ya_aplicado(id)) {
        ESP_LOGI(TAG, "id %lu ya aplicado, lo descarto", (unsigned long)id);
        cJSON_Delete(j);
        httpd_resp_sendstr(req, "duplicado");
        return ESP_OK;
    }

    esp_err_t ret;
    if      (!strcmp(jop->valuestring, "inicio")) ret = op_inicio(req, j, id);
    else if (!strcmp(jop->valuestring, "fin"))    ret = op_fin(req, j, id);
    else if (!strcmp(jop->valuestring, "destino")) ret = op_destino(req, j, id);
    else if (!strcmp(jop->valuestring, "registro")) ret = op_registro(req, j, id);
    else if (!strcmp(jop->valuestring, "descartar")) ret = op_descartar(req, j, id);
    else if (!strcmp(jop->valuestring, "borrar"))    ret = op_borrar(req, j, id);
    else {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "op? (inicio|fin|registro|descartar|borrar)");
        ret = ESP_OK;
    }

    cJSON_Delete(j);
    return ret;
}


/* ── Descarga de viajes ────────────────────────────────────────────────────
 *
 * Hasta ahora /data/viaje.tar NO era un viaje: empaquetaba el historico entero
 * (bateria + solar + frigo). El nombre mentia desde que existe. Ahora ese
 * paquete es /data/historico.tar y "viaje" significa un viaje de verdad.
 *
 * Un viaje solo se puede bajar cuando la P4 SABE que no le falta nada. El aviso
 * ya estaba escrito en el codigo del tar: "el analizador del PC se traga un
 * viaje incompleto creyendo que esta entero". */

/* V_DESCONOCIDO: no se ha podido MIRAR (la SD estaba ocupada justo en ese
 * instante). Antes eso devolvia V_EN_CURSO, que es una afirmacion: el usuario
 * leia "En curso: finalizalo en la cabina antes de bajarlo" sobre un viaje
 * CERRADO y sin boton de descarga, y lo unico que pasaba es que la camara
 * tenia el bus un segundo (auditoria del 13-oct-2026). Se dice que no se sabe y
 * que se recargue. */
typedef enum { V_EN_CURSO, V_INCOMPLETO, V_LISTO, V_DESCARTADO, V_DESCONOCIDO } estado_viaje_t;

/* Los dos stat() son I/O de SD de verdad: sin camera_sd_bus_lock aqui, un
 * listado de /data/viajes mientras la camara esta grabando arriesga la
 * misma contencion SDMMC<->GDMA que camera.h documenta (INT WDT -> reinicio).
 * Unico llamador (el listado de handle_data_viajes), asi que se protege aqui
 * dentro en vez de en cada sitio que lo use. Detectado el 09-sep-2026. */
static estado_viaje_t estado_de(const char *nombre)
{
    /* op_descartar() renombra la carpeta a DESCARTADO_<nombre> sin escribir
     * NINGUN marcador dentro (ni resumen.txt ni INCOMPLETO.txt) -- antes eso
     * hacia que cayera por defecto en V_EN_CURSO ("sin resumen: nunca se
     * cerro"), un estado FALSO: la pantalla decia "finalizalo en la cabina
     * antes de bajarlo" sobre un viaje que ya no se puede finalizar (ni
     * siquiera es el abierto), y el listado nunca ofrecia bajarlo pese a
     * que los datos siguen ahi (op_descartar los conserva a proposito).
     * El propio nombre de carpeta ya lleva la marca, no hace falta stat
     * adicional. Detectado por el usuario el 09-sep-2026. */
    if (!strncmp(nombre, MARCA_DESCARTADO, strlen(MARCA_DESCARTADO))) return V_DESCARTADO;
    char abierto[CARPETA_MAX];
    if (viaje_abierto(abierto, sizeof(abierto))) {
        const char *base = strrchr(abierto, '/');
        if (base && !strcmp(base + 1, nombre)) return V_EN_CURSO;
    }
    if (!camera_sd_bus_lock(1000)) return V_DESCONOCIDO;   /* no se ha podido mirar */
    char ruta[RUTA_MAX];
    struct stat st;
    snprintf(ruta, sizeof(ruta), VIAJES_DIR "/%s/" MARCA_INCOMPLETO, nombre);
    if (stat(ruta, &st) == 0) { camera_sd_bus_unlock(); return V_INCOMPLETO; }
    snprintf(ruta, sizeof(ruta), VIAJES_DIR "/%s/resumen.txt", nombre);
    bool listo = (stat(ruta, &st) == 0);
    camera_sd_bus_unlock();
    return listo ? V_LISTO : V_EN_CURSO;      /* sin resumen: nunca se cerro */
}

/* El escape HTML de nombres (html_escape) se movio a config_server_internal.h
 * para compartirlo con la galeria de vigilancia: ver alli el comentario. */

esp_err_t handle_data_viajes(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr_chunk(req,
        "<!doctype html><meta charset=utf-8>"
        "<meta name=viewport content='width=device-width,initial-scale=1'>"
        "<style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;"
        "margin:0;padding:16px}h1{font-size:20px}a{color:#4FC3F7}"
        "li{margin:14px 0;list-style:none;border-left:3px solid #333;padding-left:10px}"
        ".ok{border-color:#66BB6A}.inc{border-color:#FFA726}.cur{border-color:#888}"
        ".desc{border-color:#555;opacity:.7}"
        ".e{font-size:13px;color:#aaa}</style>"
        "<h1>Viajes guardados</h1><ul>");

    /* opendir/readdir/closedir son I/O de SD real, igual que en gallery.c
     * (35cabina) o data_export_tar.c -- sin el cerrojo, listar /data/viajes
     * mientras la camara esta grabando arriesga la misma contencion SDMMC
     * que camera.h documenta. No estaba protegido; unico sitio de este
     * fichero que tocaba directorios sin pasar por camera_sd_bus_lock.
     * Detectado el 09-sep-2026. */
    if (!camera_sd_bus_lock(2000)) {
        httpd_resp_sendstr_chunk(req, "<li>SD ocupada, prueba a recargar.</li>");
        httpd_resp_sendstr_chunk(req, "</ul>");
        httpd_resp_sendstr_chunk(req, NULL);
        return ESP_OK;
    }
    DIR *d = opendir(VIAJES_DIR);
    camera_sd_bus_unlock();
    bool alguno = false;
    if (d) {
        struct dirent *ent;
        char linea[512];
        for (;;) {
            if (!camera_sd_bus_lock(1000)) break;
            ent = readdir(d);
            camera_sd_bus_unlock();
            if (!ent) break;
            if (ent->d_name[0] == '.') continue;
            alguno = true;
            estado_viaje_t e = estado_de(ent->d_name);
            /* d_name puede tener hasta 255 caracteres y el peor escape
             * ocupa 6 (&quot;): 256*6/4 = 384 se quedaba corto y el nombre
             * salia a medias. Cabe de sobra en los 20 KB de pila del httpd. */
            char nombre_esc[256 * 6];
            html_escape(ent->d_name, nombre_esc, sizeof(nombre_esc));
            if (e == V_LISTO) {
                snprintf(linea, sizeof(linea),
                    "<li class=ok><b>%s</b><div class=e>Listo</div>"
                    "<a href='" PORTAL_HEAVY_BASE "/data/viaje.tar?v=%s'>Descargar</a></li>",
                    nombre_esc, nombre_esc);
            } else if (e == V_INCOMPLETO) {
                /* Salida de emergencia: si algo se perdio para siempre, el viaje
                 * quedaria bloqueado eternamente. Se deja bajar, pero con el
                 * nombre gritando lo que es -- imposible confundirlo con uno
                 * entero al verlo en la carpeta de descargas del PC. */
                snprintf(linea, sizeof(linea),
                    "<li class=inc><b>%s</b><div class=e>INCOMPLETO: le faltan apuntes. "
                    "Lee " MARCA_INCOMPLETO " dentro.</div>"
                    "<a href='" PORTAL_HEAVY_BASE "/data/viaje.tar?v=%s&incompleto=si'>Descargar de todos modos</a></li>",
                    nombre_esc, nombre_esc);
            } else if (e == V_DESCARTADO) {
                /* op_descartar NO borra nada -- se conserva a proposito por si
                 * hace falta rescatar algo -- asi que se deja bajar igual que
                 * un viaje normal, sin exigir resumen.txt (nunca lo tendra:
                 * se aparto antes de cerrarse). */
                snprintf(linea, sizeof(linea),
                    "<li class=desc><b>%s</b><div class=e>Descartado: no cuenta como "
                    "viaje, pero los datos se conservan.</div>"
                    "<a href='" PORTAL_HEAVY_BASE "/data/viaje.tar?v=%s'>Descargar</a></li>",
                    nombre_esc, nombre_esc);
            } else if (e == V_DESCONOCIDO) {
                /* No se pudo mirar: se dice, en vez de afirmar que sigue abierto.
                 * Recargar esta pagina lo resuelve. */
                snprintf(linea, sizeof(linea),
                    "<li class=cur><b>%s</b><div class=e>No he podido mirarlo ahora "
                    "mismo (la tarjeta estaba ocupada). Recarga la pagina.</div></li>",
                    nombre_esc);
            } else {
                snprintf(linea, sizeof(linea),
                    "<li class=cur><b>%s</b><div class=e>En curso: finalizalo en la "
                    "pantalla de la cabina antes de bajarlo</div></li>", nombre_esc);
            }
            httpd_resp_sendstr_chunk(req, linea);
        }
        /* El closedir SI tiene que ocurrir (fuga de DIR si no), asi que se
         * espera al cerrojo en vez de saltarselo -- pero acotado: un bus SD
         * atascado de verdad no puede colgar esto para siempre. */
        bool got_lock = camera_sd_bus_lock_wait(5000);
        if (!got_lock) ESP_LOGW(TAG, "closedir sin cerrojo SD tras 5s de espera");
        closedir(d);
        if (got_lock) camera_sd_bus_unlock();
    }
    if (!alguno) httpd_resp_sendstr_chunk(req, "<li>Todavia no hay ningun viaje.</li>");
    httpd_resp_sendstr_chunk(req,
        "</ul><p class=e>El paquete de siempre (bateria, solar y frigo de TODOS "
        "los dias) esta en <a href='" PORTAL_HEAVY_BASE "/data/historico.tar'>historico.tar</a>.</p>"
        "<p><a href='/data'>Volver</a></p>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}
