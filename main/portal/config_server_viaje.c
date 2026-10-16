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
#include "esp_vfs_fat.h"   /* esp_vfs_fat_info: el hueco libre de la tarjeta */

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

/* Las dos viven mas abajo (la primera, con las utilidades de nombres; la
 * segunda, con el cierre del viaje) pero se usan ya en la reparacion de
 * arranque y en las comprobaciones de dueno. */
static void destino_seguro(const char *in, char *out, size_t n);
static void renombrado_pendiente_aplicar(void);
static bool es_digito_hex(char c);
static bool dev_es_mac(const char *dev);

/* Atado a CUERPO_MAX de ~/joint/35cabina/main/net/viaje_cola.c (896): el * satelite no puede mandar mas de eso, con esto sobra margen. SI ALGUN DIA SE
 * SUBE ALLI, HAY QUE SUBIRLO AQUI TAMBIEN -- el job de CI "viaje_body_sync"
 * (.github/workflows/build.yml) falla si el satelite llega a igualar o
 * superar esto, para que no dependa de acordarse solo. */
#define VIAJE_BODY_MAX  1024

#define VIAJES_DIR      "/sdcard/viajes"
/* VEHICULO_DIR (el historial del vehiculo) vive en config_server_internal.h:
 * lo comparte data_export_tar.c, que ahora deja bajarlo. */
#define NVS_NS          "viaje_p4"
#define NVS_CARPETA     "carpeta"
/* Destino nuevo pedido a mitad de viaje, SIN aplicar todavia (ver op_destino).
 * Vacio o ausente = no se ha cambiado. */
#define NVS_DESTINO     "destino_nuevo"     /* ruta del viaje abierto, ausente si no hay */
/* Que pantalla empezo el viaje abierto: su MAC en hex. Vacio = no se sabe (una
 * cabina con firmware viejo, o un viaje empezado antes de este cambio). */
#define NVS_DEV         "v_dev"
/* Id del INICIO del viaje abierto (16-oct-2026). Sirve para reconocer un evento
 * que se quedo rezagado del viaje ANTERIOR: ver id_rezagado(). Ausente = no se
 * sabe (viaje abierto por un firmware que no lo apuntaba, o por la web). */
#define NVS_VIAJE_INI   "v_ini"
/* Renombrado del cierre que se quedo a medias (ver op_fin): el nombre VIEJO y
 * el NUEVO de la carpeta, en base (sin VIAJES_DIR). Presentes solo si hay un
 * renombrado por terminar. */
#define NVS_REN_DE      "v_ren_de"
#define NVS_REN_A       "v_ren_a"
/* Id con el que la WEB cierra un viaje (op_fin). Fijo y con el byte alto 0xFF:
 * los ids de las cabinas llevan su MAC ahi, asi que el peor caso es una pantalla
 * con mac[5]==0xFF y contador exactamente 16776961 (1 entre 16 millones). No
 * pasa por el anillo de dedupe: ver el comentario de handle_data_viajes. */
#define ID_CIERRE_WEB   0xFFFFFF01u
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
/* Cuentakilometros del vehiculo que el usuario teclea al EMPEZAR el viaje (16-
 * oct-2026). 0 = no lo apunto (o el viaje se empezo desde una pantalla que no
 * lo pide). Es un dato del viaje, asi que se borra con los totales. */
#define NVS_T_KM_CUADRO "t_kmcuadro"
/* Y el que teclea al CERRARLO (misma fecha, 16-oct-2026): con los dos se sabe lo
 * que ha hecho el viaje DE VERDAD (cuadro) y se puede comparar con lo que midio
 * el GPS. 0 = no apuntado. */
#define NVS_T_KM_LLEGADA "t_kmllegada"

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

/* ── FALLOS DE ESCRITURA DEL VIAJE (auditoria del 13-oct-2026, hallazgo 14) ──
 *
 * Hasta hoy, si la tarjeta fallaba con un viaje abierto, el unico rastro era una
 * linea en el log: el icono de la SD de la barra solo mira los fallos del
 * DATALOGGER, asi que podia estar verde mientras el viaje no se escribia. Con
 * esto, cualquier fallo de escritura del viaje (apunte, cierre, telemetria,
 * contadores, ruta) enciende el mismo aviso, que es lo que mira el usuario. */
static _Atomic uint32_t s_fallos_escritura;

static void anota_fallo_escritura(void) { s_fallos_escritura++; }
/* Y al REVES: una escritura buena borra el aviso. Antes el contador solo subia,
 * asi que un fallo puntual (un 507 que el reintento resolvio) dejaba el icono de
 * la SD en rojo hasta el siguiente reinicio (revision del 13-oct-2026). */
static void anota_escritura_ok(void) { s_fallos_escritura = 0; }

uint32_t viaje_fallos_escritura(void) { return s_fallos_escritura; }

/* Y LOS FALLOS DE NVS, APARTE (correccion del 14-oct-2026): meterlos en el
 * contador de la tarjeta hacia que el icono de la SD mintiera en las dos
 * direcciones -- rojo con la tarjeta bien (NVS llena) y verde con la NVS
 * fallando (el siguiente apunte escrito en la tarjeta ponia el contador a cero).
 * La NVS se mira donde se mira su ocupacion: Ajustes -> About. */
static uint32_t s_fallos_nvs = 0;
static void anota_fallo_nvs(void) { s_fallos_nvs++; }
uint32_t viaje_fallos_nvs(void) { return s_fallos_nvs; }

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
/* Deja el id en el anillo de dedupe SIN TOCAR de quien es el viaje abierto.
 *
 * Existe porque estado_set_en(h, NULL, id) BORRA el viaje abierto (escribe
 * NVS_CARPETA=""), y eso solo vale cuando de verdad no hay ninguno: un apunte
 * del historial del vehiculo (o de otra pantalla) pasaba por ahi y se llevaba
 * por delante el viaje que la otra cabina tenia abierto -- sus apuntes
 * siguientes al historial, su fin contestado "no habia viaje abierto" y su
 * carpeta sin resumen para siempre (auditoria del 14-oct-2026). */
static void id_anota_en(nvs_handle_t h, uint32_t id)
{
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
     * se llevaria un 412 en vez del 409 de siempre. Y se borra el id del inicio
     * por el mismo motivo: sin viaje abierto no hay "viaje anterior" contra el
     * que comparar (ver id_rezagado()). */
    if (!carpeta) {
        nvs_erase_key(h, NVS_DEV);
        nvs_erase_key(h, NVS_VIAJE_INI);
    }
    estado_set_en(h, carpeta, id);
    esp_err_t e = nvs_commit(h);
    nvs_close(h);
    return e;
}

/* ── DE QUE PANTALLA ES EL VIAJE ABIERTO (auditoria del 14-oct-2026) ─────────
 *
 * Hasta hoy 'dev' solo se miraba al ABRIR el viaje (op_inicio, que contesta 412
 * si lo empezo otra pantalla). op_fin, op_descartar y op_registro no lo miraban,
 * asi que una segunda cabina podia CERRAR o APARTAR el viaje de la primera
 * --que seguia grabando-- y a partir de ahi todos sus apuntes se iban al
 * historial del vehiculo con un 200, sin resumen y sin que nadie se enterase.
 * Es el unico fallo de la lista que destruye datos de la OTRA pantalla, y en el
 * montaje que se quiere montar (la 5" sustituyendo a la 3,5", con las dos
 * conviviendo una temporada) se juega a diario. */

/* El dueno del viaje abierto, tal y como lo apunto op_inicio. Cadena vacia si
 * no hay viaje abierto o si lo empezo un firmware que no mandaba 'dev'. */
static void dev_abierto(char *out, size_t n)
{
    out[0] = 0;
    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) return;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t sz = n;
    if (nvs_get_str(h, NVS_DEV, out, &sz) != ESP_OK) out[0] = 0;
    nvs_close(h);
    /* Un dueno que no es una MAC (aqui se guardo "desconocido" en versiones
     * anteriores a la auditoria del 14-oct-2026) vale lo mismo que no tener
     * dueno: si no, esa pantalla se llevaria un 409 al cerrar SU PROPIO viaje
     * -- porque "desconocido" nunca va a coincidir con su MAC. */
    if (!dev_es_mac(out)) out[0] = 0;
}

static bool es_digito_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

/* 12 digitos hex: lo que manda la cabina en 'dev' (su MAC). */
static bool dev_es_mac(const char *dev)
{
    if (!dev || strlen(dev) != 12) return false;
    for (int i = 0; i < 12; i++) if (!es_digito_hex(dev[i])) return false;
    return true;
}

/* El byte alto del id es la MAC de la pantalla que lo genero (ver
 * next_trip_seq() en config_storage.c de las cabinas). Devuelve -1 si no se
 * puede saber: 0 no es un id valido, asi que un byte alto 0 es "no se sabe". */
static int byte_de_id(uint32_t id)
{
    int b = (int)((id >> 24) & 0xFFu);
    return b ? b : -1;
}

/* El ultimo byte de la MAC del 'dev' ("a1b2c3d4e5f6"). -1 si no es una MAC. */
static int byte_de_dev(const char *dev)
{
    if (!dev) return -1;
    size_t l = strlen(dev);
    if (l < 2) return -1;
    if (!es_digito_hex(dev[l - 2]) || !es_digito_hex(dev[l - 1])) return -1;
    char par[3] = {dev[l - 2], dev[l - 1], 0};
    long v = strtol(par, NULL, 16);
    return (v >= 0 && v <= 255) ? (int)v : -1;
}

/* ── UN EVENTO REZAGADO DEL VIAJE ANTERIOR (auditoria del 16-oct-2026) ───────
 *
 * EL FALLO QUE ESTO CIERRA, tal cual: la cabina manda el "fin" de un viaje, la
 * P4 lo aplica y la respuesta se pierde (Wi-Fi, un 503 del cerrojo de la SD...).
 * La cabina, que reintenta la cabeza de su cola, lo vuelve a mandar. Si entre
 * medias el usuario empieza OTRO viaje -- y en el montaje previsto eso es normal:
 * el mismo aparato, el mismo dia -- ese "fin" reintentado llegaba con el viaje
 * nuevo ABIERTO, con el anillo de dedupe recien VACIADO (ids_ring_limpiar, que se
 * vacia al abrir justo para que los ids del viaje nuevo no choquen con los del
 * viejo) y sin dueño distinto al que lo manda: 200, resumen y carpeta del viaje
 * NUEVO cerrados, con los datos y el cuentakilometros del VIEJO. Ni un aviso.
 *
 * COMO SE RECONOCE, sin cambiar el protocolo: el byte alto del id es la MAC de la
 * pantalla que lo genero y el resto es su contador (next_trip_seq(), que solo
 * sube y vive en la NVS de la cabina). Asi que "mismo byte de pantalla y numero
 * ANTERIOR al inicio del viaje abierto" es, sin duda, un evento del viaje
 * anterior. Si no se sabe (sin viaje abierto, sin id de inicio apuntado, id sin
 * byte de pantalla -- la web, o un firmware viejo), esto dice que NO y todo sigue
 * como siempre. */
static uint32_t viaje_ini_id(void)
{
    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) return 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return 0;
    uint32_t ini = 0;
    nvs_get_u32(h, NVS_VIAJE_INI, &ini);
    nvs_close(h);
    return ini;
}

static bool id_rezagado(uint32_t id)
{
    uint32_t ini = viaje_ini_id();
    if (!ini || !id) return false;
    int pant = byte_de_id(ini);
    if (pant < 0 || byte_de_id(id) != pant) return false;
    /* El contador de 24 bits no da la vuelta en la vida de una pantalla (16
     * millones de eventos), asi que comparar el resto basta. */
    return (id & 0x00FFFFFFu) < (ini & 0x00FFFFFFu);
}

/* SI EL QUE PIDE ES OTRA PANTALLA.
 *
 * Ante la duda se contesta que NO es ajena (se trata como siempre): si no hay
 * dueno apuntado -- firmware viejo, o viaje empezado antes de este cambio -- o
 * el que pide no se identifica de ninguna forma, no se puede afirmar nada.
 *
 * Cuando el body trae 'dev' MANDA EL BODY (es lo que la cabina dice de si
 * misma). Los apuntes no lo llevan, asi que ahi se usa el byte alto del id, que
 * es la MAC de la pantalla que lo genero: por eso esto tambien protege contra
 * una cabina con firmware viejo. */
static bool pide_otra_pantalla(const cJSON *j, uint32_t id, bool admite_forzar)
{
    /* 'forzar' es la llave de la salida de emergencia: la pantalla NUEVA solo
     * lo manda cuando el usuario ha confirmado en un dialogo que ese viaje lo
     * empezo otra pantalla (puede estar en marcha). Una cabina con firmware
     * viejo no lo manda, asi que se queda con el 409 y no puede destruir el
     * viaje de la otra por accidente -- que es el fallo que esto viene a
     * cerrar. Sin la llave, un viaje cuyo dueño ya no existe no se podria
     * cerrar nunca (la P4 vive detras y no hay boton en ella). */
    if (admite_forzar) {
        /* 'true' (lo que manda la cabina) o un numero distinto de cero: un
         * cJSON_IsTrue() a secas rechazaria un "forzar":1, y la cabina nueva se
         * quedaria sin la salida de emergencia por un detalle de JSON. */
        const cJSON *jf = cJSON_GetObjectItemCaseSensitive(j, "forzar");
        if (cJSON_IsTrue(jf) || (cJSON_IsNumber(jf) && jf->valuedouble != 0))
            return false;
    }

    char dueno[24];
    dev_abierto(dueno, sizeof(dueno));
    if (!dueno[0]) return false;

    char pide[24] = {0};
    const cJSON *jdev = cJSON_GetObjectItemCaseSensitive(j, "dev");
    if (cJSON_IsString(jdev)) destino_seguro(jdev->valuestring, pide, sizeof(pide));
    if (pide[0]) return viaje_dev_ajeno(pide, dueno) != 0;

    int b = byte_de_id(id);
    int d = byte_de_dev(dueno);
    return (b > 0 && d >= 0 && b != d);
}

/* ¿La carpeta NO esta en la tarjeta? Solo se dice que NO cuando la FAT
 * responde y dice que no (ENOENT) Y el directorio de viajes se puede abrir.
 *
 * Cualquier otro fallo de stat (EIO, EBUSY: tarjeta sacada sin desmontar, bus
 * ocupado) NO es prueba de nada: datalogger_sd_montada() es un flag cacheado
 * (s_card != NULL), no una prueba de que la FAT sea legible. Sin esto, un fallo
 * transitorio cerraba el viaje vivo "sin resumen" (op_fin) o abria un viaje
 * nuevo encima partiendo el que estaba en marcha (op_inicio) -- auditoria del
 * 14-oct-2026. Ante la duda se dice que SI esta: el que escribe despues dara su
 * 507, que es reintentable y no pierde nada. */
static bool carpeta_fuera_de_la_tarjeta(const char *carpeta)
{
    struct stat st;
    if (stat(carpeta, &st) == 0) return !S_ISDIR(st.st_mode);
    if (errno != ENOENT) {
        ESP_LOGW(TAG, "no puedo mirar si %s esta en la tarjeta (%s): no saco "
                      "conclusiones", carpeta, strerror(errno));
        return false;
    }
    DIR *d = opendir(VIAJES_DIR);
    if (!d) {
        ESP_LOGW(TAG, "el directorio de viajes no se puede abrir (%s): no saco "
                      "conclusiones", strerror(errno));
        return false;
    }
    closedir(d);
    return true;
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

/* Deja en el log cuanto ocupa la NVS. La de esta P4 son 16 KB (hallazgo M8 de
 * la auditoria del 13-oct-2026) y, si se llena, el viaje deja de poder abrirse:
 * mas vale enterarse por un numero que por un 507 en la carretera. */
static void log_estado_nvs(void)
{
    nvs_stats_t ns = {0};
    if (nvs_get_stats(NULL, &ns) != ESP_OK) return;
    ESP_LOGI(TAG, "NVS: %u/%u entradas usadas (%u libres), %u namespace(s)",
             (unsigned)ns.used_entries, (unsigned)ns.total_entries,
             (unsigned)ns.free_entries, (unsigned)ns.namespace_count);
    if (ns.free_entries < 40)
        ESP_LOGW(TAG, "NVS casi llena: %u entradas libres. Si se llena, no se "
                      "podra abrir viaje (507) -- hay que ampliar la particion",
                 (unsigned)ns.free_entries);
}

/* Definida mas abajo, con el resto del modulo del viaje. */
bool viaje_sd_hueco(uint64_t *libre_out, uint64_t *total_out);
static void sd_hueco_medir(void);

/* Como responde_con_hueco, pero para los 507 QUE NO SON DE LA TARJETA (la NVS
 * del viaje: no poder guardar el inicio, el cierre, el apunte...). Decir ahi
 * "le quedan 29000 MB" seria desinformar: el que mira tiene que saber que lo que
 * se ha llenado es la memoria interna, no la SD. */
static void responde_sin_sitio_nvs(httpd_req_t *req, const char *texto)
{
    char cuerpo[192];
    snprintf(cuerpo, sizeof(cuerpo), "%s sin_sitio=nvs", texto);
    httpd_resp_sendstr(req, cuerpo);
}

/* CONTESTA Y DICE CUANTO QUEDA EN LA TARJETA (14-oct-2026).
 *
 * El satelite ya recibe este cuerpo y lo tira (`p4_api.c`, drena la respuesta
 * para cerrar limpio). Añadirle un sufijo es un canal que NO toca el protocolo
 * de 40 bytes de la telemetria -- que lo comparten las tres placas y no se puede
 * cambiar sin grabarlas a la vez -- y sirve justo cuando importa: el aviso de
 * "la P4 no puede escribir" (507) puede decir CUANTO QUEDA, y la pantalla puede
 * avisar antes de que la tarjeta se llene del todo.
 *
 * El sufijo es texto sin autenticar (como el 507): la pantalla lo usa para
 * INFORMAR, nunca para decidir si un apunte se guardo. Si no viene (P4 vieja) o
 * viene raro, el que lo lee se queda como estaba. */
static void responde_con_hueco(httpd_req_t *req, const char *texto)
{
    char cuerpo[192];
    uint64_t libre = 0, total = 0;
    if (viaje_sd_hueco(&libre, &total))
        snprintf(cuerpo, sizeof(cuerpo), "%s libre_mb=%llu", texto,
                 (unsigned long long)(libre / (1024ULL * 1024ULL)));
    else
        snprintf(cuerpo, sizeof(cuerpo), "%s", texto);
    httpd_resp_sendstr(req, cuerpo);
}

/* ── HUECO LIBRE EN LA TARJETA (auditoria del 14-oct-2026) ──────────────────
 *
 * Hasta ahora NO se medía en ningún sitio: se escribía y se fallaba con un 507,
 * así que el usuario se enteraba de que la tarjeta estaba llena cuando ya no se
 * podía anotar (y la pantalla de la cabina sólo dice "la P4 no puede escribir",
 * que es un síntoma, no un diagnóstico). Se mide y se dice.
 *
 * Se refresca en el tick de telemetría (cada 5 min) y tras cada 507, que son los
 * dos momentos en que el dato importa. `esp_vfs_fat_info` lee la FAT: va con el
 * cerrojo de la SD, como todo lo que la toca. */
static uint64_t s_sd_libre   = UINT64_MAX;   /* UINT64_MAX = no se ha podido medir */
static uint64_t s_sd_total   = 0;
static int64_t  s_sd_medido_us = 0;

#define SD_LIBRE_AVISO_BYTES  (20ULL * 1024ULL * 1024ULL)   /* 20 MB */

static void sd_hueco_medir(void)
{
    if (!datalogger_sd_montada()) { s_sd_libre = UINT64_MAX; s_sd_total = 0; return; }
    if (!camera_sd_bus_lock(1500)) return;      /* ocupada: se deja el valor anterior */
    uint64_t total = 0, libre = 0;
    esp_err_t e = esp_vfs_fat_info("/sdcard", &total, &libre);
    camera_sd_bus_unlock();
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "no he podido medir el hueco de la tarjeta (%s)", esp_err_to_name(e));
        return;
    }
    s_sd_total = total;
    s_sd_libre = libre;
    s_sd_medido_us = esp_timer_get_time();
    if (libre < SD_LIBRE_AVISO_BYTES)
        ESP_LOGW(TAG, "QUEDA POCO SITIO en la tarjeta: %llu MB libres de %llu MB. "
                      "Si se llena, los apuntes dan 507 y el viaje no se puede cerrar "
                      "hasta hacer sitio",
                 (unsigned long long)(libre / (1024ULL * 1024ULL)),
                 (unsigned long long)(total / (1024ULL * 1024ULL)));
}

/* Para la pantalla de Ajustes -> About. Devuelve false si no se ha podido medir
 * (sin tarjeta, o el montaje falló). */
bool viaje_sd_hueco(uint64_t *libre_out, uint64_t *total_out)
{
    if (s_sd_libre == UINT64_MAX) return false;
    if (libre_out) *libre_out = s_sd_libre;
    if (total_out) *total_out = s_sd_total;
    return true;
}

void viaje_reparar_al_arrancar(void)
{
    log_estado_nvs();
    sd_hueco_medir();
    /* Y el hueco de la tarjeta, en el log de arranque: es el otro sitio que se
     * llena y hasta ahora solo se veia entrando a Ajustes. Una linea por
     * arranque, y con el dato que luego usa el aviso de "queda poco sitio". */
    if (s_sd_libre != UINT64_MAX)
        ESP_LOGI(TAG, "SD: %llu MB libres de %llu",
                 (unsigned long long)(s_sd_libre / (1024ULL * 1024ULL)),
                 (unsigned long long)(s_sd_total / (1024ULL * 1024ULL)));
    if (!datalogger_sd_montada()) {
        ESP_LOGW(TAG, "SD sin montar: no compruebo el viaje abierto");
        return;
    }
    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) {
        /* Un renombrado del destino que se quedo a medias se termina ahora, que
         * es justo el caso del corte de corriente entre el commit del cierre y
         * el rename (ver renombrado_pendiente_marcar). */
        if (camera_sd_bus_lock(3000)) {
            renombrado_pendiente_aplicar();
            camera_sd_bus_unlock();
        }
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
    /* El retorno SI importa (auditoria del 14-oct-2026): si la NVS no acepta el
     * borrado, el viaje sigue abierto apuntando a una carpeta que no existe y
     * la P4 se queda igual de atascada. Antes se tiraba sin decir nada. */
    if (estado_set(NULL, 0) != ESP_OK)
        ESP_LOGE(TAG, "NO he podido limpiar el viaje abierto de la NVS: la P4 "
                      "sigue con %s abierto y sin carpeta. Habra que "
                      "reintentarlo en el proximo arranque", carpeta);
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
        /* Sin eventos.csv la carpeta se quedo a medias ANTES del diario (corte
         * entre el mkdir y el apunte del inicio). Antes se saltaba ("ni
         * empezo") y esa carpeta se quedaba "En curso" para siempre, sin poder
         * cerrarla ni bajarla (auditoria del 14-oct-2026). Se marca igual: es
         * lo que la hace descargable y con el nombre diciendo lo que es. */
        bool con_diario = (stat(ruta, &st) == 0);
        snprintf(ruta, sizeof(ruta), VIAJES_DIR "/%s/" MARCA_INCOMPLETO, ent->d_name);
        FILE *f = fopen(ruta, "w");
        if (!f) continue;
        if (con_diario)
            fprintf(f, "Este viaje NO SE CERRO y ya no se puede cerrar.\n\n"
                       "Se quedo a medias al EMPEZAR: la carpeta llego a crearse pero la\n"
                       "P4 no llego a apuntar el viaje (un corte de corriente en ese\n"
                       "momento). No es el viaje abierto, asi que no hay forma de\n"
                       "finalizarlo desde la cabina.\n\n"
                       "Los datos que haya dentro son los que se llegaron a escribir.\n");
        else
            fprintf(f, "Esta carpeta de viaje se quedo VACIA.\n\n"
                       "La P4 llego a crearla y no llego a escribir ni el apunte del\n"
                       "inicio: un corte de corriente justo en ese momento. Aqui no hay\n"
                       "ningun dato -- lo que se hubiera anotado despues no existe.\n");
        fflush(f);
        fsync(fileno(f));   /* es una marca: no puede quedarse en la cache */
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
    if (stat(ruta, &st) == 0) return st.st_size;
    /* Si el fichero NO existe, la marca es 0 (correcto: se acaba de crear). Si
     * el stat falla por otra cosa, es -1 y NO se deshace nada: devolver 0 en ese
     * caso podia VACIAR el fichero entero al "deshacer" (revision del
     * 13-oct-2026). */
    return (errno == ENOENT) ? 0 : -1;
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
    if (marca < 0) return;                   /* no se pudo medir: no se toca nada */
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
    /* fflush ANTES del fsync (auditoria del 14-oct-2026): fsync va sobre el
     * descriptor, no sobre el buffer de stdio del FILE*, asi que sin esto el
     * dato seguia en RAM y el fsync no aseguraba nada -- el volcado real
     * ocurria en el fclose, ya sin fsync detras. */
    fflush(f);
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

/* Igual, pero DICE si la lectura fallo de verdad. Que una clave no este es
 * normal (un viaje sin peajes no tiene NVS_T_PEAJE): eso es 0. Un error de NVS
 * -- particion ilegible, entrada corrupta -- no lo es, y con nvs_get_d solo se
 * convertia en un 0 indistinguible de "no hubo gasto ninguno" (auditoria del
 * 14-oct-2026). */
static bool nvs_get_d_verificado(nvs_handle_t h, const char *k, double *out)
{
    uint64_t raw = 0;
    esp_err_t e = nvs_get_u64(h, k, &raw);
    if (e == ESP_ERR_NVS_NOT_FOUND) { *out = 0.0; return true; }
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "no puedo leer %s de la NVS (%s)", k, esp_err_to_name(e));
        return false;
    }
    memcpy(out, &raw, sizeof(*out));
    return true;
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

        /* COBRO POR 24 H (red team del 14-oct-2026). La cabina manda
         * "cobro":"24h" o "noche" desde el formulario de la pernocta, y aqui se
         * IGNORABA: se cobraba siempre por noches de calendario. Un area que
         * cobra 20 EUR/24 h y una estancia de 25 h salian a 20 EUR en vez de 40.
         * Si el cobro es por 24 h se cuentan las fracciones de 24 h (hacia
         * arriba) de los 'minutos' que manda la cabina; si no vinieran, se cae a
         * las noches de siempre. */
        double unidades = noches;
        const cJSON *jcob = cJSON_GetObjectItemCaseSensitive(datos, "cobro");
        if (cJSON_IsString(jcob) && strstr(jcob->valuestring, "24h")) {
            const cJSON *jm = cJSON_GetObjectItem(datos, "minutos");
            double min = cJSON_IsNumber(jm) ? jm->valuedouble : 0;
            if (min > 0) {
                long u = (long)((min + 1439.0) / 1440.0);   /* hacia arriba */
                if (u < 1) u = 1;
                if (u > 60) u = 60;
                unidades = (double)u;
                ESP_LOGI(TAG, "pernocta con cobro por 24 h: %.0f min -> %ld dia(s) "
                              "de 24 h a %.2f", min, u, precio);
            }
        }
        nvs_set_d(h, NVS_T_ALOJA,
                  nvs_get_d(h, NVS_T_ALOJA) + precio * unidades + extras);
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
    /* El cuentakilometros del viaje ANTERIOR, fuera (los dos: el de salida y el
     * de llegada): si este viaje no trae lectura, el resumen tiene que decir que
     * no la hay, no repetir la del viaje de la semana pasada. */
    nvs_erase_key(h, NVS_T_KM_CUADRO);
    nvs_erase_key(h, NVS_T_KM_LLEGADA);
    nvs_commit(h);
    nvs_close(h);
}

/* El cuentakilometros que teclea el usuario en el campo 'km_cuadro': en el
 * INICIO es lo que marcaba el vehiculo al salir, y en el FIN lo que marca al
 * llegar. 0 = no apuntado (campo ausente, o una pantalla que no lo pide), que es
 * un valor legitimo: no se obliga a teclearlo.
 *
 * Se acota a 6 cifras, que es lo que admite el teclado de la cabina y lo que
 * cabe en cualquier cuadro real; un numero mayor es un dedazo y se topa. */
static uint32_t km_cuadro_de(const cJSON *j)
{
    const cJSON *jk = cJSON_GetObjectItem(j, "km_cuadro");
    if (!cJSON_IsNumber(jk) || jk->valuedouble <= 0) return 0;
    double d = jk->valuedouble;
    if (d > 999999.0) d = 999999.0;
    return (uint32_t)(d + 0.5);
}

/* ── Las operaciones ──────────────────────────────────────────────────────── */

/* Dias desde 1970 de una fecha LOCAL, sin tm_gmtoff (la newlib de ESP-IDF no
 * lo trae). Es la misma cuenta que hace udp_tx.c para armar epoch_local, que es
 * de donde sale el 'fecha_dias' que manda la cabina: asi los dos numeros son
 * comparables. Vale de sobra para 1970-2100. */
static long dias_locales_de(const struct tm *t)
{
    static const int fin_mes[12] = {0,31,59,90,120,151,181,212,243,273,304,334};
    long y = t->tm_year + 1900;
    long d = (y - 1970) * 365 + (y - 1969) / 4 - (y - 1901) / 100 + (y - 1601) / 400;
    d += fin_mes[t->tm_mon] + t->tm_mday - 1;
    if (t->tm_mon >= 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) d++;
    return d;
}

static esp_err_t op_inicio(httpd_req_t *req, const cJSON *j, uint32_t id)
{
    const cJSON *jd = cJSON_GetObjectItem(j, "destino");
    const cJSON *jf = cJSON_GetObjectItem(j, "fecha_dias");
    if (!cJSON_IsString(jd) || !cJSON_IsNumber(jf)) {
        httpd_resp_set_status(req, "400 Bad Request");
        responde_con_hueco(req, "faltan destino o fecha_dias");
        return ESP_OK;
    }

    char abierto[CARPETA_MAX];
    bool hay_abierto = viaje_abierto(abierto, sizeof(abierto));
    /* Si el viaje abierto apunta a una carpeta que YA NO ESTA (sacaron la
     * tarjeta, la formatearon o se borro desde el PC) esto era un 409 para
     * siempre: ni se podia empezar un viaje nuevo, ni cerrar el viejo, ni
     * apartarlo, y la unica salida era reiniciar la P4 -- que vive en la parte
     * de atras (auditoria del 14-oct-2026). Con la SD MONTADA, que la carpeta no
     * exista es un hecho: los datos de ese viaje no estan en esta tarjeta, asi
     * que se limpia el estado y se sigue. Si la SD no esta montada, la ausencia
     * no significa nada y se mantiene el 409 de siempre. */
    if (hay_abierto && datalogger_sd_montada() && camera_sd_bus_lock(3000)) {
        /* CON EL CERROJO: esto es I/O de tarjeta, igual que el resto. */
        bool fuera = carpeta_fuera_de_la_tarjeta(abierto);
        camera_sd_bus_unlock();
        if (fuera) {
            ESP_LOGE(TAG, "el viaje abierto %s no esta en la tarjeta montada: "
                          "limpio el estado y sigo con el viaje nuevo", abierto);
            estado_set(NULL, 0);
            hay_abierto = false;
        }
    }
    if (hay_abierto) {
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
        responde_con_hueco(req, resp);
        return ESP_OK;
    }

    char destino[CARPETA_MAX], fecha[12];
    destino_seguro(jd->valuestring, destino, DESTINO_MAX);
    fecha_de_dias((uint32_t)jf->valuedouble, fecha, sizeof(fecha));

    /* LA FECHA DE LA CARPETA LA MANDA LA CABINA, PERO SI NO CUADRA CON LA DE LA
     * P4 SE USA LA DE LA P4 (auditoria del 13-oct-2026, hallazgo M4).
     *
     * 'fecha_dias' sale del reloj de la cabina, que a su vez viene de la
     * telemetria de esta P4... pero puede ser VIEJO: si la cabina arranca
     * cuando la P4 esta apagada, o el reloj de la P4 viene de un respaldo en
     * NVS antes de que el GPS lo ponga en hora, el nombre de la carpeta sale con
     * un dia que no es. Y la fecha del nombre es el IDENTIFICADOR del viaje: si
     * miente, ordenar y buscar viajes en el PC miente. La P4 es la que tiene el
     * GPS y el RTC, asi que ante la duda manda la suya. Mas de un dia de
     * diferencia no es un viaje a caballo de medianoche: es un reloj mal. */
    {
        time_t ahora_p4 = time(NULL);
        if (ahora_p4 > 1000000000L) {
            struct tm tm_p4;
            localtime_r(&ahora_p4, &tm_p4);
            char fecha_p4[12];
            strftime(fecha_p4, sizeof(fecha_p4), "%Y-%m-%d", &tm_p4);
            /* Solo si la diferencia es de MAS DE UN DIA (como dice el
             * comentario): un viaje que empieza a las 23:59 y la P4 que ya esta
             * en el dia siguiente es normal y se respeta la fecha que pide la
             * cabina (revision del 13-oct-2026). */
            /* Dias LOCALES (ver dias_locales_de): antes esto partia la
             * medianoche local por 86400, que da el dia UTC de esa medianoche --
             * en Madrid salia SIEMPRE uno menos que el de la cabina, asi que el
             * umbral real era de 3 dias atras y 1 adelante en vez de +/-2
             * (revision del 13-oct-2026). */
            long dias_p4  = dias_locales_de(&tm_p4);
            long dias_cab = (long)jf->valuedouble;
            long dif = dias_p4 - dias_cab; if (dif < 0) dif = -dif;
            if (dif >= 2) {
                ESP_LOGW(TAG, "la cabina pide el viaje con fecha %s y aqui es %s: "
                              "uso la de la P4 (su reloj es el que tiene GPS/RTC)",
                         fecha, fecha_p4);
                snprintf(fecha, sizeof(fecha), "%s", fecha_p4);
            }
        }
    }

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
        responde_con_hueco(req, "tarjeta ocupada");
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
        responde_con_hueco(req, "no puedo crear la carpeta");
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
        anota_fallo_nvs();
        httpd_resp_set_status(req, "507 Insufficient Storage");
        responde_sin_sitio_nvs(req, "no puedo escribir el inicio del viaje en la NVS");
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
        /* Cuentakilometros que marca la autocaravana al salir (16-oct-2026): lo
         * teclea el usuario en la pantalla al empezar el viaje y es OPCIONAL --
         * una cabina que no lo pida (o un viaje empezado antes de esto) manda 0,
         * que es "no apuntado". Se acota a 6 cifras, que es lo que admite el
         * teclado de la cabina y lo que cabe en cualquier cuadro real. */
        {
            uint32_t kmc = km_cuadro_de(j);
            nvs_set_u32(h, NVS_T_KM_CUADRO, kmc);
            if (kmc) ESP_LOGI(TAG, "cuadro al salir: %lu km", (unsigned long)kmc);
        }
        /* Que pantalla empieza el viaje, en el MISMO commit que el resto del
         * estado: si no, un corte entre los dos deja un viaje sin dueno. */
        {
            const cJSON *jd2 = cJSON_GetObjectItemCaseSensitive(j, "dev");
            char dev[24] = {0};
            if (cJSON_IsString(jd2)) destino_seguro(jd2->valuestring, dev, sizeof(dev));
            /* SOLO SI ES UNA MAC (12 hex). La cabina manda "desconocido" cuando
             * esp_wifi_get_mac() falla; guardado tal cual, ese viaje quedaba con
             * un dueño que no volveria a coincidir nunca y la propia pantalla se
             * llevaria un 409 al intentar cerrarlo (auditoria del 14-oct-2026).
             * Sin dueño apuntado se trata como siempre. */
            if (!dev_es_mac(dev)) {
                if (dev[0]) ESP_LOGW(TAG, "dev '%s' no es una MAC: el viaje se "
                                          "queda SIN dueño apuntado", dev);
                dev[0] = 0;
            }
            nvs_set_str(h, NVS_DEV, dev);
        }
        /* EL ID DEL INICIO, en el mismo commit (16-oct-2026): con esto se
         * distingue un evento de ESTE viaje de uno que se quedo dando vueltas del
         * anterior (ver id_rezagado()). */
        nvs_set_u32(h, NVS_VIAJE_INI, id);
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
        anota_fallo_nvs();
        httpd_resp_set_status(req, "507 Insufficient Storage");
        responde_sin_sitio_nvs(req, "no puedo guardar el viaje en la NVS");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "VIAJE ABIERTO: %s", carpeta);
    responde_con_hueco(req, carpeta);
    return ESP_OK;
}

/* resumen.txt: lo que uno quiere saber al volver, en castellano llano y no en
 * columnas. Los CSV ya estan ahi para las cuentas finas. */
/* Devuelve si el resumen ha llegado a la tarjeta. Cerrar un viaje SIN resumen y
 * contestar que si es dejar al usuario con un viaje que cree cerrado y sin las
 * cuentas -- y sin enterarse hasta que lo descargue meses despues. */
static bool escribir_resumen(const char *carpeta)
{
    double litros = 0, combus = 0, peaje = 0, bombona = 0, manten = 0, agua = 0;
    double aloja = 0;
    uint32_t paradas = 0;
    uint64_t inicio = 0;
    uint8_t varias = 0;
    uint32_t km_cuadro = 0;   /* cuentakilometros que tecleo el usuario al salir */
    uint32_t km_llegada = 0;  /* y el que tecleo al llegar */
    char moneda[8] = "EUR";
    /* SI LA NVS NO SE PUEDE LEER, NO SE ESCRIBE EL RESUMEN (auditoria del
     * 14-oct-2026): antes cualquier fallo de lectura se convertia en un 0 y el
     * resumen salia con TODOS los totales a cero, el viaje se cerraba con 200 y
     * quedaba "Listo" con unas cuentas que mienten. Devolviendo false el viaje
     * sigue abierto y el fin se reintenta. */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGE(TAG, "no puedo abrir la NVS para el resumen de %s: no escribo "
                      "unas cuentas a cero", carpeta);
        return false;
    }
    bool lecturas_ok = true;
    lecturas_ok &= nvs_get_d_verificado(h, NVS_T_LITROS, &litros);
    lecturas_ok &= nvs_get_d_verificado(h, NVS_T_COMBUS, &combus);
    lecturas_ok &= nvs_get_d_verificado(h, NVS_T_PEAJE, &peaje);
    lecturas_ok &= nvs_get_d_verificado(h, NVS_T_BOMBONA, &bombona);
    lecturas_ok &= nvs_get_d_verificado(h, NVS_T_MANTEN, &manten);
    lecturas_ok &= nvs_get_d_verificado(h, NVS_T_AGUA, &agua);
    lecturas_ok &= nvs_get_d_verificado(h, NVS_T_ALOJA, &aloja);
    {
        esp_err_t e = nvs_get_u32(h, NVS_T_PARADAS, &paradas);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) lecturas_ok = false;
        e = nvs_get_u64(h, NVS_T_INICIO, &inicio);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) lecturas_ok = false;
        e = nvs_get_u8(h, NVS_T_VARIAS, &varias);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) lecturas_ok = false;
        e = nvs_get_u32(h, NVS_T_KM_CUADRO, &km_cuadro);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) lecturas_ok = false;
        e = nvs_get_u32(h, NVS_T_KM_LLEGADA, &km_llegada);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) lecturas_ok = false;
        size_t n = sizeof(moneda);
        e = nvs_get_str(h, NVS_T_MONEDA, moneda, &n);
        if (e == ESP_ERR_NVS_NOT_FOUND) snprintf(moneda, sizeof(moneda), "EUR");
        else if (e != ESP_OK) lecturas_ok = false;
    }
    nvs_close(h);
    if (!lecturas_ok) {
        ESP_LOGE(TAG, "la NVS no me da los totales de %s: no escribo el resumen "
                      "(el viaje sigue abierto y se reintenta)", carpeta);
        return false;
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
        /* ACOTADA: 'inicio' es un epoch guardado en NVS, y si el reloj ha dado
         * un salto (el GPS corrige una hora que venia de un respaldo) la resta
         * sale inflada -- o negativa si el reloj fue hacia atras --. Un viaje de
         * mas de 90 dias no existe en esta autocaravana: si sale eso, es el
         * reloj, y se dice (revision del 13-oct-2026, hallazgo M4). */
        if (dias < 1 || dias > 90) {
            fprintf(f, "Duracion: no se puede calcular (el reloj de la P4 dio un\n"
                       "  salto durante el viaje: %ld dias de diferencia)\n", dias);
        } else {
            fprintf(f, "Duracion: %ld dia%s\n", dias, dias == 1 ? "" : "s");
        }
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
    /* Tiempo CONDUCIENDO (16-oct-2026): lo suma el ordenador de viaje con la
     * velocidad del GPS, asi que es tiempo de volante y no tiempo con la
     * carpeta abierta (las noches paradas de un viaje de tres dias no cuentan).
     * Por debajo del minuto no se saca -- no aporta -- y sin km medidos no se
     * saca nada, para no escribir un "0 h 00 min" que se lee como un viaje
     * sentado cuando lo que pasa es que el GPS no vio el cielo. */
    if (t.seconds_driving >= 60) {
        long tot_min = (long)(t.seconds_driving / 60);
        fprintf(f, "  Conduciendo: %ld h %02ld min\n", tot_min / 60, tot_min % 60);
    } else if (t.km >= 0.1) {
        fprintf(f, "  Conduciendo: menos de 1 min\n");
    }
    /* ── El cuadro del vehiculo: al salir y al llegar (16-oct-2026) ─────────
     *
     * Con las dos lecturas se sabe lo que ha hecho el viaje DE VERDAD, que es lo
     * que uno mira al llegar a casa, y se puede comparar con lo que midio el
     * GPS: si discrepan mucho, o el GPS se perdio un tramo o alguien tecleo mal
     * un numero. Cada dato se cuenta por separado (puede haber solo una de las
     * dos lecturas: se puede dejar en blanco en cualquiera de los dos pasos, y el
     * viaje puede haberse empezado en una pantalla y cerrado en otra). */
    if (km_cuadro) {
        fprintf(f, "  Cuadro al salir: %lu km\n", (unsigned long)km_cuadro);
    } else {
        fprintf(f, "  Cuadro al salir: no se apunto\n");
    }
    if (km_llegada) {
        fprintf(f, "  Cuadro al llegar: %lu km\n", (unsigned long)km_llegada);
    } else {
        fprintf(f, "  Cuadro al llegar: no se apunto\n");
    }
    if (km_cuadro && km_llegada) {
        if (km_llegada < km_cuadro) {
            /* Un cuadro no va hacia atras: eso es un dedazo, y restar daria un
             * numero negativo con toda la pinta de ser un dato. */
            fprintf(f, "  OJO: el cuadro al llegar es MENOR que al salir.\n"
                       "  Alguna de las dos lecturas esta mal: no se puede decir\n"
                       "  cuanto ha hecho el viaje de verdad.\n");
        } else {
            unsigned long real = (unsigned long)(km_llegada - km_cuadro);
            /* Comparacion con el GPS solo si el GPS midio algo: sin posicion no
             * hay con que comparar, y un "0.0 km" al lado de 333 se leeria como
             * un fallo del GPS cuando lo que pasa es que no vio el cielo. */
            if (t.km >= 0.1) {
                double dif = (double)real - t.km;
                fprintf(f, "  El cuadro dice %lu km; el GPS midio %.1f km "
                           "(%+.1f km)\n", real, t.km, dif);
            } else {
                fprintf(f, "  El cuadro dice %lu km\n", real);
            }
            ESP_LOGI(TAG, "viaje de cuadro: %lu -> %lu km = %lu km (GPS %.1f km)",
                     (unsigned long)km_cuadro, (unsigned long)km_llegada,
                     real, t.km);
        }
    }
    /* fclose devuelve error si el volcado a la tarjeta fallo (disco lleno):
     * hasta aqui todo eran fprintf a un buffer que puede no haber bajado. Y
     * fsync antes: este es EL fichero que dice si un viaje esta cerrado, y no
     * puede quedarse en la cache de la tarjeta despues de haber contestado que
     * si (auditoria del 13-oct-2026). Y fflush antes del fsync: fsync va sobre
     * el descriptor, no sobre el buffer de stdio del FILE* (14-oct-2026). */
    fflush(f);
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
 * DEVUELVE si se ha podido DEJAR DICHO lo que pasa (no si el viaje esta
 * completo), y deja en *faltan_out cuantos apuntes faltan. Si hay que escribir
 * la marca y no se puede -- tarjeta llena justo ahi -- devuelve false: cerrar
 * entonces seria descargar el viaje como verificado sin serlo, que es justo lo
 * que la marca existe para evitar (auditoria del 14-oct-2026: antes el fopen
 * fallido no se miraba, no se escribia nada, y el viaje salia "Listo"). */
static bool comprobar_completo(const cJSON *j, const char *carpeta, uint32_t *faltan_out)
{
    const cJSON *je = cJSON_GetObjectItem(j, "eventos");
    char ruta[RUTA_MAX];
    snprintf(ruta, sizeof(ruta), "%s/" MARCA_INCOMPLETO, carpeta);
    *faltan_out = 0;

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
        if (!f) {
            ESP_LOGE(TAG, "no puedo escribir %s (disco lleno?): el cierre SIN "
                          "VERIFICAR no quedaria marcado", ruta);
            return false;
        }
        fprintf(f, "Este viaje se cerro SIN PODER COMPROBARLO.\n\n"
                   "El cierre lo mando una pantalla que no llevaba la cuenta de\n"
                   "los apuntes generados (se cambiaria la cabina, o se le borro\n"
                   "la memoria a mitad de viaje), asi que la P4 no puede saber si\n"
                   "falta algo.\n\n"
                   "Es probable que este entero, pero NO esta verificado: comparalo\n"
                   "con lo que recuerdes antes de darlo por bueno.\n");
        fflush(f);
        int e_sync = fsync(fileno(f));
        int e_close = fclose(f);
        if (e_sync != 0 || e_close != 0) {
            ESP_LOGE(TAG, "%s no se escribio bien (disco lleno?)", ruta);
            return false;
        }
        ESP_LOGW(TAG, "cierre SIN VERIFICAR de %s: el fin no traia contador de "
                      "eventos", carpeta);
        return true;                         /* no se puede afirmar que falten */
    }

    uint32_t esperados = (uint32_t)je->valuedouble;
    uint32_t aplicados = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, NVS_T_APLIC, &aplicados);
        nvs_close(h);
    }
    aplicados++;                            /* este mismo fin */

    /* ADEMAS DE LOS CONTADORES, LAS FILAS (auditoria del 14-oct-2026): los
     * totales van por NVS, asi que una fila que se quedo a medias por un corte
     * -- el fsync llega al final del fichero, no en cada linea -- no se notaba
     * de ninguna forma. Se cuentan las lineas CON DATOS de eventos.csv (menos
     * la cabecera) y, si hay MENOS que apuntes aplicados, es que se perdio
     * alguna. Solo en ese sentido: filas de mas las hay legitimas (la nota de
     * hora corregida, los volcados, un fin reintentado), y contarlas como
     * sobrantes marcaria INCOMPLETO un viaje entero. */
    uint32_t filas = 0;
    {
        char ruta_ev[RUTA_MAX];
        snprintf(ruta_ev, sizeof(ruta_ev), "%s/eventos.csv", carpeta);
        FILE *fe = fopen(ruta_ev, "r");
        if (fe) {
            /* 1024: el formato del viaje EN CURSO con su boton son ~250 chars mas
             * 3 veces el nombre escapado; con 512 un nombre largo partia el
             * <button> a media etiqueta (revision adversarial del 14-oct-2026). */
            char linea[1024];
            bool cabecera = true;
            while (fgets(linea, sizeof(linea), fe)) {
                bool vacia = (linea[0] == '\n' || linea[0] == '\r' || linea[0] == 0);
                if (cabecera) { cabecera = false; continue; }
                if (!vacia) filas++;
            }
            fclose(fe);
        }
    }
    if (filas > 0 && filas < aplicados) {
        uint32_t por_filas = aplicados - filas;
        ESP_LOGW(TAG, "en %s hay %lu filas y %lu apuntes aplicados: faltan %lu "
                      "lineas en el diario", carpeta, (unsigned long)filas,
                 (unsigned long)aplicados, (unsigned long)por_filas);
        if (por_filas > *faltan_out) *faltan_out = por_filas;
    }

    if (aplicados < esperados) {
        uint32_t faltan = esperados - aplicados;
        if (faltan > *faltan_out) *faltan_out = faltan;
    }
    if (*faltan_out == 0) {
        /* Y SI YA NO FALTA NADA, LA MARCA SE RETIRA (auditoria del 14-oct-2026).
         * La marca se escribe antes del resumen: si el resumen falla (507, sin
         * sitio), el viaje sigue abierto CON la marca puesta; al reintentar con
         * sitio los apuntes que faltaban ya han llegado (la cabina los reintenta
         * cada 15 s) y sin este remove() el viaje se cerraba marcado INCOMPLETO
         * para siempre: estado_de() mira la marca ANTES que el resumen, asi que
         * la descarga quedaba en "de todos modos" y el nombre gritando. */
        if (remove(ruta) == 0) ESP_LOGI(TAG, "retirada la marca INCOMPLETO de %s", carpeta);
        return true;
    }

    uint32_t faltan = *faltan_out;
    FILE *f = fopen(ruta, "w");
    if (!f) {
        ESP_LOGE(TAG, "no puedo escribir %s (disco lleno?): el viaje quedaria "
                      "como verificado faltandole %lu apuntes", ruta,
                 (unsigned long)faltan);
        return false;
    }
    fprintf(f, "A este viaje le faltan %lu apunte%s.\n\n",
            (unsigned long)faltan, faltan == 1 ? "" : "s");
    fprintf(f, "La pantalla de la cabina dice haber generado %lu y aqui han\n"
               "llegado %lu. Se perdieron por el camino: la cola se lleno, o\n"
               "hubo un reinicio en mal momento.\n\n",
            (unsigned long)esperados, (unsigned long)aplicados);
    /* ANTES DE DARLOS POR PERDIDOS (auditoria del 14-oct-2026): un apunte que
     * llega DESPUES de cerrarse el viaje no se puede meter aqui -- el resumen ya
     * esta escrito y los totales no se rehacen --, asi que la P4 lo guarda en el
     * HISTORIAL DEL VEHICULO (carpeta 'vehiculo'). Decir solo "se perdieron por
     * el camino" era falso y mandaba a buscar un dato que si esta. */
    fprintf(f, "ANTES DE DARLO POR PERDIDO: un apunte que llega DESPUES de\n"
               "cerrarse el viaje no puede entrar aqui (el resumen ya esta\n"
               "escrito), asi que se guarda en el HISTORIAL DEL VEHICULO:\n"
               "la carpeta 'vehiculo' de la tarjeta, o /data/viajes ->\n"
               "Historial del vehiculo en el portal.\n\n");
    fprintf(f, "Lo demas es correcto. Este aviso esta para que no des el\n"
               "viaje por entero al analizarlo.\n");
    fflush(f);
    int e_sync = fsync(fileno(f));
    int e_close = fclose(f);
    if (e_sync != 0 || e_close != 0) {
        ESP_LOGE(TAG, "%s no se escribio bien (disco lleno?)", ruta);
        return false;
    }
    ESP_LOGW(TAG, "viaje INCOMPLETO: esperados %lu, aplicados %lu, filas %lu",
             (unsigned long)esperados, (unsigned long)aplicados, (unsigned long)filas);
    return true;
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
    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) {
        /* Sin viaje abierto no hay a que cambiarle el nombre. 200 y no 409
         * (auditoria del 14-oct-2026): el destino es ACCESORIO -- el fin lo
         * lleva, y la cabina lo tiene en su NVS -- y con 409 la cola del
         * satelite se queda reintentando para siempre este mensaje, con todo lo
         * de detras (incluido el fin) atascado detras. */
        ESP_LOGW(TAG, "destino sin viaje abierto: lo doy por hecho");
        responde_con_hueco(req, "no hay viaje abierto: destino ignorado");
        return ESP_OK;
    }

    /* Y SI EL VIAJE ES DE OTRA PANTALLA, ESTE AVISO NO SE APUNTA (auditoria del
     * 14-oct-2026): esto es lo unico que escribe NVS_DESTINO, y op_fin lo usa
     * como respaldo cuando su fin no trae destino. Un aviso de la pantalla B
     * entregado tarde (con el viaje de A abierto) hacia que el FIN DE A
     * renombrase su carpeta con el destino de B. 200 igualmente: es un aviso
     * accesorio y un error atascaria la cola. */
    if (pide_otra_pantalla(j, id, false)) {
        ESP_LOGW(TAG, "destino de OTRA pantalla con el viaje %s abierto: lo ignoro",
                 carpeta);
        responde_con_hueco(req, "ese viaje no es de esta pantalla: destino ignorado");
        return ESP_OK;
    }

    const cJSON *jd = cJSON_GetObjectItemCaseSensitive(j, "destino");
    if (!cJSON_IsString(jd)) {
        httpd_resp_set_status(req, "400 Bad Request");
        responde_con_hueco(req, "falta 'destino'");
        return ESP_OK;
    }
    char nd[DESTINO_MAX];
    destino_seguro(jd->valuestring, nd, sizeof(nd));

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        responde_con_hueco(req, "no puedo guardar el destino");
        return ESP_OK;
    }
    esp_err_t e = nvs_set_str(h, NVS_DESTINO, nd);
    if (e == ESP_OK) e = nvs_commit(h);
    nvs_close(h);
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        responde_con_hueco(req, "no puedo guardar el destino");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "destino nuevo APUNTADO (sin renombrar): %s -> %s", carpeta, nd);
    responde_con_hueco(req, nd);
    return ESP_OK;
}

/* Renombrado del cierre que se quedo a medias.
 *
 * op_fin limpia el estado en NVS y DESPUES renombra la carpeta (a proposito: al
 * reves la NVS podia quedar apuntando a una carpeta que ya no existe). Si se
 * corta la corriente entre las dos cosas, o si el rename falla -- destino
 * ocupado, tarjeta llena --, el viaje queda cerrado con el nombre VIEJO. El
 * reintento del fin caia entonces en "no habia viaje abierto" y el renombrado
 * no se hacia NUNCA, en silencio: el usuario habia cambiado el destino a mitad
 * de viaje, la pantalla se lo enseno toda la ruta, y la carpeta se quedaba con
 * el nombre de antes para siempre (auditoria del 14-oct-2026).
 *
 * Se apunta lo que hay que renombrar ANTES de limpiar el estado, y lo termina
 * el siguiente fin (el reintento de la cabina, o el cierre de otro viaje). */
static void renombrado_pendiente_marcar(const char *de, const char *a)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    if (nvs_set_str(h, NVS_REN_DE, de) == ESP_OK &&
        nvs_set_str(h, NVS_REN_A, a) == ESP_OK)
        nvs_commit(h);
    else
        ESP_LOGW(TAG, "no he podido apuntar el renombrado pendiente %s -> %s", de, a);
    nvs_close(h);
}

static void renombrado_pendiente_olvidar(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, NVS_REN_DE);
    nvs_erase_key(h, NVS_REN_A);
    nvs_commit(h);
    nvs_close(h);
}

/* Aplica el renombrado pendiente, si lo hay. Se llama con la SD YA bloqueada. */
static void renombrado_pendiente_aplicar(void)
{
    char de[64] = {0}, a[64] = {0};
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    size_t nd = sizeof(de), na = sizeof(a);
    if (nvs_get_str(h, NVS_REN_DE, de, &nd) != ESP_OK) de[0] = 0;
    if (nvs_get_str(h, NVS_REN_A, a, &na) != ESP_OK) a[0] = 0;
    nvs_close(h);
    if (!de[0] || !a[0]) return;

    char viejo[RUTA_MAX], nuevo[RUTA_MAX];
    snprintf(viejo, sizeof(viejo), VIAJES_DIR "/%s", de);
    snprintf(nuevo, sizeof(nuevo), VIAJES_DIR "/%s", a);

    /* NO SE TOCA EL VIAJE ABIERTO (auditoria del 14-oct-2026). La marca se
     * apunta ANTES de limpiar el estado, asi que si el cierre no llego a
     * completarse (507 del resumen, fallo de NVS) el viaje SIGUE ABIERTO con la
     * marca puesta: sin este guard, el reintento del fin renombraba la carpeta
     * VIVA y luego el stat de op_fin fallaba -> estado limpio y 200 "sin
     * resumen", con la telemetria y los apuntes escribiendo a una ruta que ya no
     * existe. */
    char abierto[CARPETA_MAX];
    if (viaje_abierto(abierto, sizeof(abierto))) {
        const char *base = strrchr(abierto, '/');
        base = base ? base + 1 : abierto;
        if (!strcmp(base, de)) {
            ESP_LOGW(TAG, "renombrado pendiente %s: es el viaje ABIERTO, no lo toco", de);
            return;
        }
    }
    struct stat st;
    /* Y solo se renombra lo que esta CERRADO: un cierre siempre deja
     * resumen.txt (op_fin no cierra sin el). */
    snprintf(viejo, sizeof(viejo), VIAJES_DIR "/%s/resumen.txt", de);
    if (stat(viejo, &st) != 0) {
        ESP_LOGW(TAG, "renombrado pendiente %s: no tiene resumen.txt, no lo toco", de);
        return;
    }
    snprintf(viejo, sizeof(viejo), VIAJES_DIR "/%s", de);
    if (stat(viejo, &st) != 0) {
        /* Ya esta hecho (o la carpeta se fue): no hay nada que hacer. */
        ESP_LOGI(TAG, "renombrado pendiente %s: la carpeta vieja no esta, lo doy por hecho", de);
        renombrado_pendiente_olvidar();
        return;
    }
    if (stat(nuevo, &st) == 0) {
        /* El destino ya existe: no se pisa. Se deja el nombre viejo, que es un
         * estado valido (los datos y el resumen estan dentro). */
        ESP_LOGW(TAG, "renombrado pendiente %s -> %s: el destino ya existe, no lo toco", de, a);
        renombrado_pendiente_olvidar();
        return;
    }
    if (rename(viejo, nuevo) == 0) {
        ESP_LOGI(TAG, "renombrado pendiente TERMINADO: %s -> %s", viejo, nuevo);
        renombrado_pendiente_olvidar();
    } else {
        ESP_LOGW(TAG, "renombrado pendiente %s -> %s: sigue sin poder hacerse", viejo, nuevo);
    }
}

static esp_err_t op_fin(httpd_req_t *req, const cJSON *j, uint32_t id)
{
    /* Si un cierre anterior dejo el renombrado del destino a medias, se termina
     * aqui: este es el primer momento en que se vuelve a tocar la tarjeta con
     * calma (ver renombrado_pendiente_marcar). */
    if (camera_sd_bus_lock(3000)) {
        renombrado_pendiente_aplicar();
        camera_sd_bus_unlock();
    }

    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) {
        /* Sin viaje abierto el fin ya no tiene nada que cerrar. Se responde OK
         * y no error: casi siempre sera un reintento de un fin que SI se
         * aplico, y devolver error dejaria al satelite atascado reintentando
         * algo que ya esta hecho. */
        ESP_LOGW(TAG, "fin sin viaje abierto: lo doy por hecho");
        responde_con_hueco(req, "no habia viaje abierto");
        return ESP_OK;
    }

    /* SI VIENE 'carpeta', TIENE QUE SER LA DEL VIAJE ABIERTO (revision
     * adversarial del 14-oct-2026). Hasta ahora este campo era decorativo: el
     * cierre era "lo que este abierto AHORA", asi que un clic desde una pestaña
     * vieja del portal (o entre el render de la lista y el clic, que la P4 no
     * empuja estado) cerraba el viaje que una cabina acababa de empezar, con un
     * 200 y sin que nadie se enterase. Las cabinas NO mandan 'carpeta' en el
     * fin, asi que esto solo afecta a quien lo manda a proposito (la web). */
    {
        const cJSON *jc = cJSON_GetObjectItemCaseSensitive(j, "carpeta");
        if (cJSON_IsString(jc) && jc->valuestring[0]) {
            const char *base = strrchr(carpeta, '/');
            base = base ? base + 1 : carpeta;
            if (strcmp(base, jc->valuestring) != 0) {
                ESP_LOGW(TAG, "fin de '%s' pero el viaje abierto es '%s': no cierro nada",
                         jc->valuestring, base);
                httpd_resp_set_status(req, "409 Conflict");
                responde_con_hueco(req, "el viaje abierto ya no es ese: recarga la lista");
                return ESP_OK;
            }
        }
    }

    /* EL CIERRE LO PIDE EL DUENO (auditoria del 14-oct-2026). 409 y no 412
     * porque esto llega por la COLA de la cabina: el 409 es el codigo que ya
     * sabe leer y enseñar, y el 412 solo lo entiende la pantalla que lo pide al
     * empezar. Sin esto, "GUARDARLO" desde la otra pantalla cerraba -- y le
     * ponia resumen -- al viaje que la primera seguia grabando. */
    if (pide_otra_pantalla(j, id, true)) {
        ESP_LOGW(TAG, "fin de %s RECHAZADO: lo pide otra pantalla", carpeta);
        httpd_resp_set_status(req, "409 Conflict");
        responde_con_hueco(req, "ese viaje lo empezo otra pantalla: cierralo desde ella");
        return ESP_OK;
    }

    if (!camera_sd_bus_lock(3000)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        responde_con_hueco(req, "tarjeta ocupada");
        return ESP_OK;
    }
    /* La carpeta del viaje abierto puede NO ESTAR: se saco la tarjeta y se metio
     * otra, se formateo, o se borro desde el PC. Antes esto era un 507 para
     * SIEMPRE: la unica salida era reiniciar la P4 con la tarjeta puesta, y la
     * P4 vive en la parte de atras (auditoria del 14-oct-2026). Si la SD ESTA
     * MONTADA, que la carpeta no exista es un hecho consumado: sus datos no
     * estan en esta tarjeta, asi que se limpia el estado y se dice. El 507 se
     * queda para cuando la SD NO esta montada, porque ahi su ausencia no
     * significa nada (puede volver a ponerse). */
    if (datalogger_sd_montada() && carpeta_fuera_de_la_tarjeta(carpeta)) {
        camera_sd_bus_unlock();
        {
            ESP_LOGE(TAG, "el viaje abierto %s NO ESTA en la tarjeta montada "
                          "(cambiada, formateada o borrada desde el PC): limpio "
                          "el estado. Los datos de ese viaje no estan aqui",
                     carpeta);
            estado_set(NULL, id);
            responde_con_hueco(req, "la carpeta del viaje ya no estaba: "
                                    "estado limpiado, sin resumen");
            return ESP_OK;
        }
        ESP_LOGE(TAG, "el viaje abierto %s NO ESTA donde la tarjeta dice (y la "
                      "SD no esta montada, o el fallo no es un ENOENT claro): no "
                      "puedo cerrarlo", carpeta);
        httpd_resp_set_status(req, "507 Insufficient Storage");
        responde_con_hueco(req, "la carpeta del viaje no esta en la tarjeta");
        return ESP_OK;
    }
    /* EL CUENTAKILOMETROS DEL CIERRE, a NVS ANTES del resumen (16-oct-2026):
     * escribir_resumen() lee de la NVS, asi que si esto fuera despues el resumen
     * saldria sin la lectura.
     *
     * SOLO SI HAY LECTURA (correccion del 16-oct-2026): antes se escribia
     * SIEMPRE, tambien con 0, asi que un fin que llegaba sin el campo -- la
     * reconciliacion del arranque, un reintento de una cabina con firmware viejo,
     * o el mismo usuario dejandolo en blanco -- BORRABA la lectura que si se habia
     * apuntado en el primer intento. El comentario decia justo lo contrario ("la
     * del primer intento sigue siendo la buena") mientras el codigo la pisaba.
     * Con 0 no se toca: 0 significa "no apuntado", no "marcaba cero".
     * Se guarda aunque el cierre falle mas abajo (507): el fin se reintenta y esa
     * lectura ES la del viaje que se esta cerrando. */
    {
        uint32_t kml = km_cuadro_de(j);
        if (kml) {
            nvs_handle_t hk;
            esp_err_t ek = nvs_open(NVS_NS, NVS_READWRITE, &hk);
            if (ek == ESP_OK) {
                ek = nvs_set_u32(hk, NVS_T_KM_LLEGADA, kml);
                if (ek == ESP_OK) ek = nvs_commit(hk);
                nvs_close(hk);
            }
            if (ek == ESP_OK) {
                ESP_LOGI(TAG, "cuadro al llegar: %lu km", (unsigned long)kml);
            } else {
                /* No es motivo para no cerrar el viaje: la lectura es un extra. */
                ESP_LOGW(TAG, "no he podido guardar el cuadro al llegar (%lu km): %s",
                         (unsigned long)kml, esp_err_to_name(ek));
                anota_fallo_nvs();
            }
        }
    }

    bool escrito = diario(carpeta, "fin", "");
    /* Lo que quede de ruta en memoria, a la tarjeta ANTES de cerrar. No condiciona
     * el cierre: si esto falla se pierden como mucho los ultimos minutos de traza,
     * y dejar el viaje abierto por eso seria peor que el mal que evita. */
    ruta_cerrar(carpeta);
    uint32_t faltan = 0;
    if (!comprobar_completo(j, carpeta, &faltan)) {
        /* No se ha podido DEJAR DICHO que falta algo (o que no se pudo
         * verificar). Cerrar asi seria bajarlo como verificado sin serlo: el
         * viaje sigue abierto y el fin se reintenta cuando se pueda escribir. */
        camera_sd_bus_unlock();
        anota_fallo_escritura();
        ESP_LOGE(TAG, "no he podido marcar %s como INCOMPLETO/SIN VERIFICAR: el "
                      "viaje NO se cierra", carpeta);
        httpd_resp_set_status(req, "507 Insufficient Storage");
        responde_con_hueco(req, "no se ha podido marcar el viaje: sigue abierto");
        return ESP_OK;
    }
    escrito = escribir_resumen(carpeta) && escrito;
    if (faltan) {
        FILE *rf; char rr[RUTA_MAX];
        snprintf(rr, sizeof(rr), "%s/resumen.txt", carpeta);
        rf = fopen(rr, "a");
        if (rf) {
            fprintf(rf, "\n*** VIAJE INCOMPLETO: faltan %lu apuntes. ***\n",
                    (unsigned long)faltan);
            fprintf(rf, "Los totales de arriba NO los incluyen. Ver "
                        MARCA_INCOMPLETO ".\n"
                        "Puede que NO se hayan perdido: mira el historial del\n"
                        "vehiculo (carpeta 'vehiculo' de la tarjeta).\n");
            /* fsync: este añadido es lo que deja claro que faltan apuntes. */
            fflush(rf);
            fsync(fileno(rf));
            fclose(rf);
        } else {
            /* La marca INCOMPLETO.txt YA esta escrita (es la que manda para el
             * estado y para el nombre), asi que esto es solo el resumen: se
             * deja constancia y se sigue, que el cierre no depende de ello. */
            ESP_LOGW(TAG, "no he podido anotar en resumen.txt que faltan %lu "
                          "apuntes (la marca INCOMPLETO.txt si esta)",
                     (unsigned long)faltan);
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
        anota_fallo_escritura();
        ESP_LOGE(TAG, "NO he podido escribir el cierre de %s: la tarjeta esta "
                      "llena o falla. El viaje sigue ABIERTO", carpeta);
        httpd_resp_set_status(req, "507 Insufficient Storage");
        responde_con_hueco(req, "no se ha podido escribir el cierre");
        return ESP_OK;
    }

    /* ── CAMBIO DE DESTINO APLAZADO AL CIERRE (12-oct-2026) ─────────────────
     *
     * El satelite deja cambiar el destino a mitad de viaje, pero NO renombra
     * nada mientras corre: guarda el destino nuevo y lo manda AQUI, al cerrar.
     * Es a proposito y esta razonado en cabina5/docs/RELEVO_2026-10-12.md:
     * mientras el viaje esta abierto hay ficheros escribiendose (ruta,
     * telemetria) y renombrar la carpeta por debajo obligaria a cerrarlos,
     * renombrarlos y reabrirlos, dependiendo ademas de que la P4 contestara en
     * ese momento. Al cerrar, los ficheros ya se cierran de todas formas y el
     * resumen ya esta escrito: renombrar aqui es un solo rename() y no hay
     * nada dentro de la carpeta que apunte a su nombre (todo va por fecha_hora).
     *
     * La FECHA del nombre se conserva: el viaje empezo ese dia, aunque el
     * destino se cambiara despues. Solo cambia lo que va detras del guion.
     *
     * SE CALCULA ANTES DE LIMPIAR EL ESTADO (auditoria del 14-oct-2026): si el
     * nombre nuevo se decide despues y el rename no llega a hacerse (corte de
     * corriente, destino ocupado), el reintento del fin ya no tiene de donde
     * sacarlo y el renombrado no se hacia nunca. Se apunta en NVS antes del
     * commit y lo termina renombrado_pendiente_aplicar(). */
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
    char nueva[RUTA_MAX] = {0};
    bool hay_renombrado = false;
    {
        const char *base = strrchr(carpeta, '/');
        base = base ? base + 1 : carpeta;          /* "2026-10-11_arantzazu" */
        if (nd[0]) {
            char fecha[12] = {0};
            const char *guion = strchr(base, '_');
            size_t nf = guion ? (size_t)(guion - base) : 0;
            if (nf >= sizeof(fecha)) nf = sizeof(fecha) - 1;
            memcpy(fecha, base, nf);
            fecha[nf] = 0;

            int nn = snprintf(nueva, sizeof(nueva), VIAJES_DIR "/%s_%s", fecha, nd);
            /* Si el destino no ha cambiado de verdad, no se toca nada. */
            if (nn > 0 && nn < (int)sizeof(nueva) && strcmp(nueva, carpeta) != 0) {
                /* rename() falla si el destino ya existe: se numera, igual que al
                 * apartar un viaje. */
                struct stat st;
                for (int k = 2; k < 100 && stat(nueva, &st) == 0; k++) {
                    nn = snprintf(nueva, sizeof(nueva), VIAJES_DIR "/%s_%s_%d",
                                  fecha, nd, k);
                    if (nn < 0 || nn >= (int)sizeof(nueva)) break;
                }
                hay_renombrado = true;
                char a[64];
                snprintf(a, sizeof(a), "%s", nueva + strlen(VIAJES_DIR) + 1);
                renombrado_pendiente_marcar(base, a);
            }
        }
    }

    /* El cierre YA ESTA en la tarjeta. El estado se limpia ANTES de renombrar:
     * asi el nombre de la carpeta y lo que la P4 cree que hay abierto no pueden
     * quedar desalineados, ni siquiera si se corta la corriente en medio. Si el
     * corte llega entre el commit y el rename, el viaje queda cerrado con el
     * nombre viejo -- estado valido: los datos y el resumen estan dentro -- y el
     * renombrado lo termina el siguiente fin (ver renombrado_pendiente_*). */
    if (estado_set(NULL, id) != ESP_OK) {
        camera_sd_bus_unlock();
        ESP_LOGE(TAG, "cierre de %s escrito, pero no he podido olvidarlo en NVS: "
                      "el viaje sigue abierto y habra que reintentar", carpeta);
        anota_fallo_nvs();
        httpd_resp_set_status(req, "507 Insufficient Storage");
        responde_sin_sitio_nvs(req, "no se ha podido cerrar el viaje en la NVS");
        return ESP_OK;
    }

    if (hay_renombrado) {
        if (rename(carpeta, nueva) == 0) {
            ESP_LOGI(TAG, "DESTINO CAMBIADO: %s -> %s", carpeta, nueva);
            renombrado_pendiente_olvidar();
        } else {
            /* El viaje se cierra igual: el resumen y los datos estan bien, solo
             * se queda con el nombre viejo. Perder el cierre por no poder
             * renombrar seria mucho peor. La marquita queda apuntada para
             * intentarlo en el proximo fin. */
            ESP_LOGW(TAG, "no he podido renombrar %s a %s: se queda con el "
                          "nombre viejo (reintentare al proximo cierre)",
                     carpeta, nueva);
        }
    }
    camera_sd_bus_unlock();

    /* La energia ya esta volcada en el resumen de arriba. Se guarda en NVS y se
     * cierra, para que los contadores no sigan sumando a cuenta de un viaje que
     * ya termino. */
    trip_computer_end();
    ESP_LOGI(TAG, "VIAJE CERRADO: %s", carpeta);
    responde_con_hueco(req, carpeta);
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
/* BORRAR UN VIAJE DE LA TARJETA. El nucleo, sin HTTP: lo usan la app (JSON,
 * op_borrar) y la PAPELERA DE LA WEB (formulario, handle_data_viaje_borrar).
 *
 * Devuelve 0 si se borro, o un codigo HTTP con el motivo en 'motivo'. */
int viaje_borrar_carpeta(const char *nombre, char *motivo, size_t n)
{
    if (!nombre || !nombre[0]) {
        snprintf(motivo, n, "falta el nombre");
        return 400;
    }
    /* NI "." NI NADA QUE EMPIECE POR PUNTO (16-oct-2026, arbitraje). La guarda
     * rechazaba '/' y "..", pero NO ".": con {"op":"borrar","carpeta":"."} la
     * ruta quedaba en /sdcard/viajes/. y borrar_arbol() se llevaba por delante
     * TODAS las carpetas de viaje (y el rmdir final fallaba, asi que ademas se
     * contestaba 500 "no se pudo borrar todo"). Demostrado en host con la funcion
     * real: las tres carpetas de prueba, borradas. */
    if (strlen(nombre) > 96 || strchr(nombre, '/') || strstr(nombre, "..") ||
        nombre[0] == '.') {
        ESP_LOGW(TAG, "borrar: nombre rechazado (%.40s)", nombre);
        snprintf(motivo, n, "nombre de carpeta no valido");
        return 400;
    }
    char abierto[CARPETA_MAX];
    if (viaje_abierto(abierto, sizeof(abierto))) {
        const char *base = strrchr(abierto, '/');
        base = base ? base + 1 : abierto;
        if (!strcmp(base, nombre)) {
            snprintf(motivo, n, "ese viaje esta en curso: cierralo antes de borrarlo");
            return 409;
        }
    }
    char ruta[RUTA_MAX + 64];
    int k = snprintf(ruta, sizeof(ruta), VIAJES_DIR "/%s", nombre);
    if (k < 0 || k >= (int)sizeof(ruta)) {
        snprintf(motivo, n, "nombre demasiado largo");
        return 400;
    }
    if (!camera_sd_bus_lock(3000)) {
        snprintf(motivo, n, "tarjeta ocupada, reintenta");
        return 503;
    }
    struct stat st;
    if (stat(ruta, &st) != 0) {
        camera_sd_bus_unlock();
        snprintf(motivo, n, "ese viaje no esta en la tarjeta");
        return 404;
    }
    const int fallos = borrar_arbol(ruta, 0);
    camera_sd_bus_unlock();
    if (fallos) {
        ESP_LOGW(TAG, "borrar viaje '%s': %d fallo(s)", nombre, fallos);
        snprintf(motivo, n, "no se pudo borrar todo (tarjeta ocupada?)");
        return 500;
    }
    ESP_LOGW(TAG, "VIAJE BORRADO: %s", nombre);
    if (motivo && n) motivo[0] = 0;
    return 0;
}

static esp_err_t op_borrar(httpd_req_t *req, const cJSON *j, uint32_t id)
{
    (void)id;   /* 'borrar' no lleva id: ver viaje_campos_que_faltan */
    const cJSON *jc = cJSON_GetObjectItemCaseSensitive(j, "carpeta");
    if (!cJSON_IsString(jc) || !jc->valuestring[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        responde_con_hueco(req, "falta 'carpeta'");
        return ESP_OK;
    }
    char motivo[96] = {0};
    int cod = viaje_borrar_carpeta(jc->valuestring, motivo, sizeof(motivo));
    if (cod) {
        char cuerpo[128];
        snprintf(cuerpo, sizeof(cuerpo), "%d %s", cod, motivo);
        httpd_resp_set_status(req, cod == 400 ? "400 Bad Request"
                              : cod == 404 ? "404 Not Found"
                              : cod == 409 ? "409 Conflict"
                              : cod == 503 ? "503 Service Unavailable"
                                           : "500 Internal Server Error");
        responde_con_hueco(req, motivo);
        return ESP_OK;
    }
    responde_con_hueco(req, "viaje borrado");
    return ESP_OK;
}

static esp_err_t op_descartar(httpd_req_t *req, const cJSON *j, uint32_t id)
{
    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) {
        /* Mismo criterio que op_fin: casi siempre es un reintento de algo que
         * ya se aplico, y un error dejaria al satelite atascado. */
        ESP_LOGW(TAG, "descartar sin viaje abierto: lo doy por hecho");
        responde_con_hueco(req, "no habia viaje abierto");
        return ESP_OK;
    }

    /* APARTAR TAMPOCO LO PUEDE HACER CUALQUIERA (auditoria del 14-oct-2026).
     * Esto empieza por "(void)j;" y era el agujero mas gordo de todos: la
     * segunda pantalla recibia 412 al empezar, tenia delante el boton "apartarlo
     * (era una prueba)" y con el renombraba a DESCARTADO_ la carpeta del viaje
     * REAL de la primera, que seguia grabando. Sus apuntes se iban al historial
     * del vehiculo con un 200 y su fin se daba por hecho: el viaje entero
     * perdido como viaje, en silencio. */
    if (pide_otra_pantalla(j, id, true)) {
        ESP_LOGW(TAG, "descartar %s RECHAZADO: lo pide otra pantalla", carpeta);
        httpd_resp_set_status(req, "409 Conflict");
        responde_con_hueco(req, "ese viaje lo empezo otra pantalla: apartalo desde ella");
        return ESP_OK;
    }

    const char *nombre = strrchr(carpeta, '/');
    nombre = nombre ? nombre + 1 : carpeta;

    if (!camera_sd_bus_lock(3000)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        responde_con_hueco(req, "tarjeta ocupada");
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
        responde_con_hueco(req, "nombre demasiado largo");
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
    /* El bucle comprobaba el candidato ANTES de asignarlo, asi que el ultimo
     * ("..._99") no se llegaba a mirar y el rename fallaba con un 500 opaco.
     * Con dos pruebas del mismo dia no se llega ni de lejos, pero el error
     * tiene que decir lo que pasa (auditoria del 14-oct-2026). */
    if (stat(destino, &st) == 0) {
        camera_sd_bus_unlock();
        ESP_LOGE(TAG, "no hay nombre libre para apartar %s", carpeta);
        httpd_resp_set_status(req, "500 Internal Server Error");
        responde_con_hueco(req, "no hay nombre libre para apartarlo");
        return ESP_OK;
    }

    diario(carpeta, "descartado", "");
    int r = rename(carpeta, destino);

    if (r != 0) {
        camera_sd_bus_unlock();
        ESP_LOGE(TAG, "no puedo apartar %s -> %s", carpeta, destino);
        httpd_resp_set_status(req, "500 Internal Server Error");
        responde_con_hueco(req, "no puedo apartar la carpeta");
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
        anota_fallo_nvs();
        httpd_resp_set_status(req, "507 Insufficient Storage");
        responde_sin_sitio_nvs(req, "no puedo apartar el viaje");
        return ESP_OK;
    }
    camera_sd_bus_unlock();

    /* Y los contadores del viaje, a NVS y a cero, igual que en el fin
     * (auditoria del 16-oct-2026): el viaje descartado dejaba de estar abierto en
     * la NVS pero trip_computer seguia sumando km y horas de conduccion a cuenta
     * de el, y esos numeros son los que van en la telemetria de la 5" -- o sea que
     * la tarjeta del viaje nuevo arrancaba con lo del descartado dentro. */
    trip_computer_end();
    ESP_LOGW(TAG, "VIAJE DESCARTADO: %s -> %s", carpeta, destino);
    responde_con_hueco(req, destino);
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
    fflush(f);   /* ver la nota de diario_en(): fsync no vacia el buffer de stdio */
    if (fsync(fileno(f)) != 0) {
        ESP_LOGW(TAG, "fsync fallo en %s (disco lleno?)", ruta);
        fclose(f);
        return false;
    }
    bool csv_ok = (fclose(f) == 0);
    if (csv_ok) anota_escritura_ok();
    return csv_ok;
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
        responde_con_hueco(req, "falta tipo");
        return ESP_OK;
    }
    /* El 'tipo' va al NOMBRE DE UN FICHERO ("<tipo>s.csv"), asi que se filtra
     * antes de usarlo: un tipo con "/" o ".." escribiria fuera de la carpeta
     * del viaje (auditoria del 14-oct-2026). No es alcanzable desde la pantalla
     * -- los tipos los pone el firmware -- pero esto es una API HTTP. */
    char tipo[24];
    {
        size_t k = 0;
        for (size_t i = 0; jt->valuestring[i] && k + 1 < sizeof(tipo); i++) {
            char c = jt->valuestring[i];
            bool vale = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
            if (vale) tipo[k++] = c;
        }
        tipo[k] = 0;
    }
    if (!tipo[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        responde_con_hueco(req, "tipo invalido");
        return ESP_OK;
    }
    if (strcmp(tipo, jt->valuestring) != 0)
        ESP_LOGW(TAG, "tipo con caracteres raros: '%s' -> '%s'", jt->valuestring, tipo);

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
    /* 'en_viaje' dice DONDE SE ESCRIBE el apunte; 'viaje_en_nvs' dice si la NVS
     * apunta a ese viaje abierto. Son dos cosas distintas desde que existe el
     * desvio a /sdcard/vehiculo: desviar el apunte NO puede borrar el viaje
     * abierto de la NVS (ver id_anota_en). */
    bool viaje_en_nvs = en_viaje;

    /* UN APUNTE DE OTRA PANTALLA NO SE MEZCLA CON ESTE VIAJE (auditoria del
     * 14-oct-2026): si el viaje abierto lo empezo otra cabina, este apunte no es
     * suyo -- la segunda pantalla puede estar grabando su propia salida sin
     * saber que la P4 ya tenia una abierta -- y meterlo en la misma carpeta
     * dejaba los dos viajes revueltos en los mismos CSV, sin forma de separarlos
     * despues. Se manda al historial del vehiculo, que es la red de seguridad
     * que ya existe, y se contesta 200 para no atascar la cola del satelite: un
     * 409 ahi la dejaria reintentando para siempre algo que no va a cambiar. */
    bool de_otra_pantalla = false;
    if (en_viaje && pide_otra_pantalla(j, id, false)) {
        ESP_LOGW(TAG, "apunte de OTRA pantalla con el viaje %s abierto: va al "
                      "historial del vehiculo en vez de mezclarse", carpeta);
        en_viaje = false;      /* se escribe en VEHICULO_DIR... */
        de_otra_pantalla = true;   /* ...y la fila lo dice */
        /* ...pero el viaje ABIERTO sigue siendo el de la otra: ni se toca. */
    }
    if (!en_viaje) snprintf(carpeta, sizeof(carpeta), VEHICULO_DIR);
    /* OJO: esta lectura es SIN el cerrojo (se toma mas abajo), asi que la
     * carpeta se vuelve a comprobar con el cerrojo tomado: op_fin cierra y
     * RENOMBRA precisamente con ese cerrojo, y escribir entonces iba a una ruta
     * que ya no existe (mismo fallo que se arreglo en el tick de telemetria;
     * detectado en la revision del 13-oct-2026). */

    /* Cuando ocurrio. */
    const cJSON *jts = cJSON_GetObjectItem(j, "ts");
    const cJSON *ja  = cJSON_GetObjectItem(j, "aprox");
    /* EL ts TIENE COTA POR ARRIBA (auditoria del 14-oct-2026): venia de la
     * cabina sin comprobar nada, y un valor absurdo hacia que strftime
     * devolviera 0 dejando 'cuando' SIN INICIALIZAR, que acababa tal cual en el
     * CSV. Fuera de [2001, 2100) no es una hora: se trata como aproximada y se
     * pone la de recepcion, que es lo honesto. */
    double ts_crudo = cJSON_IsNumber(jts) ? jts->valuedouble : 0.0;
    bool aprox = cJSON_IsTrue(ja) || !cJSON_IsNumber(jts) ||
                 ts_crudo < 1000000000.0 || ts_crudo > 4102444800.0;
    time_t t = aprox ? time(NULL) : (time_t)ts_crudo;
    struct tm tm_l;
    /* El ts del satelite ya viene en hora LOCAL (deriva de epoch_local), asi
     * que gmtime; la hora de recepcion, en cambio, es un epoch de verdad y
     * necesita localtime. Mezclarlos correria una de las dos dos horas. */
    char cuando[20] = {0};
    if ((aprox ? localtime_r(&t, &tm_l) : gmtime_r(&t, &tm_l)) == NULL ||
        strftime(cuando, sizeof(cuando), "%Y-%m-%d %H:%M:%S", &tm_l) == 0)
        snprintf(cuando, sizeof(cuando), "fecha desconocida");

    const cJSON *jr = cJSON_GetObjectItem(j, "resumen");
    char detalle[128];
    /* 'de_otra_pantalla' se sabe desde arriba: si el viaje abierto lo empezo
     * OTRA cabina, este apunte no es suyo y se escribe en el historial del
     * vehiculo (ver el comentario de mas arriba). Decirlo en la propia fila
     * evita que, al leer el historial, parezca un apunte suelto sin motivo. */
    snprintf(detalle, sizeof(detalle), "%s%s%s",
             cJSON_IsString(jr) ? jr->valuestring : "",
             aprox ? " (hora aproximada)" : "",
             de_otra_pantalla ? " (de otra pantalla)" : "");

    if (!camera_sd_bus_lock(3000)) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        responde_con_hueco(req, "tarjeta ocupada");
        return ESP_OK;
    }
    if (en_viaje) {
        char ahora_viaje[CARPETA_MAX];
        if (!viaje_abierto(ahora_viaje, sizeof(ahora_viaje)) ||
            strcmp(ahora_viaje, carpeta) != 0) {
            /* El viaje se cerro entre la lectura de arriba y el cerrojo (o es
             * otro). El apunte NO se escribe en la carpeta de un viaje cerrado
             * -- el cierre no siempre renombra, asi que la carpeta sigue ahi --
             * ni se vuelve a apuntar el viaje como abierto en NVS: se trata como
             * lo que es, un apunte SIN viaje abierto, y se va al historial del
             * vehiculo, que es el camino de seguridad que ya existe.
             *
             * La condicion es la del tick de telemetria. La primera version de
             * esto exigia "hay otro viaje y es distinto", y por ahi se colaba
             * justo el caso normal: el viaje cerrado SIN otro detras (lo cazo la
             * revision del 13-oct-2026). Y no se contesta 507: un apunte sin
             * viaje abierto no es un error, es el caso de la red de seguridad. */
            ESP_LOGW(TAG, "el viaje %s ya no esta abierto: el apunte va al "
                          "historial del vehiculo", carpeta);
            en_viaje = false;
            /* Y la NVS no se toca: si lo que hay ahora abierto es OTRO viaje
             * (se cerro el nuestro y se abrio uno nuevo entre las dos
             * lecturas), borrar NVS_CARPETA se llevaba por delante ese otro. */
            viaje_en_nvs = false;
            snprintf(carpeta, sizeof(carpeta), VEHICULO_DIR);
        }
    }
    if (!en_viaje) mkdir(VEHICULO_DIR, 0777);
    /* MARCA DE AGUA de los dos ficheros ANTES de tocarlos: si algo falla
     * despues (la tarjeta o la NVS), se deshace lo escrito y el reintento del
     * satelite entra limpio, sin duplicar la linea (auditoria del 13-oct-2026:
     * antes el reintento volvia a escribir lo que ya habia entrado, y el
     * analizador suma filas, no ids). */
    char ruta_ev[RUTA_MAX], ruta_tipo[RUTA_MAX];
    snprintf(ruta_ev, sizeof(ruta_ev), "%s/eventos.csv", carpeta);
    snprintf(ruta_tipo, sizeof(ruta_tipo), "%s/%ss.csv", carpeta, tipo);
    off_t marca_ev = marca_de(ruta_ev), marca_tipo = marca_de(ruta_tipo);

    bool escrito = diario_en(carpeta, cuando, tipo, detalle);
    escrito = csv_por_tipo(carpeta, tipo, jd, cuando) && escrito;
    if (!escrito) {
        marca_deshacer(ruta_ev, marca_ev);
        marca_deshacer(ruta_tipo, marca_tipo);
    }
    camera_sd_bus_unlock();

    if (!escrito) {
        /* 507 y no 200: es 5xx, asi que el satelite NO lo descarta y lo vuelve
         * a intentar. Tampoco se cuenta ni se suma a los totales ni se marca el
         * id como aplicado, para que el reintento entre de verdad. */
        anota_fallo_escritura();
        ESP_LOGE(TAG, "NO he podido escribir el apunte '%s' en %s: la tarjeta "
                      "esta llena o falla. Devuelvo 507 para que lo reintente",
                 tipo, carpeta);
        httpd_resp_set_status(req, "507 Insufficient Storage");
        responde_con_hueco(req, "no se ha podido escribir en la tarjeta");
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
            totales_sumar_en(h, tipo, jd);
            uint32_t n = 0;
            nvs_get_u32(h, NVS_T_APLIC, &n);
            nvs_set_u32(h, NVS_T_APLIC, n + 1);
        }
        /* 'viaje_en_nvs' y NO 'en_viaje': el apunte del historial (o de otra
         * pantalla) no puede ni abrir un viaje fantasma en /sdcard/vehiculo ni
         * BORRAR el que la otra cabina tenga abierto. El id se guarda siempre:
         * es la idempotencia. */
        if (viaje_en_nvs) estado_set_en(h, carpeta, id);
        else              id_anota_en(h, id);
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
                 tipo, esp_err_to_name(commit_err));
        if (camera_sd_bus_lock(3000)) {
            marca_deshacer(ruta_ev, marca_ev);
            marca_deshacer(ruta_tipo, marca_tipo);
            camera_sd_bus_unlock();
        }
        anota_fallo_nvs();
        httpd_resp_set_status(req, "507 Insufficient Storage");
        responde_sin_sitio_nvs(req, "no se ha podido guardar el apunte en la NVS");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "apunte '%s' guardado en %s", tipo, carpeta);
    responde_con_hueco(req, "ok");
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
        anota_fallo_escritura();
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
    /* El aviso solo se borra si la fila HA LLEGADO ENTERA: con el fsync fallido
     * y el fclose bien, antes se ponia a cero y el icono quedaba verde con la
     * tarjeta fallando (revision del 13-oct-2026). */
    fflush(f);   /* ver la nota de diario_en(): fsync no vacia el buffer de stdio */
    bool ok = (fsync(fileno(f)) == 0);
    if (!ok) ESP_LOGW(TAG, "fsync fallo en %s (disco lleno?)", ruta);
    if (fclose(f) != 0) {
        ok = false;
        ESP_LOGW(TAG, "%s no se escribio bien (disco lleno?)", ruta);
    }
    if (ok) anota_escritura_ok(); else anota_fallo_escritura();
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
        anota_fallo_escritura();
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
    fflush(f);   /* ver la nota de diario_en(): fsync no vacia el buffer de stdio */
    bool ok = (fsync(fileno(f)) == 0);
    if (!ok) ESP_LOGW(TAG, "fsync fallo en %s (disco lleno?)", ruta);
    if (fclose(f) != 0) {
        ok = false;
        ESP_LOGW(TAG, "%s no se escribio bien (disco lleno?)", ruta);
    }
    if (ok) anota_escritura_ok(); else anota_fallo_escritura();
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
    if (!f) {                              /* se reintenta en el siguiente tick */
        anota_fallo_escritura();
        return false;
    }
    if (nuevo) fprintf(f, "fecha_hora,latitud,longitud,altitud_m,km\n");
    for (int i = 0; i < s_ruta_n; i++) {
        /* Seis decimales son ~11 cm: de sobra, y no engorda el fichero. */
        fprintf(f, "%s,%.6f,%.6f,%.0f,%.2f\n", s_ruta[i].cuando,
                s_ruta[i].lat, s_ruta[i].lon, (double)s_ruta[i].alt, s_ruta[i].km);
    }
    fflush(f);   /* ver la nota de diario_en(): fsync no vacia el buffer de stdio */
    if (fsync(fileno(f)) != 0) {           /* disco lleno */
        anota_fallo_escritura();
        fclose(f);
        return false;
    }
    if (fclose(f) != 0) {                  /* tarjeta llena: NO se tiran los puntos */
        anota_fallo_escritura();
        return false;
    }
    s_ruta_n = 0;
    anota_escritura_ok();
    return true;
}

/* Salto de reloj avisado por el GPS, para que lo anote la tarea del viaje (ver
 * viaje_nota_hora_corregida). */
static _Atomic bool     s_hora_salto_pendiente;
static _Atomic int64_t  s_hora_salto_s;

/* Deja escrito en el viaje que el reloj ha dado un salto (lo llama gps.c). */
void viaje_nota_hora_corregida(int64_t delta_s)
{
    /* SE PIDE, no se escribe aqui: esto lo llama la tarea del GPS, que tiene
     * 4096 de pila y no esta en stack_watch, y el resto de escritores de SD del
     * proyecto usan 6144 o mas. Hacer un fopen+fsync con un cerrojo de hasta 3 s
     * en esa tarea es pedir un desbordamiento de pila (revision del
     * 13-oct-2026). Lo escribe la tarea del viaje en su siguiente vuelta. */
    s_hora_salto_s = delta_s;
    s_hora_salto_pendiente = true;
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
bool viaje_volcar_ahora(const char *motivo)
{
    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) return false;
    if (!camera_sd_bus_lock(3000)) {
        ESP_LOGW(TAG, "viaje abierto y la tarjeta ocupada: no puedo volcar la "
                      "ruta antes de reiniciar");
        return false;
    }
    bool ok = ruta_volcar(carpeta);
    /* La linea del diario SOLO si de verdad se volco: con la tarjeta llena se
     * reintenta cada 30 s y antes dejaba una linea "volcado" por intento. */
    if (ok) diario(carpeta, "volcado", motivo ? motivo : "volcado a peticion");
    else ESP_LOGW(TAG, "quedaban puntos de ruta y no se han podido escribir");
    camera_sd_bus_unlock();
    ESP_LOGW(TAG, "viaje abierto en %s: ruta volcada (%s)", carpeta,
             motivo ? motivo : "a peticion");
    return ok;
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
        anota_fallo_escritura();
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
    } else {
        /* BUFFER LLENO = la tarjeta no ha querido el volcado (llena o fallando).
         * Antes el punto nuevo se TIRABA y el buffer se quedaba con los diez mas
         * VIEJOS: la traza se congelaba justo donde empezo el problema. Ahora se
         * tira el mas viejo: se pierde el principio de los ultimos 5 minutos, no
         * el final, que es lo que hace falta para saber por donde ibas
         * (auditoria del 13-oct-2026, hallazgo 4). */
        memmove(&s_ruta[0], &s_ruta[1], sizeof(s_ruta[0]) * (RUTA_BUFFER_N - 1));
        ruta_punto_t *p = &s_ruta[RUTA_BUFFER_N - 1];
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
    /* EL HUECO DE LA TARJETA SE MIDE AQUI (correccion del 14-oct-2026, lo cazo la
     * revision adversarial): lo puse dentro de escribir_resumen(), que NO es el
     * tick sino el cierre -- y alli el cerrojo de la SD YA esta tomado, asi que
     * sd_hueco_medir() se autobloqueaba 1,5 s y no medía NUNCA, con un retardo de
     * regalo en cada cierre. Aqui no hay cerrojo tomado y esto si es cada 5 min.
     * Va ANTES del "sin viaje abierto" a proposito: la tarjeta se llena tambien
     * con el vehiculo parado (la vigilancia), que es justo cuando no hay viaje. */
    sd_hueco_medir();

    char carpeta[CARPETA_MAX];
    if (!viaje_abierto(carpeta, sizeof(carpeta))) {
        /* Sin viaje abierto no hay donde anotar nada: los avisos pendientes se
         * tiran, que si no se escriben (mintiendo) en el viaje siguiente. */
        s_hora_salto_pendiente = false;
        s_volcado_pedido = false;
        return;
    }

    /* Lo pedido desde fuera va ANTES que el resto del tick (telemetria,
     * contadores y ruta): si la bateria se esta cayendo, no se puede esperar. */
    if (s_hora_salto_pendiente) {
        char txt[128];
        snprintf(txt, sizeof(txt),
                 "el reloj se ha movido %lld s (lo puso el GPS); los apuntes "
                 "anteriores llevan la hora vieja",
                 (long long)s_hora_salto_s);
        /* EL FLAG SE LIMPIA DESPUES DE ESCRIBIR, no antes: si se limpiaba antes
         * y el cerrojo de la SD no llegaba, la nota se perdia (revision del
         * 13-oct-2026). Y si sigue sin haber viaje abierto cuando llegue aqui,
         * se tira: es la nota de un viaje que ya no esta, y guardarla la
         * escribiria en el viaje SIGUIENTE diciendo una mentira. */
        if (camera_sd_bus_lock(3000)) {
            char ahora_carpeta[CARPETA_MAX];
            if (viaje_abierto(ahora_carpeta, sizeof(ahora_carpeta)) &&
                strcmp(ahora_carpeta, carpeta) == 0) {
                /* Solo se da por escrita si diario() dice que SI (devuelve bool
                 * desde el arreglo del 13-oct): con la tarjeta llena se perdia y
                 * el flag se limpiaba igual. */
                if (diario(carpeta, "hora_corregida", txt))
                    s_hora_salto_pendiente = false;
            } else {
                s_hora_salto_pendiente = false;   /* ya no hay viaje donde anotarla */
            }
            camera_sd_bus_unlock();
        }
    }
    if (s_volcado_pedido) {
        /* SE LIMPIA DESPUES, y solo si de verdad se ha volcado: antes se ponia a
         * false antes de intentarlo, y como la peticion es de un solo uso, si el
         * cerrojo de la SD no llegaba se perdia el volcado -- justo cuando la
         * alarma de bateria baja lo pide porque se puede cortar la corriente
         * (revision del 13-oct-2026). */
        if (viaje_volcar_ahora(s_volcado_motivo == 0
                               ? "bateria baja: puede cortarse la corriente"
                               : "volcado pedido desde la pantalla"))
            s_volcado_pedido = false;
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
    /* NIVEL ABIERTO (la clave del PORTAL), no el de administracion, y esto es
     * importante: este endpoint es EL CANAL DE LAS CABINAS (POST /api/viaje:
     * inicio, registro, fin, descartar). Las cabinas solo tienen las
     * credenciales del portal (ver p4_creds.c), asi que con REQUIRE_AUTH_STRICT
     * en cuanto se generaba la clave de administracion se quedaban fuera: 401 en
     * cada apunte, la cola atascada y el viaje sin poder cerrarse -- justo lo que
     * se estaba intentando arreglar. Lo cazo la revision adversarial del
     * 13-oct-2026.
     *
     * Sigue exigiendo credenciales (PORTAL_REQUIRE_BASIC_AUTH esta a 1); lo que
     * NO pide es la clave de administracion, que es para lo que de verdad puede
     * dejar el cacharro inservible: escribir firmware (/ota), leer o cambiar las
     * claves AES de los Victron (/keys, /save) y las capturas de pantalla. */
    REQUIRE_AUTH(req);

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
    /* CIERRE DESDE LA WEB: un 'fin' que trae 'carpeta' y NO trae 'dev'. El
     * 'carpeta' solo lo manda la web (las cabinas mandan id+eventos+destino+dev),
     * asi que esto los identifica sin ambiguedad. Dos cosas:
     *  - NO pasa por el anillo de dedupe (revision adversarial del 14-oct-2026):
     *    con un id fijo, el segundo clic se lo tragaria un "duplicado" 200 y el
     *    usuario no sabria si cerro algo; sin dedupe llega a op_fin y contesta lo
     *    que pasa de verdad.
     *  - PIDE LA CLAVE DE ADMINISTRACION (mismo patron que 'borrar'): esto puede
     *    cerrar el viaje que una cabina esta GRABANDO, y su clave esta publicada
     *    en el historial de dos repos publicos. La cabina no manda 'carpeta', asi
     *    que su GUARDARLO (que si es la salida de emergencia legitima, detras de
     *    un dialogo que avisa) sigue por el nivel abierto. */
    bool es_cierre_web = !strcmp(jop->valuestring, "fin") &&
                         cJSON_IsString(jc) && jc->valuestring[0] &&
                         !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(j, "dev"));
    if (es_cierre_web &&
        check_basic_auth_strict_aviso(req,
            "cerrar desde la web el viaje que quiza esta grabando la cabina necesita "
            "la clave de administracion (pantalla de la P4: Ajustes -> Wi-Fi)") != ESP_OK) {
        cJSON_Delete(j);
        ESP_LOGW(TAG, "cierre desde la web sin credencial de administracion: rechazado");
        return ESP_OK;
    }

    if (!es_inicio && !es_cierre_web && id != 0 && id_ya_aplicado(id)) {
        ESP_LOGI(TAG, "id %lu ya aplicado, lo descarto", (unsigned long)id);
        cJSON_Delete(j);
        httpd_resp_sendstr(req, "duplicado");
        return ESP_OK;
    }

    /* UN EVENTO REZAGADO DEL VIAJE ANTERIOR NO SE APLICA AL QUE HAY ABIERTO
     * (auditoria del 16-oct-2026, ver id_rezagado()): es el "fin" reintentado que
     * cerraba el viaje nuevo, con el resumen y la carpeta del viejo.
     *
     * QUE SE CONTESTA, y por que no lo mismo en los dos casos:
     *  - "fin" y "descartar" (y "borrar"): "duplicado", que es lo que ya contesta
     *    el anillo de dedupe. Su intencion -- cerrar aquel viaje -- ya se cumplio
     *    (por eso existe el reintento) o la superó el viaje nuevo, y devolver un
     *    409 dejaria la cola de la cabina atascada detras de algo que ya no hay
     *    que hacer. El nombre/destino no se pierde: lo lleva el fin.
     *  - "registro": un apunte lleva DATOS del usuario, asi que no se tira en
     *    silencio. Se contesta 409 -- el codigo que la cola ya sabe leer y
     *    enseñar -- y queda a la vista para decidirlo a mano. */
    if (!es_inicio && id != 0 && id_rezagado(id)) {
        if (!strcmp(jop->valuestring, "registro")) {
            ESP_LOGW(TAG, "apunte con id %lu (pantalla %d) ANTERIOR al inicio del "
                          "viaje abierto: es del viaje anterior, no lo aplico",
                     (unsigned long)id, byte_de_id(id));
            httpd_resp_set_status(req, "409 Conflict");
            responde_con_hueco(req, "ese apunte es de un viaje ya cerrado: "
                                    "no lo meto en el que hay abierto");
        } else {
            ESP_LOGW(TAG, "%s con id %lu (pantalla %d) ANTERIOR al inicio del viaje "
                          "abierto: es del viaje anterior, lo descarto",
                     jop->valuestring, (unsigned long)id, byte_de_id(id));
            httpd_resp_sendstr(req, "duplicado");
        }
        cJSON_Delete(j);
        return ESP_OK;
    }

    /* BORRAR UN VIAJE PIDE LA CLAVE DE ADMINISTRACION (auditoria del 14-oct-2026).
     *
     * Es la unica operacion de esta API que DESTRUYE datos de la tarjeta
     * (op_borrar borra la carpeta entera) y estaba en el nivel ABIERTO: bastaba
     * la clave del portal, que en esta unidad esta publicada en el historial de
     * dos repos publicos. Las demas ops siguen abiertas A PROPOSITO: las usan
     * las cabinas, que no tienen ni deben tener la clave de administracion.
     *
     * Si el usuario no ha puesto clave de administracion, el nivel estricto cae
     * al abierto (config_server_auth.c) y esto no le cambia nada: la app sigue
     * borrando como antes. El dia que la ponga, borrar pedira esa clave -- y se
     * le dice en el cuerpo, que la app lo enseña. */
    if (!strcmp(jop->valuestring, "borrar") &&
        check_basic_auth_strict_aviso(req,
            "borrar un viaje necesita la clave de administracion (pantalla de la "
            "P4: Ajustes -> Wi-Fi)") != ESP_OK) {
        /* La respuesta 401 ya la ha mandado auth_comprueba; esto solo añade el
         * motivo, que si no se queda en un 401 pelado. */
        cJSON_Delete(j);
        ESP_LOGW(TAG, "borrar sin credencial de administracion: rechazado");
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

/* Dias que lleva ABIERTO un viaje, sacados del NOMBRE de la carpeta
 * ("AAAA-MM-DD_destino"). Devuelve -1 si el nombre no trae fecha legible.
 *
 * Del nombre y NO de NVS_T_INICIO (arbitraje del 14-oct-2026): esa clave se
 * escribe al abrir y NO se borra al cerrar, asi que para una carpeta que no es
 * el viaje abierto daria la fecha del ULTIMO viaje. El nombre no miente. */
static int dias_abierto_de_nombre(const char *nombre)
{
    if (!nombre || strlen(nombre) < 10) return -1;
    for (int i = 0; i < 10; i++) {
        char c = nombre[i];
        bool digito = (c >= '0' && c <= '9');
        bool guion  = (i == 4 || i == 7) && c == '-';
        if (!digito && !guion) return -1;
    }
    struct tm t = {0};
    t.tm_year = (nombre[0]-'0')*1000 + (nombre[1]-'0')*100 + (nombre[2]-'0')*10 + (nombre[3]-'0') - 1900;
    t.tm_mon  = (nombre[5]-'0')*10 + (nombre[6]-'0') - 1;
    t.tm_mday = (nombre[8]-'0')*10 + (nombre[9]-'0');
    if (t.tm_mon < 0 || t.tm_mon > 11 || t.tm_mday < 1 || t.tm_mday > 31) return -1;
    time_t ahora = time(NULL);
    if (ahora < 1000000000L) return -1;         /* sin reloj no se afirma nada */
    struct tm hoy;
    localtime_r(&ahora, &hoy);
    long dias = dias_locales_de(&hoy) - dias_locales_de(&t) + 1;   /* el dia 1 es el de salida */
    if (dias < 1 || dias > 3650) return -1;
    return (int)dias;
}

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

    /* CERRAR EL VIAJE ABIERTO DESDE AQUI: /data/viajes?cerrar=<carpeta>
     *
     * Es una NAVEGACION normal (un enlace), no un fetch, y a proposito: el
     * cierre pide la clave de administracion, y solo una navegacion de nivel
     * superior hace que el navegador la pida (401 + realm admin). Con un fetch,
     * un 403 no abre ningun dialogo y el boton quedaria muerto justo para quien
     * tiene clave puesta.
     *
     * Se reutiliza op_fin -- el MISMO cierre que la pantalla cuando el viaje no
     * es suyo ("GUARDARLO"): escribe el resumen y marca el viaje SIN VERIFICAR,
     * sin inventarse que este entero. Y se le pasa 'carpeta', que op_fin valida
     * contra lo que esta abierto AHORA: si la lista es vieja, no cierra nada. */
    {
        char q[192] = {0};
        char cerrar[128] = {0};
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK &&
            httpd_query_key_value(q, "cerrar", cerrar, sizeof(cerrar)) == ESP_OK &&
            cerrar[0]) {
            if (check_basic_auth_strict(req) != ESP_OK) return ESP_OK;   /* ya contestado */
            char cuerpo[192];
            snprintf(cuerpo, sizeof(cuerpo),
                     "{\"op\":\"fin\",\"forzar\":true,\"carpeta\":\"%s\","
                     "\"id\":%lu}", cerrar, (unsigned long)ID_CIERRE_WEB);
            cJSON *j = cJSON_Parse(cuerpo);
            if (!j) {
                httpd_resp_set_status(req, "500 Internal Server Error");
                httpd_resp_sendstr(req, "no he podido montar el cierre");
                return ESP_OK;
            }
            ESP_LOGW(TAG, "cierre del viaje abierto pedido desde la web: %s", cerrar);
            /* 302 ANTES de llamar a op_fin: en el camino bueno, op_fin NO toca el
             * estado de la respuesta (solo manda el nombre de la carpeta), asi
             * que este 302 se mantiene y el navegador vuelve a la lista. En los
             * caminos de error op_fin SI pone su estado (409, 507...) y entonces
             * el usuario ve el motivo en vez de una redireccion. Asi el enlace
             * funciona como una accion de la web y no como una pagina de texto. */
            httpd_resp_set_status(req, "302 Found");
            httpd_resp_set_hdr(req, "Location", "/data/viajes?cerrado=si");
            esp_err_t r = op_fin(req, j, ID_CIERRE_WEB);
            cJSON_Delete(j);
            return r;
        }
    }

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
        "<h1>Viajes guardados</h1>");
    /* Resultado de la papelera: se vuelve aqui con ?borrado=si o ?error=no. */
    {
        char q[64] = {0};
        if (httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK) {
            char v[16] = {0};
            if (httpd_query_key_value(q, "borrado", v, sizeof(v)) == ESP_OK &&
                !strcmp(v, "si"))
                httpd_resp_sendstr_chunk(req,
                    "<p style='color:#66BB6A'>Viaje borrado de la tarjeta.</p>");
            else if (httpd_query_key_value(q, "error", v, sizeof(v)) == ESP_OK &&
                     !strcmp(v, "no"))
                httpd_resp_sendstr_chunk(req,
                    "<p style='color:#FFA726'>No se ha podido borrar (el viaje en "
                    "curso no se puede: cierralo antes).</p>");
            else if (httpd_query_key_value(q, "cerrado", v, sizeof(v)) == ESP_OK &&
                     !strcmp(v, "si"))
                httpd_resp_sendstr_chunk(req,
                    "<p style='color:#66BB6A'>Viaje cerrado y marcado SIN VERIFICAR: "
                    "ya se puede descargar (con el aviso dentro) y la P4 vuelve a "
                    "admitir un viaje nuevo.</p>");
        }
    }
    httpd_resp_sendstr_chunk(req, "<ul>");

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
    /* Cual es el viaje ABIERTO, por su nombre. Hace falta para NO ofrecer el
     * cierre en una fila que solo PARECE en curso: estado_de() devuelve V_EN_CURSO
     * tanto para el viaje abierto como para una carpeta sin resumen ni marca (esa
     * la marca marcar_huerfanas al arrancar, pero puede aparecer en caliente). El
     * boton manda cerrar EL VIAJE ABIERTO, asi que en cualquier otra fila
     * cerraria un viaje distinto -- justo el fallo que no se puede permitir. */
    char abierto_ahora[CARPETA_MAX] = {0};
    const char *abierto_base = NULL;
    if (viaje_abierto(abierto_ahora, sizeof(abierto_ahora))) {
        abierto_base = strrchr(abierto_ahora, '/');
        abierto_base = abierto_base ? abierto_base + 1 : abierto_ahora;
    }
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
                /* El viaje ABIERTO no se puede borrar (op_borrar lo rechaza con
                 * 409) y por eso tampoco se ofrece el boton: una accion que
                 * siempre falla es peor que no tenerla. */
                /* CERRARLO DESDE AQUI (arbitraje del 14-oct-2026). Un viaje que
                 * se queda abierto -- se rompio la pantalla, se olvido -- tiene
                 * los datos ATRAPADOS: no se puede bajar (el .tar exige
                 * resumen.txt) y BLOQUEA empezar otro (409/412). El boton manda
                 * el MISMO cierre que la pantalla cuando no es su viaje
                 * ("GUARDARLO"): op_fin con forzar y SIN contador de eventos, que
                 * escribe el resumen y marca el viaje como SIN VERIFICAR -- no
                 * se inventa que este entero. */
                bool es_el_abierto = abierto_base && !strcmp(ent->d_name, abierto_base);
                char dias_txt[64] = {0};
                int dias = dias_abierto_de_nombre(ent->d_name);
                if (es_el_abierto && dias >= 2)
                    snprintf(dias_txt, sizeof(dias_txt),
                             "<div class=e>Lleva %d dias abierto.</div>", dias);
                char dueno_txt[128] = {0};
                if (es_el_abierto) {
                    char dv[24];
                    dev_abierto(dv, sizeof(dv));
                    if (dv[0] && strcmp(dv, "desconocido") != 0)
                        snprintf(dueno_txt, sizeof(dueno_txt),
                                 "<div class=e>Lo empezo la pantalla %s: si esa pantalla "
                                 "esta grabando, cierralo DESDE ELLA.</div>", dv);
                }
                if (es_el_abierto)
                    /* El nombre va en un atributo data- y NO dentro del onclick: el
                     * escape de HTML no sirve para un literal de JavaScript (una
                     * comilla simple en el nombre rompia el boton, y con el texto
                     * adecuado era codigo ejecutable). El atributo se escapa para
                     * HTML y el navegador lo decodifica al leerlo. */
                    snprintf(linea, sizeof(linea),
                        "<li class=cur><b>%s</b>%s%s<div class=e>En curso. Se puede "
                        "cerrar desde la pantalla de la cabina; si no la tienes a mano, "
                        "cierralo aqui y quedara marcado SIN VERIFICAR.</div>"
                        "<a href=\"?cerrar=%s\" onclick=\"return confirm('Cerrar el "
                        "viaje SIN VERIFICAR?\\n\\nSe le escribe el resumen y queda "
                        "marcado como NO comprobado: puede que le falten apuntes.')\">"
                        "Cerrarlo SIN VERIFICAR</a>"
                        "</li>", nombre_esc, dias_txt, dueno_txt, nombre_esc);
                else
                    snprintf(linea, sizeof(linea),
                        "<li class=cur><b>%s</b><div class=e>Sin cerrar y sin resumen: "
                        "no es el viaje abierto (se marca al arrancar la P4). Si crees "
                        "que es el viaje en curso, recarga la lista.</div></li>",
                        nombre_esc);
            }
            httpd_resp_sendstr_chunk(req, linea);

            /* PAPELERA (auditoria del 14-oct-2026). Borrar un viaje paso a pedir
             * la clave de administracion -- es lo unico de /api/viaje que
             * destruye datos --, y la app manda la del portal: esta es la salida
             * que deja la funcion usable. POST (no un GET que borra) y con
             * confirmacion; el nombre va escapado para el atributo. */
            if (e == V_LISTO || e == V_INCOMPLETO || e == V_DESCARTADO) {
                snprintf(linea, sizeof(linea),
                    "<form method=post action=/data/viaje_borrar style='display:inline;margin-left:10px'"
                    " onsubmit=\"return confirm('Borrar este viaje de la tarjeta?\n"
                    "No se puede deshacer.')\">"
                    "<input type=hidden name=carpeta value=\"%s\">"
                    "<button style='background:#7f1d1d;color:#eee;border:0;border-radius:4px;"
                    "padding:4px 10px;cursor:pointer'>Borrar</button></form>",
                    nombre_esc);
                httpd_resp_sendstr_chunk(req, linea);
            }
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
    httpd_resp_sendstr_chunk(req, "</ul>");

    /* Y EL HISTORIAL DEL VEHICULO, aparte y visible (auditoria del 14-oct-2026).
     *
     * Ahi van los apuntes que no son de un viaje (repostar de camino al taller,
     * la ITV, una bombona) y -- desde el arreglo del dueño del viaje -- los que
     * manda OTRA pantalla mientras la P4 tiene abierto un viaje que no es suyo.
     * Sin esto, eso era una fosa comun: se guardaba (que es lo correcto, mejor
     * que perderlo o mezclarlo) pero no salia en NINGUNA lista y solo se veia
     * pinchando la tarjeta en un PC. */
    {
        char ruta_ev[RUTA_MAX];
        struct stat stv;
        snprintf(ruta_ev, sizeof(ruta_ev), VEHICULO_DIR "/eventos.csv");
        bool hay = false;
        if (camera_sd_bus_lock(1000)) {
            hay = (stat(ruta_ev, &stv) == 0);
            camera_sd_bus_unlock();
        }
        if (hay) {
            httpd_resp_sendstr_chunk(req,
                "<h1>Historial del vehiculo</h1><ul>"
                "<li class=ok><b>vehiculo</b><div class=e>Apuntes que no son de un "
                "viaje (repostar, la ITV, una bombona) y los de otra pantalla cuando "
                "el viaje abierto no era suyo.</div>"
                "<a href='" PORTAL_HEAVY_BASE "/data/viaje.tar?v=vehiculo&hist=1'>Descargar</a></li>"
                "</ul>");
        }
    }

    httpd_resp_sendstr_chunk(req,
        "<p class=e>El paquete de siempre (bateria, solar y frigo de TODOS "
        "los dias) esta en <a href='" PORTAL_HEAVY_BASE "/data/historico.tar'>historico.tar</a>.</p>"
        "<p><a href='/data'>Volver</a></p>");
    httpd_resp_sendstr_chunk(req, NULL);
    return ESP_OK;
}
