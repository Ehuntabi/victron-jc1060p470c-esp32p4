/* udp_tx.c - Publisher 1 Hz hacia el mini, vía UDP broadcast sobre el AP.
 *
 * Plan B tras descubrir que esp_hosted (P4 + slave C6 SDIO) no exporta la API
 * esp_now_*. El SoftAP del 7" sí funciona via esp_hosted, así que enviamos
 * UDP broadcast a 192.168.4.255:MINI_PROTO_UDP_PORT y el mini, asociado al
 * mismo AP como cliente STA, recibe los paquetes.
 *
 * Lee de dashboard_state (battery/dcdc), frigo (componente local) y ne185
 * (RS-485 NE185), arma mini_msg_t y lo emite.
 */
#include "udp_tx.h"
#include "mini_proto.h"
#include "gps/gps.h"
#include "../data/dashboard_state.h"
#include "../data/trip_computer.h"   /* los contadores del viaje en curso */
#include "../alarma_estado.h"   /* alarma_estado_bits(): que alarmas estan activas */
#include "frigo.h"
#include "watchdog.h"   /* watchdog_heartbeat: la tarea late (ver watchdog.c) */
#include "../ne185/ne185.h"
#include "../ui.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <errno.h>
#include <time.h>

static const char *TAG = "udp_tx";

static uint32_t s_sent_ok = 0;
static uint32_t s_sent_err = 0;

/* Días transcurridos desde 1970-01-01 para una fecha civil (algoritmo
 * days_from_civil de Howard Hinnant). Exacto para cualquier fecha del
 * calendario gregoriano proléptico y sin depender de la libc. */
static int32_t dias_desde_1970(int y, unsigned m, unsigned d)
{
    y -= (m <= 2);
    const int32_t  era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);                 /* [0, 399]    */
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;     /* [0, 146096] */
    return era * 146097 + (int32_t)doe - 719468;
}

static void build_msg(mini_msg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->version = MINI_PROTO_VERSION;

    /* Batería + DC/DC desde dashboard_state */
    dashboard_snapshot_t snap;
    dashboard_state_snapshot(&snap);
    /* bat_fresh y dcdc_fresh, NO bat_has/dcdc_has: "has" solo dice que ALGUNA
     * VEZ hubo dato. Si el SmartShunt deja de hablar -- se apaga, se va el BLE,
     * se queda sin pila -- con "has" se seguiria mandando el ultimo porcentaje
     * como si fuera de ahora, y el satelite lo pintaria tan tranquilo. Con
     * "fresh" se manda el sentinela de "no hay dato" a los 30 s y la 3,5" lo
     * ensena apagado, que es la verdad. Las aguas ya lo hacian bien (cd.fresh).
     * Visto auditando el 24-ago-2026. */
    if (snap.bat_fresh) {
        /* El SoC puede ser "sin dato" aunque el shunt este hablando (hasta que
         * sincroniza manda el NA). Se traduce al centinela del protocolo, en vez
         * de mandar un 102.3 % que la cabina saturaba a "100 %" en verde: el
         * porcentaje es el unico campo que decide si la bateria esta llena, asi
         * que un valor falso ahi no es cosmetico. Visto en el banco el
         * 24-sep-2026 (2.619 tramas con SOC=102.3 % en una tanda). */
        out->shunt_soc_deci      = (snap.soc_deci <= 1000)
                                   ? (int16_t)snap.soc_deci : MINI_NO_DATA_I16;
        /* EL CERO DEL SHUNT NO ES UN VOLTAJE (tanda 1 de la auditoria del
         * 18-oct-2026). victron_ble.c traduce el NA del SmartShunt (0x7FFF) a 0
         * -- y la pantalla de 7" lo pinta como "--" --, pero por UDP viajaba 0 y
         * la cabina lo daba por bueno (su centinela es -32768): enseñaba
         * "0.00 V" con la bateria desconectada. 0 V en una bateria de 12 no es
         * una lectura, es la ausencia de lectura. */
        out->shunt_voltage_centi = (snap.bat_v_centi > 0)
                                   ? (int16_t)snap.bat_v_centi : MINI_NO_DATA_I16;
        out->shunt_current_milli = snap.bat_i_milli;
        out->aux_value_raw       = snap.aux_value;
        out->aux_input           = snap.aux_input;
    } else {
        out->shunt_soc_deci      = MINI_NO_DATA_I16;
        out->shunt_voltage_centi = MINI_NO_DATA_I16;
        out->shunt_current_milli = MINI_NO_DATA_I32;
        out->aux_value_raw       = 0;
        out->aux_input           = MINI_NO_DATA_U8;
    }
    if (snap.dcdc_fresh) {
        /* Y EL RANGO, que aqui no habia (tanda 1 de la auditoria del
         * 18-oct-2026): el registro del Orion no tiene filtro de cordura (el del
         * battery monitor si), asi que un valor corrupto de 32768..65535 se
         * volvia NEGATIVO al hacer el cast y viajaba como -327 V. Fuera de
         * 0..60 V (6000 centi) no es un voltaje: se manda "sin dato". */
        out->dcdc_v_in_centi  = (snap.dc_in_v_centi  > 0 && snap.dc_in_v_centi  <= 6000)
                                ? (int16_t)snap.dc_in_v_centi  : MINI_NO_DATA_I16;
        out->dcdc_v_out_centi = (snap.dc_out_v_centi > 0 && snap.dc_out_v_centi <= 6000)
                                ? (int16_t)snap.dc_out_v_centi : MINI_NO_DATA_I16;
        out->dcdc_state       = snap.dc_state;
    } else {
        out->dcdc_v_in_centi  = MINI_NO_DATA_I16;
        out->dcdc_v_out_centi = MINI_NO_DATA_I16;
        out->dcdc_state       = 0;
    }

    /* Frigo */
    frigo_state_t fr_copy;
    frigo_get_state_copy(&fr_copy);
    const frigo_state_t *fr = &fr_copy;
    if (fr && fr->T_Congelador > -100.0f) {
        float v = fr->T_Congelador * 100.0f;
        if (v >  32767.0f) v =  32767.0f;
        if (v < -32767.0f) v = -32767.0f;
        out->frigo_temp_centi = (int16_t)v;
        out->frigo_fan_pct    = fr->fan_percent;
    } else {
        out->frigo_temp_centi = MINI_NO_DATA_I16;
        out->frigo_fan_pct    = 0;
    }

    /* Aguas */
    ne185_data_t cd;
    ne185_get(&cd);
    if (cd.fresh) {
        out->water_clean = cd.s1;
        out->water_gray  = cd.r1;
    } else {
        out->water_clean = MINI_NO_DATA_U8;
        out->water_gray  = MINI_NO_DATA_U8;
    }

    /* Exterior (slot Exterior del frigo; 1-Wire si esta asignado) */
    if (fr && fr->T_Exterior > -100.0f) {
        float ve = fr->T_Exterior * 100.0f;
        if (ve >  32767.0f) ve =  32767.0f;
        if (ve < -32767.0f) ve = -32767.0f;
        out->exterior_temp_centi = (int16_t)ve;
    } else {
        out->exterior_temp_centi = MINI_NO_DATA_I16;
    }

    /* Salvapantallas: el mini atenúa su pantalla cuando el 7" está dormido. */
    out->screensaver = ui_screensaver_is_active() ? 1 : 0;

    /* Día de calendario para el satélite, que no tiene reloj: se apaga con el
     * contacto y al encender no sabe ni qué día es. Con esto puede contar las
     * noches que ha durado una parada. Ver mini_proto.h para el porqué del día
     * y no del instante.
     *
     * time(NULL) devuelve una fecha de 1970 mientras el RTC no haya puesto la
     * hora del sistema; en ese caso se manda 0 = "sin dato" y el satélite
     * espera callado en vez de contar noches inventadas. */
    time_t ahora = time(NULL);
    struct tm tm_local;
    if (ahora > MINI_EPOCH_VALIDO && localtime_r(&ahora, &tm_local)) {
        /* Se manda el epoch DESPLAZADO a hora local, para que el satélite saque
         * el día de calendario con una división y no necesite saber nada de
         * husos ni de horario de verano.
         *
         * Se arma a mano desde los campos de localtime_r en vez de usar
         * tm_gmtoff o timegm(): la newlib de ESP-IDF no trae ninguno de los
         * dos. Y a mano sale exacto, sin el "adivina el horario de verano" que
         * haría mktime() en las dos horas ambiguas del año. */
        int32_t dias = dias_desde_1970(tm_local.tm_year + 1900,
                                       (unsigned)tm_local.tm_mon + 1,
                                       (unsigned)tm_local.tm_mday);
        out->epoch_local = (uint32_t)(dias * 86400L
                                      + tm_local.tm_hour * 3600
                                      + tm_local.tm_min * 60
                                      + tm_local.tm_sec);
        /* El desfase, para que la cabina pueda corregir las duraciones que
         * cruzan un cambio de horario (ver mini_proto.h). Sale de la diferencia
         * entre el epoch local y el UTC, que aqui se tienen los dos. */
        int32_t off = (int32_t)out->epoch_local - (int32_t)ahora;
        out->utc_off_quince = (uint8_t)(int8_t)(off / 900);
        /* Una linea, la primera vez: la hora local que se esta mandando y con
         * que desfase. Es lo que se mira cuando "la hora va rara" (14-oct-2026),
         * y de paso deja en el log que el desfase horario viaja de verdad. */
        static bool avisado = false;
        if (!avisado) {
            avisado = true;
            ESP_LOGI(TAG, "hora local: %04d-%02d-%02d %02d:%02d:%02d (UTC%+ld h) "
                          "-> a la cabina con desfase %d cuartos",
                     tm_local.tm_year + 1900, tm_local.tm_mon + 1, tm_local.tm_mday,
                     tm_local.tm_hour, tm_local.tm_min, tm_local.tm_sec,
                     (long)(off / 3600), out->utc_off_quince);
        }
    } else {
        out->epoch_local = 0;
        out->utc_off_quince = 0;   /* sin hora valida no hay desfase que dar */
    }

    /* Estado del GPS. Los tres estados salen de gps.c: si no llegan tramas el
     * modulo no esta, si llegan sin posicion esta buscando. */
    {
        gps_data_t g;
        gps_get(&g);
        out->gps_estado = !g.hay_datos ? 0 : (g.hay_fix ? 2 : 1);
    }

    /* Alarmas activas (bitmask MINI_ALARM_*), de alarma_estado.c, que es la
     * unica que sabe si una alarma esta activa de verdad: tiene los umbrales y
     * las temporizaciones. La cabina lo usa para poner el icono del altavoz en
     * la tarjeta que toca y para saber CUAL silencia cuando se toca.
     *
     * OJO con el sentido de "activa": dice que la CONDICION se cumple, suene o
     * este silenciada. Lo que se calla con el silencio es el pitido de la P4,
     * no la senal visual de la cabina (decision del 13-sep-2026), asi que la
     * cabina tiene que seguir enseñando la alarma aunque la P4 este callada.
     *
     * Se lee SIN cerrojo a proposito: alarma_estado.c publica cada alarma en un
     * bool volatile (asignacion de un byte), asi que no hay valor a medias. */
    out->alarmas = alarma_estado_bits();

    /* ── Contadores del viaje en curso (16-oct-2026) ────────────────────────
     *
     * Los km y los minutos de CONDUCCION que la pantalla de 5" enseña en su
     * tarjeta del viaje. Salen del ordenador de viaje, que es el unico que los
     * sabe (ver mini_proto.h, bloque CONTADORES DEL VIAJE, para el reparto en
     * bytes sueltos y para el porqué de km de GPS y no del cuadro).
     *
     * SI NO HAY VIAJE ABIERTO SE MANDA "SIN DATO", y esto es lo importante:
     * los contadores de trip_computer NO se borran al cerrar un viaje (se
     * quedan con lo del anterior hasta el siguiente reset), asi que mandarlos
     * tal cual pondria en la pantalla los km del viaje de la semana pasada como
     * si fueran de ahora. El flag 'active' es justo el que distingue las dos
     * cosas. */
    {
        trip_computer_t t;
        trip_computer_get(&t);
        if (t.active) {
            /* km en DECIMAS (100 m de resolucion), saturado al maximo que cabe
             * antes del centinela: 6553,4 km en un solo viaje no pasa, pero si
             * pasara es mejor un numero topado que uno dado la vuelta. */
            long km_dec = (long)(t.km * 10.0 + 0.5);
            if (km_dec < 0) km_dec = 0;
            if (km_dec > (long)(MINI_NO_DATA_U16 - 1)) km_dec = (long)(MINI_NO_DATA_U16 - 1);
            out->viaje_km_hi = (uint8_t)((uint16_t)km_dec >> 8);
            out->viaje_km_lo = (uint8_t)((uint16_t)km_dec & 0xFF);

            /* Minutos de conduccion, con el mismo tope. */
            long min = (long)(t.seconds_driving / 60);
            if (min < 0) min = 0;
            if (min > (long)(MINI_NO_DATA_U16 - 1)) min = (long)(MINI_NO_DATA_U16 - 1);
            out->viaje_min_hi = (uint8_t)((uint16_t)min >> 8);
            out->viaje_min_lo = (uint8_t)((uint16_t)min & 0xFF);
        } else {
            out->viaje_km_hi  = (uint8_t)(MINI_NO_DATA_U16 >> 8);
            out->viaje_km_lo  = (uint8_t)(MINI_NO_DATA_U16 & 0xFF);
            out->viaje_min_hi = (uint8_t)(MINI_NO_DATA_U16 >> 8);
            out->viaje_min_lo = (uint8_t)(MINI_NO_DATA_U16 & 0xFF);
        }
    }

    /* CRC32 sobre todo el msg excepto el propio campo crc32. */
    out->crc32 = esp_crc32_le(0, (const uint8_t *)out,
                               sizeof(*out) - sizeof(uint32_t));
}

static void tx_task(void *arg)
{
    (void)arg;

    /* Crear (o recrear) el socket con reintento. Antes un fallo al arranque
     * hacia vTaskDelete -> la tarea moria para siempre y el display mini se
     * quedaba mudo hasta reboot. Ahora reintentamos cada 5 s. */
    int sock = -1;
    while (sock < 0) {
        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            ESP_LOGE(TAG, "socket() fallo: errno=%d (reintento en 5s)", errno);
            /* Late TAMBIEN aqui: la tarea esta viva, solo no tiene socket. El
             * watchdog vigila que la tarea no se cuelgue, no que el socket este
             * listo (si no, un AP raro provocaria un reset en bucle). */
            watchdog_heartbeat(WD_TASK_UDP_TX);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        int yes = 1;
        if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) < 0) {
            ESP_LOGE(TAG, "SO_BROADCAST fallo: errno=%d (reintento en 5s)", errno);
            close(sock);
            sock = -1;
            watchdog_heartbeat(WD_TASK_UDP_TX);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
    }

    struct sockaddr_in dest = {0};
    dest.sin_family      = AF_INET;
    dest.sin_port        = htons(MINI_PROTO_UDP_PORT);
    dest.sin_addr.s_addr = inet_addr("192.168.4.255");

    ESP_LOGI(TAG, "Publisher UDP listo -> 192.168.4.255:%d sizeof(mini_msg_t)=%u",
             MINI_PROTO_UDP_PORT, (unsigned)sizeof(mini_msg_t));

    /* Espera unos segundos a que el AP esté del todo levantado. */
    vTaskDelay(pdMS_TO_TICKS(3000));

    TickType_t next = xTaskGetTickCount();
    static uint32_t last_log_ok = 0;
    int fallos_seguidos = 0;   /* para recrear el socket solo si falla de verdad */
    for (;;) {
        mini_msg_t msg;
        build_msg(&msg);

        int n = sendto(sock, &msg, sizeof(msg), 0,
                       (struct sockaddr *)&dest, sizeof(dest));
        if (n == (int)sizeof(msg)) {
            s_sent_ok++;
        } else {
            s_sent_err++;
            if ((s_sent_err % 10) == 1) {
                ESP_LOGW(TAG, "sendto err errno=%d (ok=%lu err=%lu)",
                         errno, (unsigned long)s_sent_ok, (unsigned long)s_sent_err);
            }
            /* ENOTCONN (118): el socket se creo ANTES de que el AP terminara de
             * levantarse, asi que quedo ligado a una interfaz que todavia no
             * existia y ya no envia NUNCA (ok=0 para siempre). Visto el
             * 7-oct-2026 en el banco: 20 minutos con "errno=118 (ok=0 err=1)" y
             * el satelite sin recibir nada, con el AP funcionando y clientes
             * conectados. Se recrea el socket: al crearlo ahora, la interfaz ya
             * esta y el envio sale. Se exige un minimo de fallos seguidos para
             * no estar cerrando y abriendo por un fallo suelto. */
            if (errno == ENOTCONN && ++fallos_seguidos >= 3) {
                fallos_seguidos = 0;
                ESP_LOGW(TAG, "socket sin salida (ENOTCONN): lo recreo");
                close(sock);
                /* A -1: sin esto el while de abajo no se ejecuta NUNCA (sock
                 * sigue valiendo el fd ya cerrado), no se llama a socket(), el
                 * setsockopt va a un descriptor cerrado y se loggea "socket
                 * recreado" para nada. Lo cazo la revision del 13-oct-2026: el
                 * arreglo, tal cual, era un no-op. */
                sock = -1;
                /* CON REINTENTO (auditoria del 13-oct-2026): si socket() falla
                 * aqui (sin descriptores, sin memoria) el anterior quedaba
                 * cerrado y el nuevo en -1, asi que el sendto siguiente fallaba
                 * con EBADF -- que NO es ENOTCONN y por tanto no volvia a
                 * entrar por aqui: la telemetria se quedaba muerta hasta el
                 * siguiente reinicio, con un unico warning cada 10 fallos. Se
                 * reintenta en bucle, como en el arranque (ver udp_tx_start). */
                while (sock < 0) {
                    sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                    if (sock < 0) {
                        ESP_LOGE(TAG, "no puedo recrear el socket (errno=%d): "
                                      "reintento en 5 s", errno);
                        watchdog_heartbeat(WD_TASK_UDP_TX);
                        vTaskDelay(pdMS_TO_TICKS(5000));
                    }
                }
                int yes2 = 1;
                setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes2, sizeof(yes2));
                ESP_LOGW(TAG, "socket recreado");
            }
        }
        if (n == (int)sizeof(msg)) fallos_seguidos = 0;

        if (s_sent_ok - last_log_ok >= 30) {
            ESP_LOGI(TAG, "TX ok=%lu err=%lu",
                     (unsigned long)s_sent_ok, (unsigned long)s_sent_err);
            last_log_ok = s_sent_ok;
        }

        watchdog_heartbeat(WD_TASK_UDP_TX);   /* late: la tarea esta viva */
        vTaskDelayUntil(&next, pdMS_TO_TICKS(1000));
    }
}

void udp_tx_start(void)
{
    xTaskCreate(tx_task, "udp_tx", 4096, NULL, 4, NULL);
}
