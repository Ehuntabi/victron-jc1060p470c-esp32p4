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
#include "../dashboard_state.h"
#include "frigo.h"
#include "../ne185/ne185.h"
#include "esp_log.h"
#include "esp_crc.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <errno.h>

static const char *TAG = "udp_tx";

static uint32_t s_sent_ok = 0;
static uint32_t s_sent_err = 0;

static void build_msg(mini_msg_t *out)
{
    memset(out, 0, sizeof(*out));
    out->version = MINI_PROTO_VERSION;

    /* Batería + DC/DC desde dashboard_state */
    dashboard_snapshot_t snap;
    dashboard_state_snapshot(&snap);
    if (snap.bat_has) {
        out->shunt_soc_deci      = (int16_t)snap.soc_deci;
        out->shunt_voltage_centi = (int16_t)snap.bat_v_centi;
        out->shunt_current_milli = snap.bat_i_milli;
    } else {
        out->shunt_soc_deci      = MINI_NO_DATA_I16;
        out->shunt_voltage_centi = MINI_NO_DATA_I16;
        out->shunt_current_milli = MINI_NO_DATA_I32;
    }
    if (snap.dcdc_has) {
        out->dcdc_v_in_centi  = (int16_t)snap.dc_in_v_centi;
        out->dcdc_v_out_centi = (int16_t)snap.dc_out_v_centi;
        out->dcdc_state       = snap.dc_state;
    } else {
        out->dcdc_v_in_centi  = MINI_NO_DATA_I16;
        out->dcdc_v_out_centi = MINI_NO_DATA_I16;
        out->dcdc_state       = 0;
    }

    /* Frigo */
    const frigo_state_t *fr = frigo_get_state();
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

    /* Exterior - sin sensor todavía */
    out->exterior_temp_centi = MINI_NO_DATA_I16;

    /* CRC32 sobre todo el msg excepto el propio campo crc32. */
    out->crc32 = esp_crc32_le(0, (const uint8_t *)out,
                               sizeof(*out) - sizeof(uint32_t));
}

static void tx_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() falló: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }
    int yes = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes)) < 0) {
        ESP_LOGE(TAG, "SO_BROADCAST falló: errno=%d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
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
        }

        if (s_sent_ok - last_log_ok >= 30) {
            ESP_LOGI(TAG, "TX ok=%lu err=%lu",
                     (unsigned long)s_sent_ok, (unsigned long)s_sent_err);
            last_log_ok = s_sent_ok;
        }

        vTaskDelayUntil(&next, pdMS_TO_TICKS(1000));
    }
}

void udp_tx_start(void)
{
    xTaskCreate(tx_task, "udp_tx", 4096, NULL, 4, NULL);
}
