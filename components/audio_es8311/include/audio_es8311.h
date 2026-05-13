#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int freq_hz;        /* 0 = silencio */
    int duration_ms;
} audio_note_t;

typedef enum {
    AUDIO_JINGLE_BOOT_OK = 0,
    AUDIO_JINGLE_CRITICAL,
    AUDIO_JINGLE_WARNING,
    AUDIO_JINGLE_CONFIRM,
} audio_jingle_t;

esp_err_t audio_init(i2c_master_bus_handle_t bus);
esp_err_t audio_beep(int freq_hz, int duration_ms);
esp_err_t audio_play_tones(const audio_note_t *notes, size_t count);
esp_err_t audio_play_jingle(audio_jingle_t jingle);

/* Volumen 0..100 (persistente en NVS namespace 'audio'). */
esp_err_t audio_set_volume(int vol);
int       audio_get_volume(void);

/* Mute global (persistente). Cuando esta muted, audio_play_* no produce nada. */
esp_err_t audio_set_mute(bool mute);
bool      audio_is_muted(void);

/* Cancelar la reproduccion en curso (audio_beep / audio_play_tones).
 * El playback en progreso saldra del bucle interno en < 50 ms y devolvera.
 * El flag se autorresetea al inicio de la siguiente llamada a audio_beep
 * o audio_play_tones. Util para alarmas que el usuario quiere silenciar
 * instantaneamente. */
void      audio_cancel_playback(void);

#ifdef __cplusplus
}
#endif
