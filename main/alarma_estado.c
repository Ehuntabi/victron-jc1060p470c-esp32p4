/* alarma_estado.c - Estado de las cuatro alarmas y su pitido. Ver el .h.
 *
 * Aqui vive lo que antes estaba repartido por view_overview.c: la temporizacion
 * de las condiciones (los rebotes de agua y grises, y la subida del
 * congelador), el silencio por alarma, y el pitido de 5 s cada 5 minutos.
 */
#include "alarma_estado.h"
#include "net/mini_proto.h"      /* MINI_ALARM_*, el bitmask que viaja */
#include "ui.h"                  /* ui_get_freezer_alarm() */
#include "data/dashboard_state.h" /* SoC y su frescura (alarma de bateria) */
#include "frigo.h"
#include "audio_es8311.h"
#include "alerts.h"
#include "ne185/ne185.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "fonts/fonts_es.h"        /* lv_font_montserrat_20_es: la fuente del aviso */
#include <string.h>
#include <stdio.h>

static const char *TAG = "alarma";

/* ── Estado compartido ──────────────────────────────────────────────────────
 * Un struct por alarma con los tres booleanos de su vida. Los escribe el
 * temporizador de evaluacion; los leen la pantalla (tarea LVGL), el emisor de
 * telemetria (tarea udp_tx) y el portal (tarea del httpd). Asignaciones de un
 * byte, sin cerrojo y sin campo de 32 bits que se pueda leer a medias. */
typedef struct {
    volatile bool activa;      /* la condicion se cumple (suene o no) */
    volatile bool silenciada;  /* el usuario la ha callado */
    /* Cuando empezo a cumplirse la condicion, para los rebotes. 0 = no se
     * cumple o todavia no ha llegado a alarma. */
    uint32_t      desde_ms;
    /* Ultimo pitido, para el intervalo de 5 min. 0 = no ha sonado todavia. */
    uint32_t      ultimo_pitido_ms;
} alarma_est_t;

static alarma_est_t s_al[ALARMA_CUANTAS];

/* Mascara de bits del protocolo, en el mismo orden que el enum. */
static const uint8_t s_bit[ALARMA_CUANTAS] = {
    MINI_ALARM_AGUA, MINI_ALARM_GRISES, MINI_ALARM_BATERIA, MINI_ALARM_CONGELADOR
};

static const char *const s_nombre[ALARMA_CUANTAS] = {
    "agua", "grises", "bateria", "congelador"
};

/* ── Pitido ─────────────────────────────────────────────────────────────────
 * El patron es el que ya habia (triple pitido, estilo detector de humos): es el
 * sonido que se reconoce como ALARMA sin pensarlo. */
static const audio_note_t s_patron[] = {
    /* Triple pitido #1 */
    {2700, 120}, {0, 80},
    {2700, 120}, {0, 80},
    {2700, 120}, {0, 700},

    /* Triple pitido #2 */
    {2700, 120}, {0, 80},
    {2700, 120}, {0, 80},
    {2700, 120}, {0, 700},

    /* Triple pitido #3 */
    {2700, 120}, {0, 80},
    {2700, 120}, {0, 80},
    {2700, 120}, {0, 700},

    /* Triple pitido final */
    {2700, 120}, {0, 80},
    {2700, 120}, {0, 80},
    {2700, 120},
};

static QueueHandle_t s_cola_pitido = NULL;

static void alarma_task(void *arg)
{
    (void)arg;
    uint8_t v;
    for (;;) {
        if (xQueueReceive(s_cola_pitido, &v, portMAX_DELAY) == pdTRUE) {
            /* Subir al maximo solo durante el pitido y restaurar despues, para
             * no tocar el volumen normal del usuario. */
            int prev_vol = audio_get_volume();
            audio_set_volume_transient(100);
            audio_play_alarm_tones(s_patron,
                                   sizeof(s_patron) / sizeof(s_patron[0]),
                                   true);
            audio_set_volume_transient(prev_vol);
        }
    }
}

#define PITIDO_INTERVALO_MS  (5u * 60u * 1000u)   /* 5 s de pitido cada 5 min */

static void evaluar(const alarma_tipo_t t, const bool condicion, const uint32_t ahora_ms)
{
    alarma_est_t *a = &s_al[t];

    if (!condicion) {
        /* Al recuperarse la alarma el silencio SE REARMA: si vuelve a pasar,
         * vuelve a sonar. Sin esto se quedaba muda para siempre, que es justo
         * lo que no puede pasar con una alarma.
         *
         * El rearme va AQUI y no en el flanco de bajada, y eso es a proposito:
         * el estado se rearma solo mientras la condicion NO se cumple, asi que
         * no puede envenenar a las alarmas que nunca han estado activas (el
         * fallo que se arreglo auditando el 13-sep-2026). */
        /* Al recuperarse, cortar el pitido en curso: si estaba activa, el
         * patron seguia sonando hasta ~4,4 s despues de recuperada. */
        bool estaba_activa = a->activa;
        if (estaba_activa) {
            audio_cancel_playback();
        }
        a->activa     = false;
        a->silenciada = false;
        a->desde_ms   = 0;
        a->ultimo_pitido_ms = 0;
        return;
    }

    if (!a->activa) {
        /* Primer aviso de la condicion: se enciende ya la senal visual (eso es
         * lo que pinta la pantalla leyendo 'activa'), aunque el pitido todavia
         * espere a su temporizacion. La alarma de verdad -- el pitido -- la
         * decide quien llame, con su propio tiempo. */
        a->activa = true;
        ESP_LOGW(TAG, "alarma de %s activa", s_nombre[t]);
    }

    if (!a->silenciada && s_cola_pitido) {
        if (a->ultimo_pitido_ms == 0 ||
            (ahora_ms - a->ultimo_pitido_ms) >= PITIDO_INTERVALO_MS) {
            a->ultimo_pitido_ms = ahora_ms;
            uint8_t v = 1;
            xQueueSend(s_cola_pitido, &v, 0);
        }
    } else {
        /* Silenciada (o sin cola): el reloj del proximo pitido se reinicia, de
         * modo que al volver a habilitar el sonido pita ENSEGUIDA en vez de
         * esperar a que se cumplan los 5 minutos. */
        a->ultimo_pitido_ms = 0;
    }
}

/* Tiempo que tiene que mantenerse una condicion antes de considerarse alarma.
 * El agua chapotea en marcha y el sensor lee "vacio" un instante; sin esto
 * sonaba en cada curva. El congelador ademas tiene que estar SUBIENDO ese rato
 * (ver main.c::frigo_update_cb, que es quien lleva esa cuenta). */
#define REBOTE_AGUA_MS     (60u * 1000u)
#define REBOTE_GRISES_MS   (60u * 1000u)

static void evaluar_todo(uint32_t ahora_ms)
{
    /* ── Aguas (NE185 por RS-485) ─────────────────────────────────────────── */
    ne185_data_t cd;
    ne185_get(&cd);
    /* Limpio en reserva: nivel 0 sostenido un minuto. */
    bool raw_agua = cd.fresh && cd.s1 == 0;
    if (!raw_agua || s_al[ALARMA_AGUA].desde_ms == 0) {
        if (raw_agua) s_al[ALARMA_AGUA].desde_ms = ahora_ms ? ahora_ms : 1;
        else          s_al[ALARMA_AGUA].desde_ms = 0;
    }
    bool agua = raw_agua && (ahora_ms - s_al[ALARMA_AGUA].desde_ms) >= REBOTE_AGUA_MS;

    /* Grises lleno (NE185 real: 0=vacio, 1=lleno), mismo rebote de un minuto. */
    bool raw_grises = cd.fresh && cd.r1 == 1;
    if (!raw_grises || s_al[ALARMA_GRISES].desde_ms == 0) {
        if (raw_grises) s_al[ALARMA_GRISES].desde_ms = ahora_ms ? ahora_ms : 1;
        else            s_al[ALARMA_GRISES].desde_ms = 0;
    }
    bool grises = raw_grises && (ahora_ms - s_al[ALARMA_GRISES].desde_ms) >= REBOTE_GRISES_MS;

    /* ── Bateria: umbral critico configurable (NVS, por defecto 30 %) ─────── */
    /* El dato tiene que ser FRESCO. Antes esto se miraba con 'has_data', que se
     * pone a true la primera vez que llega el BatteryMonitor y NUNCA se
     * resetea: si el BLE se caia con el SoC ya por debajo del umbral, el pitido
     * y el parpadeo seguian para siempre con el ultimo dato congelado. Visto
     * auditando el 08-sep-2026 y arreglado en la vista; al traerlo aqui se
     * conserva el criterio. */
    dashboard_snapshot_t snap;
    dashboard_state_snapshot(&snap);
    bool bateria = snap.bat_fresh && snap.soc_deci < (uint16_t)(alerts_get_soc_critical() * 10);

    /* ── Congelador: quien lleva la cuenta es main.c (ui_get_freezer_alarm) ──
     * Ahi esta la deteccion con su subida sostenida y sus minutos, que necesitan
     * el criterio robusto del frigo; aqui solo se lee el resultado. */
    bool congelador = ui_get_freezer_alarm();

    evaluar(ALARMA_AGUA,       agua,       ahora_ms);
    evaluar(ALARMA_GRISES,     grises,     ahora_ms);
    evaluar(ALARMA_BATERIA,    bateria,    ahora_ms);
    evaluar(ALARMA_CONGELADOR, congelador, ahora_ms);
}

/* ── Aviso flotante de alarma, en lv_layer_top() ─────────────────────────────
 * Antes esto vivia DENTRO de overview_render(): el texto solo se refrescaba
 * cuando la vista Overview se dibujaba, y su tick se salta cuando el root esta
 * oculto o el salvapantallas rota. Resultado (auditoria): con el usuario en
 * Ajustes y sin trafico BLE que forzara renders, la alarma pitaba sin que se
 * viera NADA en pantalla. Desde aqui lo hace el propio timer de 500 ms, asi
 * que el aviso se ve desde cualquier pantalla. Mismo diseno y posicion que el
 * original: fuente 20_es, texto ambar sobre negro, arriba al centro (no tapa
 * la barra inferior) y NO clickable (flotando por encima de todo, un
 * rectangulo pulsable robaria toques a lo que hay debajo). Solo informa: dice
 * cual es la alarma y que hay que tocar para callarla. El parpadeo de las
 * tarjetas sigue en la vista Overview. */
static lv_obj_t *s_aviso = NULL;
static char      s_aviso_txt[64];   /* ultimo texto puesto: no rehacerlo cada tick */

/* Texto del aviso: la primera alarma activa, o NULL si no hay ninguna. Usa el
 * estado local s_al[] (recien evaluado en este mismo tick), igual que el
 * resto de consumidores. */
static const char *alarm_hint_text(void)
{
    /* Se enseña SIEMPRE que la alarma este activa, silenciada o no: silenciar
     * corta el sonido, no la señal visual (decidido el 13-sep-2026). Si esta
     * silenciada, el texto lo dice, para que se sepa que sigue pasando. */
    if (alarma_activa(ALARMA_AGUA))  return alarma_silenciada(ALARMA_AGUA)  ? "Agua limpia en reserva (silenciada)" : "Agua limpia en reserva: toca el deposito";
    if (alarma_activa(ALARMA_GRISES)) return alarma_silenciada(ALARMA_GRISES) ? "Aguas grises llenas (silenciada)" : "Aguas grises llenas: toca el deposito";
    if (alarma_activa(ALARMA_BATERIA)) return alarma_silenciada(ALARMA_BATERIA) ? "Bateria baja (silenciada)" : "Bateria baja: toca la bateria";
    if (alarma_activa(ALARMA_CONGELADOR)) return alarma_silenciada(ALARMA_CONGELADOR) ? "Congelador fuera de temperatura (silenciada)" : "Congelador fuera de temperatura: toca su temperatura";
    return NULL;
}

/* Creacion perezosa del label: la primera vez que hace falta. Estilos y
 * posicion copiados del aviso que existia en view_overview.c. */
static void aviso_crear(void)
{
    if (s_aviso) return;
    s_aviso = lv_label_create(lv_layer_top());
    lv_obj_add_flag(s_aviso, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(s_aviso, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_aviso, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(s_aviso, &lv_font_montserrat_20_es, 0);
    lv_obj_set_style_text_color(s_aviso, lv_color_hex(0xFFD54F), 0);
    lv_obj_set_style_bg_color(s_aviso, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_aviso, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(s_aviso, 8, 0);
    lv_obj_set_style_radius(s_aviso, 8, 0);
    lv_obj_align(s_aviso, LV_ALIGN_TOP_MID, 0, 8);   /* arriba: no tapa los botones de abajo */
}

static bool s_prev_any_alarm = false;

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    evaluar_todo((uint32_t)lv_tick_get());

    /* Flanco ascendente de "alguna alarma activa" (silenciada o no: la pantalla
     * tiene que enseñarla, lo que se calla es el pitido): si el salvapantallas
     * esta rotando, se interrumpe y salta a Live+Overview para que la alarma
     * sea visible. Antes lo hacia overview_render(); al estar aqui vale desde
     * cualquier pantalla. */
    bool any = alarma_estado_bits() != 0;
    if (any && !s_prev_any_alarm) {
        ui_alarm_interrupt_screensaver();
    }
    s_prev_any_alarm = any;

    /* Aviso flotante: pintarlo/ocultarlo y cambiar el texto solo cuando cambie
     * de verdad (lv_label_set_text no compara y este tick corre 2 veces por
     * segundo). Se muestra aunque la alarma este SILENCIADA y se oculta cuando
     * no queda ninguna activa. Todo corre en la tarea LVGL (es un lv_timer). */
    const char *cual = alarm_hint_text();
    const char *txt = cual ? cual : "";
    if (strcmp(txt, s_aviso_txt) != 0) {
        snprintf(s_aviso_txt, sizeof(s_aviso_txt), "%s", txt);
        if (cual) {
            aviso_crear();
            lv_label_set_text(s_aviso, cual);
        }
    }
    if (cual) {
        if (s_aviso) lv_obj_clear_flag(s_aviso, LV_OBJ_FLAG_HIDDEN);
    } else if (s_aviso) {
        lv_obj_add_flag(s_aviso, LV_OBJ_FLAG_HIDDEN);
    }
}

void alarma_estado_init(void)
{
    if (s_cola_pitido) return;   /* ya arrancado */
    s_cola_pitido = xQueueCreate(1, sizeof(uint8_t));
    if (!s_cola_pitido) {
        ESP_LOGE(TAG, "sin cola para el pitido: las alarmas no sonaran");
        return;
    }
    if (xTaskCreate(alarma_task, "alarma", 3072, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "no puedo crear la tarea del pitido");
        vQueueDelete(s_cola_pitido);
        s_cola_pitido = NULL;
        return;
    }
    /* 500 ms: el mismo paso que usa la pantalla para parpadear, y de sobra para
     * un rebote de un minuto. Este temporizador NO depende de ninguna vista. */
    lv_timer_create(tick_cb, 500, NULL);
    ESP_LOGI(TAG, "estado de alarmas propio (500 ms), independiente de la vista");
}

bool alarma_activa(alarma_tipo_t t)
{
    if (t < 0 || t >= ALARMA_CUANTAS) return false;
    return s_al[t].activa;
}

bool alarma_silenciada(alarma_tipo_t t)
{
    if (t < 0 || t >= ALARMA_CUANTAS) return false;
    return s_al[t].silenciada;
}

void alarma_silenciar(alarma_tipo_t t)
{
    if (t < 0 || t >= ALARMA_CUANTAS) return;
    if (s_al[t].silenciada) return;
    s_al[t].silenciada = true;
    ESP_LOGI(TAG, "alarma de %s silenciada (la senal visual sigue)", s_nombre[t]);

    /* El corte del pitido va AQUI, en la tarea que pide el silencio, y no en la
     * tarea del pitido. Parece mas ordenado pasarlo por una cola -- y se hizo
     * asi en la primera version -- pero entonces el corte solo se atendia
     * CUANDO TERMINABA la reproduccion en curso: el pitido sonaba 5 segundos
     * enteros aunque el usuario (o la cabina) lo hubiera callado al primer
     * segundo. Justo el caso que se quiere resolver: se toca porque se oye.
     *
     * Llamarlo desde otra tarea es seguro POR DISEÑO: audio_cancel_playback()
     * solo incrementa un contador de generacion (s_audio_gen) y el bucle de
     * reproduccion lo comprueba en cada trozo, asi que aborta en milisegundos.
     * Ese contador existe precisamente para esto (ver el comentario de
     * audio_es8311.c sobre el caso del 09-sep-2026). Auditado el 14-sep-2026. */
    audio_cancel_playback();
}

void alarma_alternar_silencio(alarma_tipo_t t)
{
    if (t < 0 || t >= ALARMA_CUANTAS) return;
    if (s_al[t].silenciada) {
        s_al[t].silenciada = false;
        s_al[t].ultimo_pitido_ms = 0;   /* vuelve a sonar ya, no en 5 min */
        ESP_LOGI(TAG, "alarma de %s vuelve a sonar", s_nombre[t]);
    } else {
        alarma_silenciar(t);
    }
}

uint8_t alarma_estado_bits(void)
{
    uint8_t bits = 0;
    for (int i = 0; i < ALARMA_CUANTAS; i++) {
        if (s_al[i].activa) bits |= s_bit[i];
    }
    return bits;
}

/* API conservada para el salvapantallas (settings_panel.c): antes la
 * actualizaba overview_render() con sus propios estados locales
 * (s_ov_alarm_active); ahora es una vista de la mascara del estado
 * compartido, que este fichero ya mantiene al dia en cada tick. */
bool ui_overview_alarm_active(void)
{
    return alarma_estado_bits() != 0;
}

int alarma_silenciar_mask(uint8_t mask)
{
    int n = 0;
    for (int i = 0; i < ALARMA_CUANTAS; i++) {
        if (!(mask & s_bit[i])) continue;
        if (!s_al[i].activa)    continue;   /* no habia nada que silenciar */
        if (s_al[i].silenciada) continue;
        alarma_silenciar((alarma_tipo_t)i);
        n++;
    }
    ESP_LOGI(TAG, "orden de silencio mask=0x%02x: %d alarma(s) callada(s); "
                  "activas ahora 0x%02x", mask, n, alarma_estado_bits());
    return n;
}

const char *alarma_nombre(alarma_tipo_t t)
{
    if (t < 0 || t >= ALARMA_CUANTAS) return "?";
    return s_nombre[t];
}
