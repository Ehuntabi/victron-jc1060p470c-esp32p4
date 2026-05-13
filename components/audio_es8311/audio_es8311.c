#include "audio_es8311.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <stdbool.h>
#include <math.h>
#include <string.h>

static const char *TAG = "audio_es8311";

/* Pines segun esquematico JC1060P470C_I */
#define I2S_NUM         I2S_NUM_0
#define I2S_MCK         GPIO_NUM_13
#define I2S_BCK         GPIO_NUM_12
#define I2S_LRCK        GPIO_NUM_10
#define I2S_DOUT        GPIO_NUM_9
#define PA_CTRL         GPIO_NUM_11

#define ES8311_ADDR     0x18
#define SAMPLE_RATE     16000

static i2s_chan_handle_t          s_tx_chan = NULL;
static esp_codec_dev_handle_t     s_codec   = NULL;


static int  s_volume = 50;     /* 0..100 */
static bool s_muted  = false;
/* Generation counter para cancelacion: cada llamada a audio_beep /
 * audio_play_tones captura su propia generacion al entrar y aborta su
 * loop cuando audio_cancel_playback la incrementa. Asi una llamada nueva
 * no resetea la cancelacion de otra anterior aun en curso. */
static volatile uint32_t s_audio_gen = 0;

static void load_audio_prefs(void)
{
    nvs_handle_t h;
    if (nvs_open("audio", NVS_READONLY, &h) == ESP_OK) {
        int8_t vol = 50; uint8_t mute = 0;
        nvs_get_i8(h, "vol", &vol);
        nvs_get_u8(h, "mute", &mute);
        if (vol < 0) vol = 0;
        if (vol > 100) vol = 100;
        s_volume = vol;
        s_muted  = mute ? true : false;
        nvs_close(h);
    }
}

static void save_audio_prefs(void)
{
    nvs_handle_t h;
    if (nvs_open("audio", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i8(h, "vol", (int8_t)s_volume);
        nvs_set_u8(h, "mute", s_muted ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

esp_err_t audio_set_volume(int vol)
{
    if (vol < 0) vol = 0;
    if (vol > 100) vol = 100;
    s_volume = vol;
    save_audio_prefs();
    if (s_codec) esp_codec_dev_set_out_vol(s_codec, vol);
    return ESP_OK;
}

int audio_get_volume(void) { return s_volume; }

esp_err_t audio_set_mute(bool mute)
{
    s_muted = mute;
    save_audio_prefs();
    if (s_codec) esp_codec_dev_set_out_mute(s_codec, mute);
    return ESP_OK;
}

bool audio_is_muted(void) { return s_muted; }

esp_err_t audio_init(i2c_master_bus_handle_t bus)
{

    /* 1. Configurar I2S TX */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
    esp_err_t ret = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(ret)); return ret; }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_MCK,
            .bclk = I2S_BCK,
            .ws   = I2S_LRCK,
            .dout = I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { 0 },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    ret = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "i2s_init_std: %s", esp_err_to_name(ret)); return ret; }

    ret = i2s_channel_enable(s_tx_chan);
    if (ret != ESP_OK) { ESP_LOGE(TAG, "i2s_enable: %s", esp_err_to_name(ret)); return ret; }

    /* 2. Configurar PA por GPIO53 (NS4150) */
    gpio_config_t pa_cfg = {
        .pin_bit_mask = (1ULL << PA_CTRL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = 0, .pull_down_en = 0, .intr_type = 0,
    };
    gpio_config(&pa_cfg);
    gpio_set_level(PA_CTRL, 1);

    /* 3. Crear interfaz audio_codec_data (I2S) */
    audio_codec_i2s_cfg_t i2s_data_cfg = {
        .port = I2S_NUM,
        .tx_handle = s_tx_chan,
        .rx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_data_cfg);
    if (!data_if) { ESP_LOGE(TAG, "new_i2s_data fallo"); return ESP_FAIL; }

    /* 4. Crear interfaz audio_codec_ctrl (I2C) */
    audio_codec_i2c_cfg_t i2c_ctrl_cfg = {
        .port = 0,
        .addr = ES8311_ADDR << 1, /* lib espera formato 8-bit, lo desplaza internamente */
        .bus_handle = bus,
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_ctrl_cfg);
    if (!ctrl_if) { ESP_LOGE(TAG, "new_i2c_ctrl fallo"); return ESP_FAIL; }

    /* 5. Crear codec ES8311 (no GPIO if porque PA va manual) */
    es8311_codec_cfg_t es_cfg = {
        .codec_mode  = ESP_CODEC_DEV_WORK_MODE_DAC,
        .ctrl_if     = ctrl_if,
        .gpio_if     = NULL,
        .pa_pin      = -1,                /* manejamos PA manualmente */
        .pa_reverted = false,
        .master_mode = false,             /* slave I2S */
        .use_mclk    = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain     = { 0 },
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) { ESP_LOGE(TAG, "es8311_codec_new fallo"); return ESP_FAIL; }

    /* 6. Crear handle final */
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = codec_if,
        .data_if  = data_if,
    };
    s_codec = esp_codec_dev_new(&dev_cfg);
    if (!s_codec) { ESP_LOGE(TAG, "esp_codec_dev_new fallo"); return ESP_FAIL; }

    /* 7. Abrir codec con sample rate y volumen */
    esp_codec_dev_sample_info_t fs = {
        .sample_rate     = SAMPLE_RATE,
        .channel         = 2,
        .bits_per_sample = 16,
    };
    if (esp_codec_dev_open(s_codec, &fs) != ESP_OK) {
        ESP_LOGE(TAG, "esp_codec_dev_open fallo");
        return ESP_FAIL;
    }
    load_audio_prefs();
    esp_codec_dev_set_out_vol(s_codec, s_volume);
    if (s_muted) esp_codec_dev_set_out_mute(s_codec, true);

    ESP_LOGI(TAG, "Audio inicializado (sr=%d Hz)", SAMPLE_RATE);
    return ESP_OK;
}

esp_err_t audio_beep(int freq_hz, int duration_ms)
{
    if (!s_codec) return ESP_ERR_INVALID_STATE;
    if (s_muted) return ESP_OK;
    uint32_t my_gen = s_audio_gen;
    /* Asegurar PA encendido */
    gpio_set_level(PA_CTRL, 1);
    if (freq_hz < 50) freq_hz = 50;
    if (freq_hz > 8000) freq_hz = 8000;
    if (duration_ms < 10) duration_ms = 10;
    if (duration_ms > 5000) duration_ms = 5000;

    const int chunk_ms = 100;
    const size_t samples_per_chunk = (SAMPLE_RATE * chunk_ms) / 1000;
    size_t buf_bytes = samples_per_chunk * 2 * sizeof(int16_t);
    int16_t *buf = malloc(buf_bytes);
    if (!buf) return ESP_ERR_NO_MEM;

    float phase = 0.0f;
    float step  = 2.0f * (float)M_PI * (float)freq_hz / (float)SAMPLE_RATE;
    int chunks_total = (duration_ms + chunk_ms - 1) / chunk_ms;
    int amp = 2000;

    for (int c = 0; c < chunks_total; ++c) {
        if (s_audio_gen != my_gen) break;   /* cancelacion instantanea */
        for (size_t i = 0; i < samples_per_chunk; ++i) {
            int16_t s = (int16_t)(amp * sinf(phase));
            phase += step;
            if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
            buf[i*2 + 0] = s;
            buf[i*2 + 1] = s;
        }
        if (c == 0) {
            for (size_t i = 0; i < samples_per_chunk; ++i) {
                float k = (float)i / (float)samples_per_chunk;
                buf[i*2 + 0] = (int16_t)(buf[i*2 + 0] * k);
                buf[i*2 + 1] = (int16_t)(buf[i*2 + 1] * k);
            }
        }
        if (c == chunks_total - 1) {
            for (size_t i = 0; i < samples_per_chunk; ++i) {
                float k = 1.0f - (float)i / (float)samples_per_chunk;
                buf[i*2 + 0] = (int16_t)(buf[i*2 + 0] * k);
                buf[i*2 + 1] = (int16_t)(buf[i*2 + 1] * k);
            }
        }
        esp_codec_dev_write(s_codec, buf, buf_bytes);
    }
    /* Enviar un buffer de silencio para vaciar el DMA. Si fue cancelado
     * solo metemos 1 buffer en lugar de 4 para minimizar la latencia. */
    memset(buf, 0, buf_bytes);
    int silence_chunks = (s_audio_gen != my_gen) ? 1 : 4;
    for (int i = 0; i < silence_chunks; ++i) {
        esp_codec_dev_write(s_codec, buf, buf_bytes);
    }
    /* Apagar el PA para no consumir corriente */
    gpio_set_level(PA_CTRL, 0);
    free(buf);
    return ESP_OK;
}

esp_err_t audio_play_tones(const audio_note_t *notes, size_t count)
{
    if (!s_codec) return ESP_ERR_INVALID_STATE;
    if (s_muted) return ESP_OK;
    if (!notes || count == 0) return ESP_ERR_INVALID_ARG;
    uint32_t my_gen = s_audio_gen;

    /* Asegurar PA encendido durante toda la secuencia */
    gpio_set_level(PA_CTRL, 1);

    const int chunk_ms = 50;
    const size_t samples_per_chunk = (SAMPLE_RATE * chunk_ms) / 1000;
    size_t buf_bytes = samples_per_chunk * 2 * sizeof(int16_t);
    int16_t *buf = malloc(buf_bytes);
    if (!buf) {
        gpio_set_level(PA_CTRL, 0);
        return ESP_ERR_NO_MEM;
    }

    for (size_t n = 0; n < count; ++n) {
        if (s_audio_gen != my_gen) break;
        int freq = notes[n].freq_hz;
        int dur  = notes[n].duration_ms;
        if (dur < 5) dur = 5;
        if (dur > 5000) dur = 5000;
        int chunks = (dur + chunk_ms - 1) / chunk_ms;

        if (freq == 0) {
            /* Silencio */
            memset(buf, 0, buf_bytes);
            for (int c = 0; c < chunks; ++c) {
                if (s_audio_gen != my_gen) break;
                esp_codec_dev_write(s_codec, buf, buf_bytes);
            }
            continue;
        }

        if (freq < 50) freq = 50;
        if (freq > 8000) freq = 8000;

        float phase = 0.0f;
        float step  = 2.0f * (float)M_PI * (float)freq / (float)SAMPLE_RATE;
        int amp = 2500; /* moderado para evitar brownout */

        for (int c = 0; c < chunks; ++c) {
            if (s_audio_gen != my_gen) break;
            for (size_t i = 0; i < samples_per_chunk; ++i) {
                int16_t s = (int16_t)(amp * sinf(phase));
                phase += step;
                if (phase > 2.0f * (float)M_PI) phase -= 2.0f * (float)M_PI;
                buf[i*2 + 0] = s;
                buf[i*2 + 1] = s;
            }
            /* Fade-in y fade-out cortos para evitar clicks entre notas */
            if (c == 0) {
                for (size_t i = 0; i < samples_per_chunk; ++i) {
                    float k = (float)i / (float)samples_per_chunk;
                    buf[i*2 + 0] = (int16_t)(buf[i*2 + 0] * k);
                    buf[i*2 + 1] = (int16_t)(buf[i*2 + 1] * k);
                }
            }
            if (c == chunks - 1) {
                for (size_t i = 0; i < samples_per_chunk; ++i) {
                    float k = 1.0f - (float)i / (float)samples_per_chunk;
                    buf[i*2 + 0] = (int16_t)(buf[i*2 + 0] * k);
                    buf[i*2 + 1] = (int16_t)(buf[i*2 + 1] * k);
                }
            }
            esp_codec_dev_write(s_codec, buf, buf_bytes);
        }
    }

    /* Silencio final + apagar PA */
    memset(buf, 0, buf_bytes);
    for (int i = 0; i < 4; ++i) esp_codec_dev_write(s_codec, buf, buf_bytes);
    gpio_set_level(PA_CTRL, 0);
    free(buf);
    return ESP_OK;
}

esp_err_t audio_play_jingle(audio_jingle_t j)
{
    /* Notas en Hz: C5=523, D5=587, E5=659, G5=784, A5=880, C6=1047, E6=1319, G4=392, C4=261 */
    switch (j) {
        case AUDIO_JINGLE_BOOT_OK: {
            static const audio_note_t notes[] = {
                {523, 100}, {659, 100}, {784, 100}, {1047, 200},
            };
            return audio_play_tones(notes, sizeof(notes)/sizeof(notes[0]));
        }
        case AUDIO_JINGLE_CRITICAL: {
            static const audio_note_t notes[] = {
                {880, 150}, {0, 50}, {880, 150}, {0, 50}, {880, 150}, {0, 50}, {880, 300},
            };
            return audio_play_tones(notes, sizeof(notes)/sizeof(notes[0]));
        }
        case AUDIO_JINGLE_WARNING: {
            static const audio_note_t notes[] = {
                {784, 120}, {0, 60}, {659, 120},
            };
            return audio_play_tones(notes, sizeof(notes)/sizeof(notes[0]));
        }
        case AUDIO_JINGLE_CONFIRM: {
            static const audio_note_t notes[] = {
                {1319, 80},
            };
            return audio_play_tones(notes, sizeof(notes)/sizeof(notes[0]));
        }
    }
    return ESP_ERR_INVALID_ARG;
}


void audio_cancel_playback(void) { s_audio_gen++; }
