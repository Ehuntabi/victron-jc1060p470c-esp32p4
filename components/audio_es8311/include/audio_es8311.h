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
/* wait_if_busy: true (alarma) espera a que termine otra reproduccion en curso;
 * false (click/jingle) aborta y devuelve ESP_ERR_TIMEOUT si ya suena algo.
 * Garantiza que nunca hay dos reproducciones a la vez sobre el codec. */
esp_err_t audio_play_tones(const audio_note_t *notes, size_t count, bool wait_if_busy);
esp_err_t audio_play_jingle(audio_jingle_t jingle);

/* Volumen 0..100 (persistente en NVS namespace 'audio'). */
esp_err_t audio_set_volume(int vol);
/* Ajusta el volumen del HW SIN persistir en NVS (subidas transitorias como la
 * alarma, que luego restaura el valor previo). No toca s_volume ni graba flash. */
esp_err_t audio_set_volume_transient(uint8_t vol);
int       audio_get_volume(void);

/* Mute global (persistente). Cuando esta muted, audio_play_* no produce nada. */
esp_err_t audio_set_mute(bool mute);
bool      audio_is_muted(void);

/* Cancelar la reproduccion en curso (audio_play_tones).
 * El playback en progreso saldra del bucle interno en < 50 ms y devolvera.
 * Incrementa un generation counter: la llamada en curso lo detecta y aborta,
 * y la siguiente captura su propia generacion al entrar. Util para alarmas
 * que el usuario quiere silenciar instantaneamente. */
void      audio_cancel_playback(void);

#ifdef __cplusplus
}
#endif
