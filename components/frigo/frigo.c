#include "frigo.h"
#include "frigo_solar.h"
#include "onewire_bus.h"
#include "ds18b20.h"
#include "driver/ledc.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "FRIGO";

#define NVS_NS           "frigo"
#define NVS_KEY_ASSIGN   "assign"
#define NVS_KEY_TMIN     "tmin"
#define NVS_KEY_TMAX     "tmax"
#define NVS_KEY_FANMIN   "fanmin"
#define NVS_KEY_SOL_EN   "sol_en"
#define NVS_KEY_SOL_ON   "sol_on"
#define NVS_KEY_SOL_OFF  "sol_off"
#define NVS_KEY_ROLEADDR "roleaddr"

#define READ_INTERVAL_MS   2000
#define DS18B20_CONV_MS     800
#define LEDC_CHANNEL       LEDC_CHANNEL_0
#define LEDC_TIMER         LEDC_TIMER_0
#define LEDC_RESOLUTION    LEDC_TIMER_10_BIT
#define FAN_HYST_DEG       0.5f

static frigo_state_t      s_state = {
    .T_Aletas     = -127.0f,
    .T_Congelador = -127.0f,
    .T_Exterior   = -127.0f,
    .T_min        = 40,
    .T_max        = 50,
    .fan_min_pct  = FRIGO_FAN_MIN_DUTY_PCT,
    .assignment   = {0, 1, 2},
};
static SemaphoreHandle_t  s_mutex = NULL;
/* Ultima copia consistente de s_state, actualizada bajo lock en cada lectura
 * correcta; sirve de fallback si el lock esta ocupado (evita leer s_state
 * a medio escribir). */
static frigo_state_t      s_last_good;
static bool                s_last_good_valid = false;
static frigo_update_cb_t  s_cb    = NULL;
static frigo_heartbeat_cb_t s_hb_cb = NULL;
static onewire_bus_handle_t s_bus  = NULL;
static ds18b20_device_handle_t s_devs[FRIGO_MAX_SENSORS] = {0};
/* Config del bus guardada: hace falta para poder recrearlo en caliente. */
static onewire_bus_config_t s_bus_cfg = { .bus_gpio_num = FRIGO_ONEWIRE_GPIO };
static onewire_bus_rmt_config_t s_rmt_cfg = { .max_rx_bytes = 10 };
/* Ultimo estado de banderas visto, para avisar solo cuando aparece el problema
 * y no en cada escaneo mientras dura. */
static uint8_t s_scan_flags = 0;
/* La UI pide escaneo desde otro hilo; frigo_task lo atiende. */
static volatile bool s_rescan_req = false;
/* Definida mas abajo (junto al resto del escaneo), pero frigo_task la usa antes. */
static void escanear_y_aplicar(bool recuperar);
/* Cuando el sim inyecta datos (banco sin sondas), frigo_task deja de escribir
 * s_state para no pisar los valores simulados (evita el parpadeo de las temps). */
static volatile bool s_sim_mode = false;

/* Modo excedente solar: config + estado + ultima telemetria empujada por main. */
static bool     s_sol_en      = false;
static uint8_t  s_sol_on_pct  = 95;
static uint8_t  s_sol_off_pct = 80;
static frigo_solar_sm_t s_sol_sm = {0};
static uint16_t s_sol_soc_deci = 0;
static uint16_t s_sol_pv_w     = 0;
static bool     s_sol_shore    = false;
static bool     s_sol_fresh    = false;
static uint32_t s_sol_feed_ms  = 0;

/* ── NVS ─────────────────────────────────────────────────────── */
static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    uint8_t buf[FRIGO_MAX_SENSORS];
    size_t  len = sizeof(buf);
    if (nvs_get_blob(h, NVS_KEY_ASSIGN, buf, &len) == ESP_OK)
        memcpy(s_state.assignment, buf, FRIGO_MAX_SENSORS);
    /* Direccion de la sonda de cada rol. Si no esta (primer arranque con esta
     * version), queda a cero y escanear_y_aplicar hace la migracion desde
     * assignment[]. */
    size_t rlen = sizeof(s_state.role_addr);
    if (nvs_get_blob(h, NVS_KEY_ROLEADDR, s_state.role_addr, &rlen) != ESP_OK)
        memset(s_state.role_addr, 0, sizeof(s_state.role_addr));
    uint8_t v;
    if (nvs_get_u8(h, NVS_KEY_TMIN, &v) == ESP_OK) s_state.T_min = v;
    if (nvs_get_u8(h, NVS_KEY_TMAX, &v) == ESP_OK) s_state.T_max = v;
    if (nvs_get_u8(h, NVS_KEY_FANMIN, &v) == ESP_OK) s_state.fan_min_pct = v;
    uint8_t sv;
    if (nvs_get_u8(h, NVS_KEY_SOL_EN,  &sv) == ESP_OK) s_sol_en      = sv ? true : false;
    if (nvs_get_u8(h, NVS_KEY_SOL_ON,  &sv) == ESP_OK) s_sol_on_pct  = sv;
    if (nvs_get_u8(h, NVS_KEY_SOL_OFF, &sv) == ESP_OK) s_sol_off_pct = sv;
    nvs_close(h);
}

/* NVS save en tarea dedicada para no bloquear LVGL */
static void nvs_save_task(void *arg)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            nvs_set_blob(h, NVS_KEY_ASSIGN, s_state.assignment, FRIGO_MAX_SENSORS);
            nvs_set_blob(h, NVS_KEY_ROLEADDR, s_state.role_addr,
                         sizeof(s_state.role_addr));
            nvs_set_u8(h, NVS_KEY_TMIN, s_state.T_min);
            nvs_set_u8(h, NVS_KEY_TMAX, s_state.T_max);
            nvs_set_u8(h, NVS_KEY_FANMIN, s_state.fan_min_pct);
            nvs_set_u8(h, NVS_KEY_SOL_EN,  s_sol_en ? 1 : 0);
            nvs_set_u8(h, NVS_KEY_SOL_ON,  s_sol_on_pct);
            nvs_set_u8(h, NVS_KEY_SOL_OFF, s_sol_off_pct);
            xSemaphoreGive(s_mutex);
        }
        nvs_commit(h);
        nvs_close(h);
    }
    vTaskDelete(NULL);
}

static void nvs_save(void)
{
    if (xTaskCreate(nvs_save_task, "frigo_nvs", 3072, NULL, 3, NULL) != pdPASS)
        ESP_LOGW(TAG, "nvs_save: no pude crear la tarea, ajuste no persistido");
}

/* ── PWM ─────────────────────────────────────────────────────── */
/* Ultimo % efectivamente aplicado al LEDC (tras suelo/kickstart). Sirve para
 * detectar el paso de parado->girando y disparar el pulso de arranque. */
static uint8_t          s_last_applied = 0;
/* Kickstart no bloqueante: al arrancar de parado damos 100% durante
 * FRIGO_FAN_KICKSTART_MS y una callback de esp_timer baja al objetivo. No se
 * puede usar vTaskDelay porque fan_set_percent se llama con s_mutex tomado
 * (congelaria UI/HTTP, que leen el estado bajo el mismo mutex). */
static esp_timer_handle_t s_kick_timer = NULL;
static volatile uint8_t   s_kick_target = 0;

/* Duty crudo al LEDC (0-100% -> 0-1023 en 10 bits). */
static void fan_apply_duty(uint8_t pct)
{
    uint32_t duty = ((uint32_t)pct * 1023) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL);
}

/* Fin del pulso de arranque: baja del 100% al objetivo real. */
static void fan_kick_done_cb(void *arg)
{
    fan_apply_duty(s_kick_target);
}

static void fan_pwm_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER,
        .duty_resolution = LEDC_RESOLUTION,
        .freq_hz         = FRIGO_FAN_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .gpio_num   = FRIGO_FAN_GPIO,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL,
        .timer_sel  = LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ch);

    const esp_timer_create_args_t kick = {
        .callback = fan_kick_done_cb,
        .name     = "fan_kick",
    };
    if (esp_timer_create(&kick, &s_kick_timer) != ESP_OK) {
        ESP_LOGW(TAG, "kickstart timer no creado; arranque sin pulso");
        s_kick_timer = NULL;
    }
}

static void fan_set_percent(uint8_t pct)
{
    /* Suelo: con PWM sobre la alimentacion (MOSFET) por debajo de este % el
     * ventilador no llega a girar; lo forzamos al minimo util. Ajustable desde
     * la UI (s_state.fan_min_pct, persistido en NVS). 0 = sin suelo / apagado.
     * Llamada siempre con s_mutex tomado, asi que leer s_state es seguro. */
    uint8_t floor_pct = s_state.fan_min_pct;
    if (pct > 0 && pct < floor_pct) pct = floor_pct;

    if (s_kick_timer && s_last_applied == 0 && pct > 0) {
        /* Parado -> girando: pulso 100% y la callback baja al objetivo. */
        s_kick_target = pct;
        fan_apply_duty(100);
        esp_timer_stop(s_kick_timer);   /* por si venia armado */
        esp_timer_start_once(s_kick_timer,
                             (uint64_t)FRIGO_FAN_KICKSTART_MS * 1000);
    } else {
        /* Cambio en marcha o parada: si hay un kickstart pendiente, cancelarlo
         * para que no pise este valor. */
        if (s_kick_timer) esp_timer_stop(s_kick_timer);
        fan_apply_duty(pct);
    }
    s_last_applied = pct;
}

/* ── Lógica ventilador proporcional con histéresis ───────────── */
static uint8_t compute_fan(float t_aletas, uint8_t t_min, uint8_t t_max,
                            uint8_t fan_prev)
{
    if (t_aletas < -120.0f) return 0;

    float tmin = (float)t_min;
    float tmax = (float)t_max;

    if (t_aletas < tmin - FAN_HYST_DEG) return 0;
    if (t_aletas > tmax + FAN_HYST_DEG) return 100;

    if (t_aletas <= tmin) {
        if (fan_prev == 0) return 0;
        return 20;
    }

    float ratio = (t_aletas - tmin) / (tmax - tmin);
    uint8_t pct = (uint8_t)(20.0f + ratio * 80.0f);
    if (pct > 100) pct = 100;
    return pct;
}

/* Un tick de la maquina de estados del modo excedente solar. Se llama en cada
 * iteracion de frigo_task (~1-3 s; MIN_ON=30min tolera esa cadencia). */
static void frigo_solar_tick(void)
{
    bool relay;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    /* Frescura efectiva: main debe refrescar; si deja de hacerlo, no fresco.
     * Ventana de 10 s (antes 3 s): main alimenta cada 1 s desde la tarea
     * esp_timer COMPARTIDA, donde tambien corren los flush a SD y el auto-off
     * del portal. Un atasco puntual de esa tarea (fclose lento en la SD SPI)
     * bastaba para abrir el rele del frigo y obligar a rehacer todo el ciclo
     * SoC+PV+debounce. Esto NO relaja la comprobacion real de datos rancios:
     * la frescura de bateria/solar ya la valida dashboard_state con su propia
     * ventana de 30 s antes de llegar aqui (ver frigo_solar_feed_cb). */
    bool fresh = s_sol_fresh && ((uint32_t)(now - s_sol_feed_ms) < 10000u);
    frigo_solar_in_t in = {
        .enabled = s_sol_en, .soc_deci = s_sol_soc_deci, .pv_w = s_sol_pv_w,
        .shore = s_sol_shore, .fresh = fresh, .soc_on_pct = s_sol_on_pct,
        .soc_off_pct = s_sol_off_pct, .now_ms = now,
    };
    bool prev = s_sol_sm.active;
    relay = frigo_solar_eval(&in, &s_sol_sm);
    xSemaphoreGive(s_mutex);

    gpio_set_level(FRIGO_SOLAR_RELAY_GPIO, relay ? 1 : 0);
    if (relay != prev)
        ESP_LOGI(TAG, "Excedente solar: frigo 12V %s (SoC=%.1f%% PV=%dW)",
                 relay ? "ON" : "OFF", s_sol_soc_deci / 10.0f, s_sol_pv_w);
}

/* ── Tarea de lectura ────────────────────────────────────────── */
static void frigo_task(void *arg)
{
    while (1) {
        if (s_hb_cb) s_hb_cb();  /* latido watchdog */
        frigo_solar_tick();

        /* Modo simulacion (banco sin sondas): manda el sim, aqui NO tocamos
         * s_state para no pisar sus valores. El watchdog ya se alimento arriba. */
        if (s_sim_mode) { vTaskDelay(pdMS_TO_TICKS(1000)); continue; }

        /* Escaneo del bus: bajo peticion, o de fondo cada 5 min.
         * Va AQUI, antes del corte por "no hay sondas": si no, cuando el bus se
         * queda vacio nunca se volveria a mirar. El periodico usa el camino
         * barato (sin recrear el bus) para no molestar a un sistema que va bien;
         * el manual usa el de recuperacion, porque el usuario acaba de tocar el
         * cableado y lo normal es que el bus este en mal estado. */
        static uint32_t ultimo_scan_ms = 0;
        uint32_t ahora_ms = (uint32_t)(esp_timer_get_time() / 1000);
        if (s_rescan_req) {
            s_rescan_req = false;
            escanear_y_aplicar(true);
            ultimo_scan_ms = ahora_ms;
        } else if (ahora_ms - ultimo_scan_ms >= 5 * 60 * 1000) {
            escanear_y_aplicar(false);
            ultimo_scan_ms = ahora_ms;
        }

        /* Si no hay sensores DS18B20, igualmente atendemos los cambios de
         * modo OFF/50/100 (que no requieren temperatura) para que la UI
         * refleje correctamente el ventilador. Solo AUTO necesita sensor.
         * Antes, el continue prematuro impedia llamar al callback y la UI
         * (incluido el indicador LED del ventilador) no se actualizaba. */
        if (s_state.n_sensors == 0) {
            if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                s_state.T_Aletas     = -127.0f;
                s_state.T_Congelador = -127.0f;
                s_state.T_Exterior   = -127.0f;
                uint8_t pct;
                switch (s_state.mode) {
                    case FRIGO_MODE_OFF:  pct = 0;   break;
                    case FRIGO_MODE_50:   pct = 50;  break;
                    case FRIGO_MODE_100:  pct = 100; break;
                    case FRIGO_MODE_AUTO:
                    default:              pct = 0;   break;  /* sin temp -> 0 */
                }
                if (pct != s_state.fan_percent) {
                    s_state.fan_percent = pct;
                    fan_set_percent(pct);
                    ESP_LOGI(TAG, "Fan → %d%% (mode=%d, sin DS18B20)",
                             pct, (int)s_state.mode);
                }
                xSemaphoreGive(s_mutex);
            }
            if (s_cb) s_cb(&s_state);
            vTaskDelay(pdMS_TO_TICKS(1000));  /* poll modo a 1Hz */
            continue;
        }

        vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));

        float temps[FRIGO_MAX_SENSORS] = {-127.0f, -127.0f, -127.0f};

        /* Broadcast: disparar conversión en todos a la vez */
        /* Los handles pueden ser NULL: el bus enumero la sonda pero crear el
         * dispositivo DS18B20 fallo (ver escanear_y_aplicar). */
        for (int i = 0; i < s_state.n_sensors; i++)
            if (s_devs[i]) ds18b20_trigger_temperature_conversion(s_devs[i]);
        vTaskDelay(pdMS_TO_TICKS(DS18B20_CONV_MS));

        for (int i = 0; i < s_state.n_sensors; i++) {
            float t;
            if (s_devs[i] && ds18b20_get_temperature(s_devs[i], &t) == ESP_OK)
                temps[i] = t;
        }

        if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            #define TEMP_INVALID(t) ((t) < -120.0f || (t) > 125.0f)
            /* Un rol sin sonda vale FRIGO_SONDA_AUSENTE (0xFF) y desbordaria
             * temps[]. -127.0f es el "sin dato" que ya usa todo el componente. */
            #define TEMP_DE(slot) \
                (s_state.assignment[(slot)] < s_state.n_sensors \
                     ? temps[s_state.assignment[(slot)]] : -127.0f)
            float ta = TEMP_DE(FRIGO_SLOT_ALETAS);
            float tc = TEMP_DE(FRIGO_SLOT_CONGELADOR);
            float te = TEMP_DE(FRIGO_SLOT_EXTERIOR);
            #undef TEMP_DE
            s_state.T_Aletas     = TEMP_INVALID(ta) ? -127.0f : ta;
            s_state.T_Congelador = TEMP_INVALID(tc) ? -127.0f : tc;
            s_state.T_Exterior   = TEMP_INVALID(te) ? -127.0f : te;

            uint8_t pct;
            switch (s_state.mode) {
                case FRIGO_MODE_OFF:  pct = 0;   break;
                case FRIGO_MODE_50:   pct = 50;  break;
                case FRIGO_MODE_100:  pct = 100; break;
                case FRIGO_MODE_AUTO:
                default:
                    /* Sin sonda de aletas no se inventa una temperatura: mismo
                     * criterio que la rama "sin DS18B20" de mas arriba, el
                     * ventilador se para. */
                    pct = (s_state.T_Aletas <= -120.0f)
                            ? 0
                            : compute_fan(s_state.T_Aletas, s_state.T_min,
                                          s_state.T_max, s_state.fan_percent);
                    break;
            }
            if (pct != s_state.fan_percent) {
                s_state.fan_percent = pct;
                fan_set_percent(pct);
                ESP_LOGI(TAG, "Fan → %d%% (mode=%d) T_Aletas=%.1f T_min=%d T_max=%d",
                         pct, (int)s_state.mode, s_state.T_Aletas,
                         s_state.T_min, s_state.T_max);
            }
            xSemaphoreGive(s_mutex);
        }

        if (s_cb) s_cb(&s_state);
    }
    vTaskDelete(NULL);
}

/* ── Escaneo del bus 1-Wire ──────────────────────────────────── */

/* Recorre el bus y deja en 'out' las direcciones encontradas. Devuelve cuantas.
 *
 * 'recuperar' controla el coste: en el escaneo periodico se intenta primero por
 * las buenas (un reset sobre el bus que ya existe). Solo si eso falla se destruye
 * y recrea el bus, porque tras un timeout el canal RX del RMT queda en
 * INVALID_STATE y los resets siguientes sobre el mismo handle NO tocan el cable
 * (son un no-op silencioso). */
static int bus_enumerar(uint64_t out[FRIGO_MAX_SENSORS], bool recuperar)
{
    int n = 0;
    const int intentos = recuperar ? 5 : 1;

    for (int intento = 0; intento < intentos && n == 0; intento++) {
        /* Latido: un escaneo con el bus muerto tarda ~6 s (5 intentos x ~1,2 s
         * medidos). El vigilante reinicia la pantalla a los 10 s sin latido, y
         * el resto del ciclo se come otros ~2,8 s: sin esto quedaba ~1 s de
         * margen. En frigo_init aun no hay latido (s_hb_cb es NULL) y no hace
         * falta, porque la tarea vigilada todavia no existe. */
        if (s_hb_cb) s_hb_cb();

        if (intento > 0) {
            if (s_bus) onewire_bus_del(s_bus);   /* puede venir NULL de un fallo previo */
            s_bus = NULL;
            vTaskDelay(pdMS_TO_TICKS(200));
            if (onewire_new_bus_rmt(&s_bus_cfg, &s_rmt_cfg, &s_bus) != ESP_OK) {
                ESP_LOGE(TAG, "intento %d: recrear bus fallo", intento);
                s_bus = NULL;
                continue;
            }
        }
        if (!s_bus) continue;

        esp_err_t rr = onewire_bus_reset(s_bus);
        if (rr != ESP_OK) {
            /* DEBUG y no INFO: esto corre cada 5 min y llenaria el log de la SD. */
            ESP_LOGD(TAG, "intento %d: reset=%s", intento, esp_err_to_name(rr));
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        onewire_device_iter_handle_t iter;
        if (onewire_new_device_iter(s_bus, &iter) != ESP_OK) continue;
        onewire_device_t dev;
        while (onewire_device_iter_get_next(iter, &dev) == ESP_OK
               && n < FRIGO_MAX_SENSORS) {
            out[n++] = dev.address;
        }
        onewire_del_device_iter(iter);
    }
    return n;
}

/* Escanea y deja el estado coherente. Llamar SOLO desde frigo_task, o desde
 * frigo_init antes de arrancarla: toca s_devs y s_state. */
static void escanear_y_aplicar(bool recuperar)
{
    /* Soltar los dispositivos ANTES de tocar el bus. Dos motivos:
     *  - ds18b20_new_device_from_enumeration reserva memoria: si solo se
     *    sobreescribian los punteros, cada escaneo (uno cada 5 min) fugaba lo
     *    reservado en el anterior.
     *  - bus_enumerar puede destruir y recrear el bus, y entonces estos handles
     *    quedarian colgando; hay que soltarlos mientras el bus sigue vivo. */
    for (int i = 0; i < FRIGO_MAX_SENSORS; i++) {
        if (s_devs[i]) {
            ds18b20_del_device(s_devs[i]);
            s_devs[i] = NULL;
        }
    }

    uint64_t hay[FRIGO_MAX_SENSORS] = {0};
    int n = bus_enumerar(hay, recuperar);

    for (int i = 0; i < n; i++) {
        onewire_device_t dev = { .bus = s_bus, .address = hay[i] };
        ds18b20_config_t ds_cfg = {};
        if (ds18b20_new_device_from_enumeration(&dev, &ds_cfg, &s_devs[i]) != ESP_OK)
            s_devs[i] = NULL;
    }

    /* Primer arranque con esta version: anclar a las direcciones la asignacion
     * por posicion que ya tuviera guardada el usuario. */
    frigo_sondas_migrar(hay, n, s_state.assignment, s_state.role_addr);

    frigo_scan_result_t r = frigo_sondas_resolver(hay, n, s_state.role_addr);

    if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_state.n_sensors = (uint8_t)n;
    for (int i = 0; i < FRIGO_MAX_SENSORS; i++) {
        s_state.sensors[i].address = (i < n) ? hay[i] : 0;
        s_state.sensors[i].valid   = (i < n);
        s_state.assignment[i]      = r.asignacion[i];
    }
    s_state.scan_event |= frigo_sondas_evento(s_scan_flags, r.flags);
    s_scan_flags = r.flags;
    if (s_mutex) xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "%d sonda(s) en bus GPIO%d (flags=0x%02x)",
             n, FRIGO_ONEWIRE_GPIO, r.flags);
}

/* ── API pública ─────────────────────────────────────────────── */
esp_err_t frigo_init(frigo_update_cb_t cb)
{
    s_cb = cb;

    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex) {
        ESP_LOGE(TAG, "No se pudo crear mutex");
        return ESP_ERR_NO_MEM;
    }

    nvs_load();

    /* Rele piloto del modo excedente solar: salida, apagado al arrancar. */
    gpio_config_t sol_cfg = {
        .pin_bit_mask = 1ULL << FRIGO_SOLAR_RELAY_GPIO,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&sol_cfg);
    gpio_set_level(FRIGO_SOLAR_RELAY_GPIO, 0);

    ESP_RETURN_ON_ERROR(
        onewire_new_bus_rmt(&s_bus_cfg, &s_rmt_cfg, &s_bus),
        TAG, "onewire_new_bus_rmt falló");

    /* Primer escaneo por el camino de recuperacion: un DS18B20 puede no dar pulso
     * de presencia en el primer reset frio tras el power-on, y ahi hay que
     * destruir y recrear el bus entre intentos (ver bus_enumerar). */
    escanear_y_aplicar(true);

    /* NOTA: aqui habia un apaño que, ante un indice fuera de rango, reapuntaba el
     * rol a la sonda 0 EN SILENCIO -> se regulaba con la sonda equivocada sin
     * decir nada. Ya no hace falta: los roles van por direccion, y el que no tiene
     * sonda presente se queda en FRIGO_SONDA_AUSENTE. */

    fan_pwm_init();
    fan_set_percent(0);

    xTaskCreate(frigo_task, "frigo", 8192, NULL, 5, NULL);
    return ESP_OK;
}

void frigo_get_state_copy(frigo_state_t *out)
{
    if (!out) return;
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        *out = s_state;
        s_last_good = s_state;
        s_last_good_valid = true;
        xSemaphoreGive(s_mutex);
    } else if (s_last_good_valid) {
        /* mutex no disponible: devolver la ultima copia consistente en vez
         * de leer s_state desprotegida (podria estar a medio escribir). */
        *out = s_last_good;
    } else {
        /* aun no hay ninguna copia consistente (arranque muy temprano) */
        *out = s_state;
    }
}

void frigo_set_heartbeat_cb(frigo_heartbeat_cb_t cb) { s_hb_cb = cb; }

void frigo_sim_inject(float t_aletas, float t_congelador,
                      float t_exterior, uint8_t fan_percent)
{
    s_sim_mode = true;   /* a partir de ahora manda el sim: frigo_task no pisa */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_state.T_Aletas     = t_aletas;
        s_state.T_Congelador = t_congelador;
        s_state.T_Exterior   = t_exterior;
        s_state.fan_percent  = fan_percent;
        xSemaphoreGive(s_mutex);
    }
    if (s_cb) s_cb(&s_state);
}

esp_err_t frigo_set_assignment(frigo_slot_t slot, uint8_t sensor_idx)
{
    if (slot >= FRIGO_MAX_SENSORS || sensor_idx >= s_state.n_sensors)
        return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_state.assignment[slot] = sensor_idx;
        /* Anclar el rol a la sonda FISICA elegida: es role_addr lo que sobrevive
         * a un reescaneo, assignment se recalcula a partir de el. */
        s_state.role_addr[slot] = s_state.sensors[sensor_idx].address;
        /* El usuario acaba de decidir, asi que la situacion ya no es "hay una
         * sonda sin rol": recalcular las banderas para no volver a avisar. */
        uint64_t hay[FRIGO_MAX_SENSORS] = {0};
        for (int i = 0; i < s_state.n_sensors; i++)
            hay[i] = s_state.sensors[i].address;
        s_scan_flags = frigo_sondas_resolver(hay, s_state.n_sensors,
                                             s_state.role_addr).flags;
        xSemaphoreGive(s_mutex);
    }
    nvs_save();
    return ESP_OK;
}

void frigo_set_mode(frigo_mode_t m)
{
    if (m > FRIGO_MODE_100) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    s_state.mode = m;
    xSemaphoreGive(s_mutex);
    /* No persistir: por decision de diseno cada reset arranca en AUTO. */
}

esp_err_t frigo_set_thresholds(uint8_t t_min, uint8_t t_max)
{
    if (t_min < 30 || t_max > 60 || t_min >= t_max) return ESP_ERR_INVALID_ARG;
    if (t_min % 5 != 0 || t_max % 5 != 0)           return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_state.T_min = t_min;
        s_state.T_max = t_max;
        xSemaphoreGive(s_mutex);
    }
    nvs_save();
    return ESP_OK;
}

esp_err_t frigo_set_fan_min(uint8_t pct)
{
    if (pct > 60)      return ESP_ERR_INVALID_ARG;   /* techo de cordura */
    if (pct % 5 != 0)  return ESP_ERR_INVALID_ARG;   /* pasos de 5 como los umbrales */
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        s_state.fan_min_pct = pct;
        xSemaphoreGive(s_mutex);
    }
    nvs_save();
    return ESP_OK;
}

void frigo_addr_to_str(const frigo_sensor_addr_t *sensor, char *buf, size_t len)
{
    uint64_t a = sensor->address;
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X",
             (uint8_t)(a>>56), (uint8_t)(a>>48), (uint8_t)(a>>40), (uint8_t)(a>>32),
             (uint8_t)(a>>24), (uint8_t)(a>>16), (uint8_t)(a>>8),  (uint8_t)(a));
}

void frigo_solar_feed(uint16_t soc_deci, uint16_t pv_w, bool shore, bool fresh)
{
    if (!s_mutex) return;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return;
    s_sol_soc_deci = soc_deci;
    s_sol_pv_w     = pv_w;
    s_sol_shore    = shore;
    s_sol_fresh    = fresh;
    s_sol_feed_ms  = (uint32_t)(esp_timer_get_time() / 1000);
    xSemaphoreGive(s_mutex);
}

esp_err_t frigo_solar_set_enabled(bool on)
{
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    s_sol_en = on;
    xSemaphoreGive(s_mutex);
    nvs_save();
    return ESP_OK;
}

esp_err_t frigo_solar_set_soc_on(uint8_t pct)
{
    if (pct < 80)  pct = 80;
    if (pct > 100) pct = 100;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    s_sol_on_pct = pct;
    if (s_sol_off_pct > (uint8_t)(pct - 5)) s_sol_off_pct = pct - 5;  /* mantener histeresis */
    xSemaphoreGive(s_mutex);
    nvs_save();
    return ESP_OK;
}

esp_err_t frigo_solar_set_soc_off(uint8_t pct)
{
    if (pct < 50) pct = 50;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(100)) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (pct > (uint8_t)(s_sol_on_pct - 5)) pct = s_sol_on_pct - 5;
    s_sol_off_pct = pct;
    xSemaphoreGive(s_mutex);
    nvs_save();
    return ESP_OK;
}

bool frigo_solar_get_enabled(void)
{
    if (!s_mutex) return false;
    bool v = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        v = s_sol_en;
        xSemaphoreGive(s_mutex);
    }
    return v;
}

uint8_t frigo_solar_get_soc_on(void)
{
    if (!s_mutex) return 95;
    uint8_t v = 95;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        v = s_sol_on_pct;
        xSemaphoreGive(s_mutex);
    }
    return v;
}

uint8_t frigo_solar_get_soc_off(void)
{
    if (!s_mutex) return 80;
    uint8_t v = 80;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        v = s_sol_off_pct;
        xSemaphoreGive(s_mutex);
    }
    return v;
}

bool frigo_solar_get_active(void)
{
    if (!s_mutex) return false;
    bool v = false;
    if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        v = s_sol_sm.active;
        xSemaphoreGive(s_mutex);
    }
    return v;
}

/* ── Escaneo bajo peticion ───────────────────────────────────── */

void frigo_request_rescan(void)
{
    /* Solo una bandera: el escaneo real lo hace frigo_task. Tocar el bus 1-Wire
     * desde el hilo de la UI se pisaria con la lectura de temperaturas. */
    s_rescan_req = true;
}

void frigo_ack_scan_event(void)
{
    if (s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_state.scan_event = 0;
        xSemaphoreGive(s_mutex);
    }
}
