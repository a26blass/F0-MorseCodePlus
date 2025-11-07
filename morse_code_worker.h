#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <furi.h>

/* Tone + timing */
#define FREQUENCY 261.63f
#define SLEEP 10
#define DOT "."
#define LINE "-"
#define SPACE " "

typedef void (*MorseCodeWorkerCallback)(FuriString* buffer, void* context);

typedef struct MorseCodeWorker MorseCodeWorker;

/* lifecycle */
MorseCodeWorker* morse_code_worker_alloc(void);
void morse_code_worker_free(MorseCodeWorker* instance);
void morse_code_worker_start(MorseCodeWorker* instance);
void morse_code_worker_stop(MorseCodeWorker* instance);

/* live keying (press/hold) */
void morse_code_worker_play(MorseCodeWorker* instance, bool play);

/* decoded text buffer mgmt */
void morse_code_worker_reset_text(MorseCodeWorker* instance);
void morse_code_worker_set_text_cstr(MorseCodeWorker* instance, const char* s);

/* params */
void morse_code_worker_set_volume(MorseCodeWorker* instance, float level);
void morse_code_worker_set_dit_delta(MorseCodeWorker* instance, uint32_t delta);

/* callbacks */
void morse_code_worker_set_callback(
    MorseCodeWorker* instance, MorseCodeWorkerCallback callback, void* context);

/* async playback (non-blocking) + cancel */
void morse_code_worker_playback_async(MorseCodeWorker* instance, const char* s, bool flash_led);
void morse_code_worker_cancel_playback(MorseCodeWorker* instance);
bool morse_code_worker_is_playback_active(MorseCodeWorker* instance);
