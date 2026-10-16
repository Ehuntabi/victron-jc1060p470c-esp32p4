#include "trip_computer.h"

#include <string.h>
#include <stdint.h>
#include <math.h>
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "TRIP";
static const char *NVS_NS = "trip";

static trip_computer_t s;
static time_t s_last_sample;
static time_t s_last_solar_sample;   /* stream propio de la solar (MPPT) */
static SemaphoreHandle_t s_mtx;
/* Mutex de la PERSISTENCIA, aparte del de los datos (auditoria del 16-oct-2026).
 *
 * El snapshot se tomaba bajo s_mtx pero write_nvs() se llamaba DESPUES de
 * soltarlo, y a write_nvs() se llega desde tres sitios que corren en tareas
 * distintas (el reset del inicio de viaje desde el httpd, el guardado de cada
 * 5 min desde el hook del BMV -- que corre en la tarea de LVGL --, el flush de
 * Ajustes y el end() del fin de viaje, tambien desde el httpd). Dos de esos
 * write_nvs() a la vez es una apertura de NVS y un commit concurrentes sobre
 * las mismas claves, y el que llegue ultimo pisa al otro con un snapshot VIEJO:
 * tras un reinicio los km/Wh del viaje salen de la copia que no tocaba. Con
 * este mutex, snapshot + write_nvs van juntos y en orden, y el ultimo en
 * escribir es siempre el que tiene el estado mas nuevo. */
static SemaphoreHandle_t s_nvs_mtx;

/* Snapshot para persistir fuera del lock (el commit/GC del NVS no debe retener
 * s_mtx: trip_computer_get quedaria bloqueado desde LVGL durante el commit). */
typedef struct {
    int64_t reset_epoch, seconds_running, seconds_driving;
    int32_t whc, whd, ahc_m, ahd_m;
    int32_t whs, ahs_m;   /* aporte solar: Wh y mAh */
    int64_t sol_secs;     /* tiempo con la placa cargando */
    int32_t km_m;         /* distancia recorrida, en METROS (10.000 km caben de sobra) */
    uint8_t active;       /* 1 = viaje en curso */
} trip_snap_t;

/* ── Odometro por GPS ──────────────────────────────────────────────
 * Umbrales elegidos para el muestreo de 5 s de main.c. */
#define GPS_MIN_SATS       5        /* con menos, la posicion da bandazos */
#define GPS_SALTO_MIN_M    15.0     /* parado, el receptor deriva unos metros */
#define GPS_SALTO_MAX_M    1000.0   /* en 5 s son 720 km/h: eso no es conducir */
#define GPS_HUECO_MAX_S    30       /* mas silencio que esto = re-anclar, no unir */

static double  s_gps_lat, s_gps_lon;
static time_t  s_gps_t;
static bool    s_gps_ancla;   /* hay un punto anterior con el que comparar */

/* ── Tiempo CONDUCIENDO (16-oct-2026) ──────────────────────────────
 * Por debajo de esta velocidad se considera PARADO. No se pone en 0 porque el
 * receptor, quieto, no da 0 exacto: baila entre 0 y 2 km/h, y contarlo seria
 * sumar horas de volante sentado en el camping. Y no se pone mas alto (10, 15)
 * porque un atasco o una pista de tierra se hacen a 6-10 km/h y eso SI es
 * conducir.
 *
 * Se cuenta por INTERVALOS DE LA LLAMADA DEL GPS (cada 5 s, ver main.c), no por
 * tramos de distancia: asi tambien suma el atasco, donde en 5 s no se llega a
 * los 15 m que exige el odometro y sin embargo se esta conduciendo. */
#define VEL_CONDUCIENDO_KMH   4.0

static time_t  s_ultima_vel_t;   /* cuando se miro la velocidad (0 = sin dato) */

/* Copia el estado a un snapshot. El caller debe tener s_mtx tomado. */
static trip_snap_t trip_snapshot_locked(void)
{
    trip_snap_t snap = {
        .reset_epoch     = s.reset_epoch,
        .seconds_running = s.seconds_running,
        .seconds_driving = s.seconds_driving,
        .whc   = (int32_t)s.wh_charged,
        .whd   = (int32_t)s.wh_discharged,
        .ahc_m = (int32_t)(s.ah_charged    * 1000.0),  /* en mAh: preserva decimales */
        .ahd_m = (int32_t)(s.ah_discharged * 1000.0),
        .whs   = (int32_t)s.wh_solar,
        .ahs_m = (int32_t)(s.ah_solar      * 1000.0),
        .sol_secs = s.solar_seconds,
        .km_m     = (int32_t)(s.km * 1000.0),
        .active   = s.active ? 1 : 0,
    };
    return snap;
}

/* Persiste el snapshot. DEVUELVE el resultado (16-oct-2026).
 *
 * Aqui pasaba lo mismo que en el resto del proyecto: se miraba SOLO el
 * nvs_commit, y en IDF 5.5 nvs_commit() es un no-op (la entrada se escribe en
 * la pagina al hacer el set; ver nvs_api.cpp). Con la NVS llena -- y la de esta
 * placa son 16 KB, que es poco -- todos los nvs_set_* fallaban, el commit decia
 * ESP_OK y el aviso "trip computer no persistio" era INALCANZABLE: los
 * contadores se quedaban sin guardar y nadie se enteraba hasta que el resumen
 * del viaje salia con la energia de hacia tres viajes. Ahora se acumula el
 * retorno de cada set (mismo criterio que config_server_viaje.c). */
static esp_err_t write_nvs(const trip_snap_t *snap)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    err = nvs_set_i64(h, "reset", snap->reset_epoch);
    #define APUNTA(e) do { esp_err_t _e = (e); if (err == ESP_OK) err = _e; } while (0)
    APUNTA(nvs_set_i64(h, "secs",  snap->seconds_running));
    APUNTA(nvs_set_i64(h, "drv_s", snap->seconds_driving));
    APUNTA(nvs_set_i32(h, "wh_c", snap->whc));
    APUNTA(nvs_set_i32(h, "wh_d", snap->whd));
    APUNTA(nvs_set_i32(h, "ah_c", snap->ahc_m));
    APUNTA(nvs_set_i32(h, "ah_d", snap->ahd_m));
    APUNTA(nvs_set_i32(h, "wh_s", snap->whs));
    APUNTA(nvs_set_i32(h, "ah_s", snap->ahs_m));
    APUNTA(nvs_set_i64(h, "sol_t", snap->sol_secs));
    APUNTA(nvs_set_i32(h, "km_m", snap->km_m));
    APUNTA(nvs_set_u8(h, "active", snap->active));
    APUNTA(nvs_commit(h));
    #undef APUNTA
    nvs_close(h);
    if (err != ESP_OK)
        ESP_LOGW(TAG, "trip computer no persistio (%s): NVS llena o gastada; los "
                      "contadores que se vean tras un reinicio seran los de antes",
                 esp_err_to_name(err));
    return err;
}

static void load_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    int64_t r = 0, secs = 0, drv_s = 0;
    int32_t whc = 0, whd = 0, ahc_m = 0, ahd_m = 0, whs = 0, ahs_m = 0;
    int64_t sol_s = 0;
    nvs_get_i64(h, "reset", &r);
    nvs_get_i64(h, "secs",  &secs);
    /* Clave nueva (16-oct-2026): en una placa que venga de antes no existe y
     * nvs_get la deja en 0, que es justo lo correcto -- las horas de
     * conduccion se empiezan a contar en el primer viaje con este firmware. */
    nvs_get_i64(h, "drv_s", &drv_s);
    nvs_get_i32(h, "wh_c", &whc);
    nvs_get_i32(h, "wh_d", &whd);
    nvs_get_i32(h, "ah_c", &ahc_m);
    nvs_get_i32(h, "ah_d", &ahd_m);
    nvs_get_i32(h, "wh_s", &whs);
    nvs_get_i32(h, "ah_s", &ahs_m);
    nvs_get_i64(h, "sol_t", &sol_s);
    /* Clave nueva (24-ago-2026, con el GPS ya montado): en una placa que venga
     * de antes no existe y nvs_get la deja en 0, que es justo lo correcto. */
    int32_t km_m = 0;
    nvs_get_i32(h, "km_m", &km_m);
    /* Migracion: en una placa que venia de antes de existir el flag no hay clave
     * "active". Se asume viaje EN CURSO, que es el caso real de cualquiera que ya
     * estuviera usando el trip computer: asi el aviso de arranque deja de salir
     * desde la primera actualizacion, sin pedir nada. Quien lo tuviera terminado
     * empieza el nuevo desde Ajustes -> Empezar viaje. */
    uint8_t act = 1;
    nvs_get_u8(h, "active", &act);
    nvs_close(h);
    s.active = (act != 0);
    s.reset_epoch     = r;
    s.seconds_running = secs;
    s.seconds_driving = drv_s;
    s.wh_charged      = (double)whc;
    s.wh_discharged   = (double)whd;
    s.ah_charged      = (double)ahc_m / 1000.0;
    s.ah_discharged   = (double)ahd_m / 1000.0;
    s.wh_solar        = (double)whs;
    s.ah_solar        = (double)ahs_m / 1000.0;
    s.solar_seconds   = sol_s;
    s.km              = (double)km_m / 1000.0;
}

/* Guarda el estado AHORA: toma s_mtx para el snapshot y s_nvs_mtx para que el
 * par snapshot+escritura sea atomico frente a otro guardado (ver el comentario
 * de s_nvs_mtx). Devuelve false si no se pudo (mutex inexistente o sin tomar),
 * que solo pasa en la ventana de arranque. */
static bool trip_guardar_ya(void)
{
    if (!s_mtx || !s_nvs_mtx) return false;
    xSemaphoreTake(s_nvs_mtx, portMAX_DELAY);   /* SIEMPRE antes que s_mtx: orden fijo */
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    const trip_snap_t snap = trip_snapshot_locked();
    xSemaphoreGive(s_mtx);
    write_nvs(&snap);
    xSemaphoreGive(s_nvs_mtx);
    return true;
}

/* Metros entre dos posiciones. Equirectangular en vez de haversine: en tramos
 * de segundos el error es despreciable y se ahorra la trigonometria pesada. */static double metros_entre(double lat1, double lon1, double lat2, double lon2)
{
    const double GRADO_M = 111320.0;
    double dlat = (lat2 - lat1) * GRADO_M;
    double dlon = (lon2 - lon1) * GRADO_M * cos(lat1 * M_PI / 180.0);
    return sqrt(dlat * dlat + dlon * dlon);
}

void trip_computer_on_gps(bool fix, uint8_t satelites, double lat, double lon,
                          double vel_kmh)
{
    /* Si aun no ha corrido trip_computer_init() (desde main.c, al arrancar),
     * saltarse esta actualizacion en vez de reintentar el init aqui: dos
     * tareas llamando a la vez re-ejecutarian trip_computer_init() (NVS +
     * escritura) en paralelo sin proteccion -- una carrera de verdad, no solo
     * de crear el mutex. Mismo patron que ya usan trip_computer_flush() y
     * trip_computer_end() unas lineas mas abajo. Detectado auditando el
     * 07-sep-2026. */
    if (!s_mtx) return;
    time_t now = time(NULL);

    /* TODO LO QUE TOCA EL ESTADO, BAJO EL MUTEX (auditoria del 16-oct-2026).
     * El corte por falta de fix escribia s_gps_ancla y s_ultima_vel_t ANTES de
     * tomarlo, mientras trip_computer_get() y trip_computer_flush() los leen con
     * el mutex tomado: una carrera de verdad sobre estado compartido, aunque en
     * un ESP32 las escrituras de 32 bits no se rompan. Ahora se toma primero y el
     * corte sale por donde entro. */
    xSemaphoreTake(s_mtx, portMAX_DELAY);

    /* Sin posicion de fiar no hay ancla: al recuperarla se empieza de nuevo en
     * vez de unir por la recta los dos extremos del tunel. */
    if (!fix || satelites < GPS_MIN_SATS) {
        s_gps_ancla = false;
        /* Y la marca de velocidad, a cero: si no, al volver el fix se contaria
         * como tiempo conduciendo TODO el hueco sin posicion (un tunel largo, o
         * el rato con la antena tapada), que es justo lo que no se sabe. */
        s_ultima_vel_t = 0;
        xSemaphoreGive(s_mtx);
        return;
    }

    /* Tiempo conduciendo: el intervalo desde la ultima muestra, contado solo si
     * la velocidad de AHORA pasa del umbral. Va ANTES del filtro de distancia a
     * proposito (ver VEL_CONDUCIENDO_KMH): el atasco no llega a los 15 m por
     * muestra y aun asi se conduce. */
    if (s_ultima_vel_t != 0) {
        long dt = (long)(now - s_ultima_vel_t);
        /* El tope de hueco es el mismo que el del ancla: con muestras cada 5 s,
         * un salto de mas de 30 s significa que esto estuvo parado o sin reloj,
         * y ese rato no se le apunta a nadie. */
        if (dt > 0 && dt <= GPS_HUECO_MAX_S && vel_kmh >= VEL_CONDUCIENDO_KMH) {
            s.seconds_driving += (int64_t)dt;
        }
    }
    s_ultima_vel_t = now;

    if (s_gps_ancla && (now - s_gps_t) <= GPS_HUECO_MAX_S) {
        double d = metros_entre(s_gps_lat, s_gps_lon, lat, lon);
        if (d >= GPS_SALTO_MIN_M && d <= GPS_SALTO_MAX_M) {
            s.km += d / 1000.0;
        } else if (d > GPS_SALTO_MAX_M) {
            ESP_LOGW(TAG, "salto de %.0f m descartado (no se cuenta la recta)", d);
        }
        /* Por debajo del umbral no se suma NI se mueve el ancla: si no, estando
         * parado el ruido iria arrastrando el punto de referencia y el viaje
         * crecería metro a metro sin que el vehiculo se moviera. */
        if (d < GPS_SALTO_MIN_M) { xSemaphoreGive(s_mtx); return; }
    }
    s_gps_lat = lat; s_gps_lon = lon; s_gps_t = now; s_gps_ancla = true;
    xSemaphoreGive(s_mtx);
}

void trip_computer_init(void)
{
    if (s_mtx == NULL) s_mtx = xSemaphoreCreateMutex();
    if (s_nvs_mtx == NULL) s_nvs_mtx = xSemaphoreCreateMutex();
    load_nvs();
    if (s.reset_epoch == 0) {
        time_t now = time(NULL);
        if (now >= 1000000000L) {
            s.reset_epoch = now;
            trip_snap_t snap = trip_snapshot_locked();
            write_nvs(&snap);
        }
    }
    ESP_LOGI(TAG, "Trip: reset=%lld, Wh +%.1f -%.1f, Ah +%.2f -%.2f, secs=%lld, "
                  "conduciendo=%.1f h",
             (long long)s.reset_epoch, s.wh_charged, s.wh_discharged,
             s.ah_charged, s.ah_discharged, (long long)s.seconds_running,
             (double)s.seconds_driving / 3600.0);
}

void trip_computer_on_battery(int32_t i_milli, uint16_t v_centi)
{
    if (!s_mtx) return;   /* ver el comentario en trip_computer_on_gps() */
    bool do_save = false;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    time_t now = time(NULL);
    if (now < 1000000000L) {
        xSemaphoreGive(s_mtx);
        return;
    }
    /* SIN VIAJE ABIERTO NO SE INTEGRA NADA (auditoria del 16-oct-2026). El
     * comentario de trip_computer_end() decia que dejaba los contadores a cero y
     * no lo hacia: seguian sumando km/horas/Wh con el viaje ya cerrado, y esos
     * numeros son los que /api/state publica como "energia del viaje" y los que
     * se llevaba el resumen del viaje SIGUIENTE si este se empezaba por un
     * camino que no llama a reset(). Mirar 'active' aqui es lo que hace que el
     * flag sirva para algo. Se siguen actualizando las marcas de tiempo (mas
     * abajo) para que al empezar el viaje nuevo el primer intervalo no cuente el
     * rato que estuvo cerrado. */
    if (!s.active) {
        s_last_sample = now;
        xSemaphoreGive(s_mtx);
        return;
    }
    if (s.reset_epoch == 0) {
        s.reset_epoch = now;
    }
    if (s_last_sample != 0) {
        double dt_s = (double)(now - s_last_sample);
        if (dt_s > 0 && dt_s < 600) {  /* descartar huecos */
            double power_w = (double)v_centi * (double)i_milli / 100000.0;
            double energy_wh = power_w * dt_s / 3600.0;
            double current_a = (double)i_milli / 1000.0;
            double charge_ah = current_a * dt_s / 3600.0;
            if (energy_wh > 0) {
                s.wh_charged += energy_wh;
                s.ah_charged += charge_ah;
            } else {
                s.wh_discharged += -energy_wh;
                s.ah_discharged += -charge_ah;
            }
            s.seconds_running += (int64_t)dt_s;
            /* Persistencia cada 5 min para no degradar la flash (NVS) */
            static time_t s_last_save = 0;
            if (now - s_last_save >= 300) {
                do_save = true;
                s_last_save = now;
            }
        }
    }
    s_last_sample = now;
    xSemaphoreGive(s_mtx);

    /* El guardado va por trip_guardar_ya(), que vuelve a tomar s_mtx (liberado
     * arriba) y ademas s_nvs_mtx: el snapshot que se escribe es el de ESTE
     * momento, no el de un reset que se haya colado entre medias. Y el commit a
     * flash sigue haciendose FUERA de s_mtx para no bloquear a
     * trip_computer_get (LVGL/dashboard) durante el commit/GC. */
    if (do_save) trip_guardar_ya();
}

void trip_computer_on_solar(int32_t i_milli, uint16_t v_centi)
{
    if (!s_mtx) return;   /* ver el comentario en trip_computer_on_gps() */
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    time_t now = time(NULL);
    if (now < 1000000000L) {
        xSemaphoreGive(s_mtx);
        return;
    }
    /* Misma guarda que en on_battery(): sin viaje abierto no se acumula. */
    if (!s.active) {
        s_last_solar_sample = now;
        xSemaphoreGive(s_mtx);
        return;
    }
    /* Solo integra aporte real (la MPPT solo carga: corriente >= 0). El guardado
     * a NVS lo hace el hook del BMV con su cadencia de 5 min (el snapshot ya
     * incluye el solar), asi no duplicamos escrituras de flash. */
    if (s_last_solar_sample != 0 && i_milli > 0) {
        double dt_s = (double)(now - s_last_solar_sample);
        if (dt_s > 0 && dt_s < 600) {  /* descartar huecos */
            double power_w = (double)v_centi * (double)i_milli / 100000.0;
            double current_a = (double)i_milli / 1000.0;
            s.wh_solar += power_w   * dt_s / 3600.0;
            s.ah_solar += current_a * dt_s / 3600.0;
            s.solar_seconds += (int64_t)dt_s;   /* tiempo con la placa cargando */
        }
    }
    s_last_solar_sample = now;
    xSemaphoreGive(s_mtx);
}

void trip_computer_flush(void)
{
    /* Fuerza el guardado en NVS ahora mismo. Normalmente se hace cada 5 min;
     * al finalizar un viaje hay que asegurarlo antes de apagar o sacar la SD. */
    trip_guardar_ya();
}

/* __attribute__((unused)): esta declarada en el .h y la llama config_server_viaje.c
 * (otro componente), asi que el compilador de ESTE fichero no la ve usada y
 * avisa; el aviso taparia los de verdad. */
__attribute__((unused)) void trip_computer_reset(void)
{
    if (!s_mtx) return;   /* ver el comentario en trip_computer_on_gps() */
    if (s_nvs_mtx) xSemaphoreTake(s_nvs_mtx, portMAX_DELAY);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    time_t now = time(NULL);
    s.reset_epoch = (now >= 1000000000L) ? now : 0;
    s.wh_charged = 0;
    s.wh_discharged = 0;
    s.ah_charged = 0;
    s.ah_discharged = 0;
    s.wh_solar = 0;
    s.ah_solar = 0;
    s.solar_seconds = 0;
    s.seconds_running = 0;
    s.seconds_driving = 0;
    s.km = 0;
    s_ultima_vel_t = 0;   /* el primer intervalo del viaje nuevo no se cuenta */
    s_last_sample = 0;
    s_last_solar_sample = 0;
    s_gps_ancla = false;   /* el primer punto del viaje nuevo no arrastra el anterior */
    /* 'active' NO SE TOCA (16-oct-2026). Antes esto ponia active = true, y como
     * esta funcion la llama TAMBIEN el boton "Poner a cero" de Ajustes
     * (trip_manager.c), pulsarlo con la P4 sin viaje abierto dejaba un VIAJE
     * FANTASMA: udp_tx publicaba "en curso" y la cabina enseñaba km 0,0 de un
     * viaje que no existe, y sobrevivia a los reinicios (va a NVS).
     *
     * El viaje lo declara el INICIO DE VIAJE, y es el quien tiene que marcar el
     * trip computer como activo: ver trip_computer_iniciar(), que llama aqui y
     * ademas pone active = true. Poner a cero los contadores es solo eso. */
    trip_snap_t snap = trip_snapshot_locked();
    write_nvs(&snap);           /* con s_nvs_mtx tomado: nadie escribe a la vez */
    xSemaphoreGive(s_mtx);
    if (s_nvs_mtx) xSemaphoreGive(s_nvs_mtx);
    ESP_LOGI(TAG, "Contadores del viaje a cero (el viaje sigue %s)",
             s.active ? "en curso" : "cerrado");
}

/* INICIO DE VIAJE: contadores a cero Y el viaje marcado como abierto.
 *
 * Es lo que llama op_inicio() (/api/viaje, la cabina declara el viaje). Antes
 * bastaba trip_computer_reset() porque era el quien ponia active = true; desde
 * el 16-oct-2026 la marca es cosa de esta funcion, para que el boton de Ajustes
 * (que solo quiere poner los numeros a cero) no invente un viaje. */
void trip_computer_iniciar(void)
{
    if (!s_mtx) return;
    trip_computer_reset();
    if (s_nvs_mtx) xSemaphoreTake(s_nvs_mtx, portMAX_DELAY);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s.active = true;
    trip_snap_t snap = trip_snapshot_locked();
    write_nvs(&snap);
    xSemaphoreGive(s_mtx);
    if (s_nvs_mtx) xSemaphoreGive(s_nvs_mtx);
    ESP_LOGI(TAG, "Viaje en curso (contadores a cero)");
}

/* TERMINAR EL VIAJE: ademas de apagar el flag, DEJA LOS CONTADORES A CERO
 * (auditoria del 16-oct-2026).
 *
 * El comentario de aqui arriba decia "sin tocar los contadores" y el del .h
 * "marca el viaje como TERMINADO y guarda", pero lo unico que pasaba era el
 * flag: los km, las horas y los Wh seguian sumando despues de cerrar (nadie
 * miraba 'active') y /api/state los publicaba como si el viaje siguiera -- el
 * dueño lo vio en la web. Peor: el motor de arranque de la 5" usa estos
 * contadores, asi que un viaje empezado por un camino que no pase por
 * trip_computer_reset() (el boton de Ajustes, un reinicio a mitad de viaje)
 * arrancaba con los numeros del viaje ANTERIOR ya dentro.
 *
 * Se ponen a cero AQUI, que es el momento en que el resumen ya se ha escrito
 * (op_fin llama a esto despues de escribir_resumen) y por tanto ya no hay nada
 * que los lea. 'reset_epoch' se deja como estaba: es "cuando empezo lo que se
 * esta midiendo", y un 0 ahi haria que el primer sample del viaje siguiente
 * creyera que no hay epoch. */
void trip_computer_end(void)
{
    if (!s_mtx) return;
    if (s_nvs_mtx) xSemaphoreTake(s_nvs_mtx, portMAX_DELAY);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    s.active = false;
    s.wh_charged = 0;
    s.wh_discharged = 0;
    s.ah_charged = 0;
    s.ah_discharged = 0;
    s.wh_solar = 0;
    s.ah_solar = 0;
    s.solar_seconds = 0;
    s.seconds_running = 0;
    s.seconds_driving = 0;
    s.km = 0;
    /* Y las anclas de los integradores, para que el primer tramo del viaje
     * siguiente no cuente el rato que estuvo cerrado. */
    s_ultima_vel_t = 0;
    s_last_sample = 0;
    s_last_solar_sample = 0;
    s_gps_ancla = false;
    trip_snap_t snap = trip_snapshot_locked();
    write_nvs(&snap);
    xSemaphoreGive(s_mtx);
    if (s_nvs_mtx) xSemaphoreGive(s_nvs_mtx);
    ESP_LOGI(TAG, "Viaje terminado: contadores a cero (no siguen sumando)");
}

void trip_computer_get(trip_computer_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));  /* nunca dejar *out sin init (ventana de arranque) */
    if (!s_mtx) return;
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    *out = s;
    xSemaphoreGive(s_mtx);
}
