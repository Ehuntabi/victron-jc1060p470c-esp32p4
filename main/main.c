/* main.c */
#include <stdio.h>
#include <sys/time.h>
#include <inttypes.h>
#include <lvgl.h>
#include "display.h"
#include "esp_bsp.h"
#include "esp_lvgl_port.h"
#include "victron_ble.h"
#include "esp_log.h"
#include "esp_flash.h"
#include "esp_chip_info.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "ui.h"
#include "dashboard_state.h"
#include "config_server.h"
#include "frigo.h"
#include "battery_history.h"
#include "log_cleanup.h"
#include "alerts.h"
#include "audio_es8311.h"
#include "esp_bsp.h"
#include "rtc_rx8025t.h"
#include "datalogger.h"
#include "ui/frigo_panel.h"
#include "camera.h"
#include "ui/ausente_mode.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_task_wdt.h"
#include "watchdog.h"
#include "config_storage.h"
#include "log_capture/log_capture.h"
#include "energy_today.h"
#include "trip_computer.h"
#include "pzem004t.h"
#include "ne185/ne185.h"
#include "net/udp_tx.h"
#include "sim_overview.h"
#include "splash.h"
#include <time.h>

/* Zona horaria de Madrid (CET/CEST con DST automático).
 * Formato POSIX TZ: cambio último domingo de marzo (M3.5.0) y octubre (M10.5.0/3) */
#define TZ_EUROPE_MADRID "CET-1CEST,M3.5.0,M10.5.0/3"

static const char *TAG = "VICTRON_LVGL_APP";
#define logSection(section) ESP_LOGI(TAG, "\n\n***** %s *****\n", section)
#define LVGL_PORT_ROTATION_DEGREE 90
#define REBOOT_INTERVAL_US (24ULL * 60 * 60 * 1000000) /* 24 horas */
#define LOG_INTERVAL_MS    (5 * 60 * 1000)             /* 5 minutos */
#define ALARM_RISE_MINUTES  30      /* minutos subiendo para alarma */
#define ALARM_TEMP_THRESHOLD -2.0f  /* °C — si supera esto y lleva subiendo 30min */

/* ── Reboot timer ────────────────────────────────────────────── */
static void reboot_timer_cb(void *arg)
{
    ESP_LOGI(TAG, "Rebooting after 24h uptime...");
    /* Flush de datalogger y battery_history a SD para no perder muestras */
    datalogger_flush();
    battery_history_flush();
    esp_restart();
}

/* ── Backup periódico de hora a NVS ──────────────────────────── */
static void rtc_backup_timer_cb(void *arg)
{
    time_t now = time(NULL);
    if (now < 1000000000L) return;  /* hora aún no válida */
    nvs_handle_t nh;
    if (nvs_open("rtc_backup", NVS_READWRITE, &nh) == ESP_OK) {
        nvs_set_i64(nh, "epoch", (int64_t)now);
        nvs_commit(nh);
        nvs_close(nh);
    }
}

/* ── Modo nocturno: aplica brillo según hora local del RTC ─────── */
extern ui_state_t *ui_get_state(void);

static bool night_hour_in_window(int now_h, uint8_t start_h, uint8_t end_h)
{
    if (start_h == end_h) return false;
    if (start_h < end_h)  return now_h >= start_h && now_h < end_h;
    /* Cruza medianoche (p. ej. 22 → 7) */
    return now_h >= start_h || now_h < end_h;
}

/* Auto-brillo por luminosidad de la camara. TODO(loop): toggle en Settings. */
static bool s_auto_brightness = true;

/* Mapea luminosidad ambiente (0-255) a brillo de pantalla (%). Suelo de
 * seguridad para que la pantalla nunca quede ilegible, y cuantizado a pasos de
 * 5% para que no parpadee con variaciones pequenas de luz. */
static int luma_to_brightness(uint8_t luma)
{
    const int MINB  = 12;   /* nunca por debajo (visibilidad) */
    const int SCALE = 150;  /* luma a la que se alcanza el 100% (calibrable) */
    int b = MINB + (int)luma * (100 - MINB) / SCALE;
    if (b > 100) b = 100;
    if (b < MINB) b = MINB;
    return (b / 5) * 5;     /* pasos de 5% -> sin parpadeo */
}

static void night_mode_timer_cb(void *arg)
{
    ui_state_t *ui = (ui_state_t *)arg;
    if (!ui) return;

    /* Solo aplicamos si el objetivo cambia (evita reescribir el duty del LEDC). */
    static int s_last_target = -1;
    int target;

    if (ausente_is_active()) {
        /* Modo ausente: precedencia MAXIMA -> pantalla apagada y se mantiene asi. */
        target = 0;
    } else if (ui->screensaver.active) {
        /* El screensaver gestiona su propio brillo. */
        return;
    } else {
        /* Precedencia: franja nocturna (atenuacion por horario) > auto-brillo por
         * luminosidad de la camara > brillo manual. El modo noche NUNCA lo pisa
         * el auto-brillo (requisito del usuario). */
        bool night_win = false;
        if (ui->night_mode.enabled) {
            time_t now = time(NULL);
            if (now >= 1000000000L) {
                struct tm tm_local;
                localtime_r(&now, &tm_local);
                night_win = night_hour_in_window(tm_local.tm_hour,
                                                 ui->night_mode.start_h,
                                                 ui->night_mode.end_h);
            }
        }
        uint8_t luma;
        if (night_win) {
            target = ui->night_mode.brightness;
        } else if (s_auto_brightness && camera_get_luma(&luma)) {
            target = luma_to_brightness(luma);
        } else {
            target = ui->brightness;
        }
    }

    if (target == s_last_target) return;
    s_last_target = target;
    bsp_display_brightness_set(target);
}

/* ── Estado UI global ────────────────────────────────────────── */
static ui_state_t *s_ui = NULL;

/* Re-aplica el brillo segun la arbitracion actual (night_mode_timer_cb). Lo usa
 * el modo ausente para apagar/restaurar la pantalla al instante, sin esperar al
 * tick de 4 s del timer. */
void brightness_apply_now(void)
{
    if (s_ui) night_mode_timer_cb(s_ui);
}

/* ── Callback frigo: actualiza UI + log cada 5 min ──────────── */
/* Latido del watchdog para frigo_task. Lo invoca frigo_task en cada
 * iteracion (via frigo_set_heartbeat_cb), no el simulador, asi que vigila
 * la tarea real aunque el modo simulacion este activo. */
static void frigo_heartbeat(void) { watchdog_heartbeat(WD_TASK_FRIGO); }

static void frigo_update_cb(const frigo_state_t *state)
{
    if (!s_ui) return;

    /* ── Alarma congelador ─────────────────────────────────────── */
    static float   s_temp_prev     = 0.0f;
    static int64_t s_rising_since  = 0;
    static bool    s_alarm_active  = false;

    float T = state->T_Congelador;
    int64_t now_ms = esp_timer_get_time() / 1000;

    if (T > -120.0f) {  /* sensor conectado */
        if (T > s_temp_prev && s_temp_prev > -120.0f) {
            /* temperatura subiendo */
            if (s_rising_since == 0) s_rising_since = now_ms;
            int64_t rising_ms = now_ms - s_rising_since;
            bool alarma = (rising_ms >= (int64_t)alerts_get_freezer_minutes() * 60 * 1000)
                          && (T > alerts_get_freezer_temp_c());
            if (alarma != s_alarm_active) {
                s_alarm_active = alarma;
                if (lvgl_port_lock(50)) {
                    ui_set_freezer_alarm(s_ui, alarma);
                    lvgl_port_unlock();
                }
            }
        } else {
            /* temperatura bajando o estable — resetear contador */
            s_rising_since = 0;
            if (s_alarm_active) {
                s_alarm_active = false;
                if (lvgl_port_lock(50)) {
                    ui_set_freezer_alarm(s_ui, false);
                    lvgl_port_unlock();
                }
            }
        }
        s_temp_prev = T;
    }

    /* ── Actualizar UI ─────────────────────────────────────────── */
    if (lvgl_port_lock(50)) {
        ui_frigo_panel_update(s_ui, state);
        lvgl_port_unlock();
    }

    /* ── Log cada 5 minutos ────────────────────────────────────── */
    static int64_t s_last_log = 0;
    int64_t now = esp_timer_get_time() / 1000;
    if (now - s_last_log >= LOG_INTERVAL_MS) {
        s_last_log = now;
        datalogger_log(state);
    }
}

/* ── Touch callback para screensaver ─────────────────────────── */
static void touch_activity_cb(lv_indev_drv_t *drv, uint8_t event)
{
    if (event != LV_EVENT_PRESSED) return;
    /* Si el salvapantallas esta activo, este primer toque solo debe
     * despertarlo, NO navegar. feedback_cb corre antes de despachar el evento
     * al widget; consumimos la pulsacion con lv_indev_wait_release() para que
     * no genere CLICKED ni gesto en el widget de debajo. Hace falta un segundo
     * toque para cambiar de pantalla. */
    bool was_active = ui_screensaver_is_active();
    ui_notify_user_activity();              /* despierta: active -> false */
    if (was_active) {
        lv_indev_t *indev = lv_indev_get_act();
        if (indev) lv_indev_wait_release(indev);
    }
}

/* Tarea one-shot: 30s tras boot vuelca el buffer log_capture a SD con el
 * reset reason en el nombre, y aplica rotacion FIFO (max 20 archivos).
 * Asume que la SD ya esta montada (datalogger_init ya ha corrido). */
static void log_autosave_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(30000));
    esp_err_t err = log_capture_autosave_now(20);
    if (err == ESP_OK) {
        ESP_LOGI("LOGSAVE", "autosave OK -> /sdcard/log_*.txt (rotacion FIFO 20)");
    } else {
        ESP_LOGW("LOGSAVE", "autosave failed: %s", esp_err_to_name(err));
    }
    vTaskDelete(NULL);
}


/* ── app_main ────────────────────────────────────────────────── */
void app_main(void)
{
    /* --- Captura de logs en RAM (hook a esp_log_set_vprintf) --- */
    /* Antes que nada para capturar todo el boot. Ringbuffer en PSRAM ~120 KB. */
    log_capture_init();

    /* --- Chip info --- */
    logSection("LVGL init start");
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG,
        "This is %s chip, %d cores, features: %s%s%s%s",
        CONFIG_IDF_TARGET, chip_info.cores,
        (chip_info.features & CHIP_FEATURE_WIFI_BGN)   ? "WiFi/"    : "",
        (chip_info.features & CHIP_FEATURE_BT)         ? "BT/"      : "",
        (chip_info.features & CHIP_FEATURE_BLE)        ? "BLE/"     : "",
        (chip_info.features & CHIP_FEATURE_IEEE802154) ? "802.15.4" : ""
    );

    /* --- Flash and heap info --- */
    uint32_t flash_size;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(TAG, "Get flash size failed");
        return;
    }
    ESP_LOGI(TAG,
        "%" PRIu32 "MB flash, min free heap: %" PRIu32 ", free PSRAM: %u",
        flash_size / (1024 * 1024),
        esp_get_minimum_free_heap_size(),
        heap_caps_get_free_size(MALLOC_CAP_SPIRAM)
    );

    /* --- NVS --- */
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    /* --- Watchdog: registra causa del ultimo reset y arranca task monitor LVGL --- */
    watchdog_init();

    /* --- Display --- */
    logSection("Display init");
    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size   = EXAMPLE_LCD_QSPI_H_RES * EXAMPLE_LCD_QSPI_V_RES,
#if LVGL_PORT_ROTATION_DEGREE == 90
        .rotate = LV_DISP_ROT_90,
#elif LVGL_PORT_ROTATION_DEGREE == 180
        .rotate = LV_DISP_ROT_180,
#elif LVGL_PORT_ROTATION_DEGREE == 270
        .rotate = LV_DISP_ROT_270,
#else
        .rotate = LV_DISP_ROT_NONE,
#endif
    };
    /* Bump LVGL task stack 7168 -> 12288 B. Margen para draw recursion +
     * assert handler en escenarios pesados (modales anidados, etc).
     * Coste ~5 KB en RAM interna (de ~474 KB libres tras LV_MEM_CUSTOM). */
    cfg.lvgl_port_cfg.task_stack = 12288;
    bsp_display_start_with_config(&cfg);
    bsp_display_brightness_set(80);

    /* --- UI --- */
    if (lvgl_port_lock(0)) {
        s_ui = ui_get_state();
        ui_init();
        /* Splash inmediatamente despues de ui_init para tapar la pantalla
         * mientras dura el resto del setup (BLE, audio, datalogger...). */
        splash_show();
        lvgl_port_unlock();
    }
    /* No volvemos a tocar el brillo aqui: ya esta a 80% desde la linea de
     * arriba, y night_mode_timer_cb al final del boot lo lleva al valor de
     * usuario en una sola transicion visible. */

    /* --- Touch callback --- */
    lv_indev_t *indev = bsp_display_get_input_dev();
    if (indev) {
        indev->driver->feedback_cb = touch_activity_cb;
    }

    /* --- SD + RTC + Frigo --- */
    esp_err_t sd_err = datalogger_init();
    if (sd_err != ESP_OK)
        ESP_LOGW(TAG, "datalogger_init failed: %s", esp_err_to_name(sd_err));

    /* Auto-save de logs a SD 30 s tras boot (con rotacion FIFO 20 archivos).
     * Stack 12 KB: fprintf bucle + opendir/readdir + bubble sort + unlink. */
    xTaskCreate(log_autosave_task, "logsave", 12288, NULL, 3, NULL);

    /* TZ desde NVS (default Madrid) antes de cualquier settimeofday/mktime/localtime */
    {
        char tz_buf[48];
        load_timezone(tz_buf, sizeof(tz_buf));
        setenv("TZ", tz_buf, 1);
        tzset();
        ESP_LOGI(TAG, "TZ: %s", tz_buf);
    }

    esp_err_t rtc_err = rtc_init(bsp_i2c_get_handle());
    /* El RTC almacena hora local (Madrid). mktime la interpreta con la TZ
     * recién configurada y devuelve el epoch UTC correcto. */
    struct tm t_rtc;
    if (rtc_get_time(&t_rtc) == ESP_OK && t_rtc.tm_year >= 120) {
        t_rtc.tm_isdst = -1;   /* dejar que mktime decida DST */
        time_t epoch = mktime(&t_rtc);
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "Hora del RTC (local): %04d-%02d-%02d %02d:%02d:%02d",
                 t_rtc.tm_year + 1900, t_rtc.tm_mon + 1, t_rtc.tm_mday,
                 t_rtc.tm_hour, t_rtc.tm_min, t_rtc.tm_sec);
    } else {
        /* RTC sin hora válida — restaurar desde backup NVS */
        nvs_handle_t nh;
        if (nvs_open("rtc_backup", NVS_READONLY, &nh) == ESP_OK) {
            int64_t epoch = 0;
            if (nvs_get_i64(nh, "epoch", &epoch) == ESP_OK && epoch > 1000000000L) {
                struct timeval tv = { .tv_sec = (time_t)epoch, .tv_usec = 0 };
                settimeofday(&tv, NULL);
                ESP_LOGI(TAG, "Hora restaurada desde NVS backup: %lld", epoch);
            }
            nvs_close(nh);
        }
    }
    if (rtc_err != ESP_OK)
        ESP_LOGW(TAG, "rtc_init failed: %s", esp_err_to_name(rtc_err));

    /* Refresco inmediato del label de la hora — sin esperar al timer de 30s */
    ui_refresh_clock();

    esp_err_t frigo_err = frigo_init(frigo_update_cb);
    if (frigo_err != ESP_OK)
        ESP_LOGW(TAG, "frigo_init failed: %s", esp_err_to_name(frigo_err));
    frigo_set_heartbeat_cb(frigo_heartbeat);

    /* Crear el mutex de dashboard_state antes de arrancar httpd/BLE/sim,
     * que son los que acceden al estado concurrentemente. */
    dashboard_state_init();

    /* --- WiFi + config server --- */
    wifi_ap_init();
    config_server_start();
    /* Actualizar SSID real del AP en el UI */
    if (s_ui) ui_update_wifi_ssid(s_ui);

    /* --- Publisher UDP hacia el mini (1 Hz broadcast 192.168.4.255:4242) --- */
    udp_tx_start();

    /* --- BLE Victron --- */
    battery_history_init();
    log_cleanup_init(60); /* Borrar logs > 60 dias */
    alerts_init();
    energy_today_init();
    trip_computer_init();

    /* PZEM-004T v3 (AC 220 V) en UART2: TX=GPIO1 (pin 7), RX=GPIO2 (pin 9)
     * del JP1 — reasignados desde GPIO32/33 (pines 19/21) para caber en el
     * conector IDC 2x10 y librar GPIO20 (pin 17) que es el único con ADC1.
     * Si no hay modulo fisico, sigue funcionando: marca has_data=false. */
    pzem_config_t pzem_cfg = {
        .uart_num       = UART_NUM_2,
        .tx_gpio        = GPIO_NUM_1,
        .rx_gpio        = GPIO_NUM_2,
        .slave_address  = 0x01,
        .poll_period_ms = 2000,
    };
    pzem_init(&pzem_cfg);

    /* NE185 (RS-485 directo via MAX485 onboard, UART1 GPIO 26/27).
     * Habla con el derivador Nordelettronica NE185 que sustituye al
     * panel NE187 retirado. Conector J5 expone el bus en MX 1.25 4P. */
    ne185_init();

    /* Audio: inicializar codec ES8311 + PA y hacer beep de prueba */
    {
        i2c_master_bus_handle_t i2c_bus = bsp_i2c_get_handle();
        if (i2c_bus) {
            esp_err_t ar = audio_init(i2c_bus);
            if (ar == ESP_OK) {
                /* Beeps en bucle para diagnostico */
                audio_play_jingle(AUDIO_JINGLE_BOOT_OK);
            } else {
                ESP_LOGW(TAG, "audio_init falla: %s", esp_err_to_name(ar));
            }
        } else {
            ESP_LOGW(TAG, "I2C bus no disponible para audio");
        }
    }
    victron_ble_register_callback(ui_on_panel_data);
    victron_ble_init();

    /* Modo simulacion: inyecta datos ficticios cambiantes para previsualizar
     * la vista Overview. Toggle desde sim_overview.h. */
    sim_overview_start();

    /* --- Timer reboot 24h --- */
    static esp_timer_handle_t reboot_timer;
    const esp_timer_create_args_t reboot_timer_args = {
        .callback        = &reboot_timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "24h_reboot",
    };
    esp_timer_create(&reboot_timer_args, &reboot_timer);
    esp_timer_start_periodic(reboot_timer, REBOOT_INTERVAL_US);

    /* Backup horario de la hora del sistema en NVS */
    static esp_timer_handle_t rtc_backup_timer;
    const esp_timer_create_args_t rtc_backup_args = {
        .callback        = &rtc_backup_timer_cb,
        .arg             = NULL,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "rtc_backup",
    };
    esp_timer_create(&rtc_backup_args, &rtc_backup_timer);
    esp_timer_start_periodic(rtc_backup_timer, 3600ULL * 1000000ULL);

    /* Modo nocturno: re-evalúa cada 60 s y aplica brillo segun la hora. */
    static esp_timer_handle_t night_timer;
    const esp_timer_create_args_t night_args = {
        .callback        = &night_mode_timer_cb,
        .arg             = s_ui,
        .dispatch_method = ESP_TIMER_TASK,
        .name            = "night_mode",
    };
    esp_timer_create(&night_args, &night_timer);
    /* Cada 4 s: suficiente para que el auto-brillo siga la luz ambiente con
     * fluidez (la luma ya va suavizada) sin trabajo inutil (aplica solo si cambia). */
    esp_timer_start_periodic(night_timer, 4ULL * 1000000ULL);
    /* Aplicación inmediata para no esperar al arrancar */
    night_mode_timer_cb(s_ui);

    /* Splash visible al menos 1.5 s desde su creacion, luego ocultar. */
    if (lvgl_port_lock(0)) {
        splash_hide();
        lvgl_port_unlock();
    }

    /* Fase 1 camara: solo init + deteccion del sensor (NO toca el brillo todavia).
     * Si falla, el resto del firmware sigue normal (aislado). */
    camera_init(bsp_i2c_get_handle());

    logSection("Setup complete");
}
