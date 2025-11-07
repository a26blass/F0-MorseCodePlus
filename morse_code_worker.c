#include "morse_code_worker.h"
#include <furi_hal.h>
#include <lib/flipper_format/flipper_format.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <string.h>

/* forward declare the worker thread fn */
static int32_t morse_code_worker_thread_callback(void* context);
/* forward declare the async playback thread fn */
static int32_t morse_code_worker_playback_thread(void* context);

#define TAG "MorseCodeWorker"
#define MORSE_CODE_VERSION 0

/* Morse tables (A–Z, 1–0) */
static const char morse_array[36][6] = {
    ".-","-...","-.-.","-..",".","..-.",
    "--.","....","..",".---","-.-",".-..",
    "--","-.","---",".--.","--.-",".-.",
    "...","-","..-","...-",".--","-..-",
    "-.--","--..",".----","..---","...--","....-",
    ".....","-....","--...","---..","----.","-----"
};
static const char symbol_array[36] = {
    'A','B','C','D','E','F','G','H','I','J','K','L',
    'M','N','O','P','Q','R','S','T','U','V','W','X',
    'Y','Z','1','2','3','4','5','6','7','8','9','0'
};

struct MorseCodeWorker {
    /* live keying thread */
    FuriThread* thread;
    MorseCodeWorkerCallback callback;
    void* callback_context;
    bool is_running;
    bool play;          /* live keying flag */
    float volume;
    uint32_t dit_delta;
    FuriString* buffer;
    FuriString* words;

    /* LED / notifications */
    NotificationApp* notification;

    /* async playback thread */
    FuriThread* pb_thread;
    FuriMutex* pb_mutex;
    FuriString* pb_text;
    bool pb_flash_led;
    volatile bool pb_cancel;
    volatile bool pb_running;
};

/* ---------- live keying decode path ---------- */

static void morse_code_worker_fill_buffer(MorseCodeWorker* instance, uint32_t duration) {
    if(duration <= instance->dit_delta)
        furi_string_push_back(instance->buffer, *DOT);
    else if(duration <= (instance->dit_delta * 3))
        furi_string_push_back(instance->buffer, *LINE);
    else
        furi_string_reset(instance->buffer);
    if(furi_string_size(instance->buffer) > 5) furi_string_reset(instance->buffer);
}

static void morse_code_worker_fill_letter(MorseCodeWorker* instance) {
    if(furi_string_size(instance->words) > 63) furi_string_reset(instance->words);
    for(size_t i = 0; i < 36; i++) {
        if(furi_string_cmp_str(instance->buffer, morse_array[i]) == 0) {
            furi_string_push_back(instance->words, symbol_array[i]);
            break;
        }
    }
    furi_string_reset(instance->buffer);
}

static int32_t morse_code_worker_thread_callback(void* context) {
    furi_assert(context);
    MorseCodeWorker* instance = context;
    bool was_playing = false;
    uint32_t start_tick = 0;
    uint32_t end_tick = 0;
    bool pushed = true;
    bool spaced = true;

    while(instance->is_running) {
        furi_delay_ms(SLEEP);

        if(instance->play) {
            if(!was_playing) {
                start_tick = furi_get_tick();
                if(furi_hal_speaker_acquire(1000)) {
                    furi_hal_speaker_start(FREQUENCY, instance->volume);
                }
                was_playing = true;
            }
        } else {
            if(was_playing) {
                pushed = false;
                spaced = false;
                if(furi_hal_speaker_is_mine()) {
                    furi_hal_speaker_stop();
                    furi_hal_speaker_release();
                }
                end_tick = furi_get_tick();
                was_playing = false;
                morse_code_worker_fill_buffer(instance, end_tick - start_tick);
                start_tick = 0;
            }
        }

        if(!pushed) {
            if(end_tick + (instance->dit_delta * 3) < furi_get_tick()) {
                if(!furi_string_empty(instance->buffer)) {
                    morse_code_worker_fill_letter(instance);
                    if(instance->callback)
                        instance->callback(instance->words, instance->callback_context);
                } else {
                    spaced = true;
                }
                pushed = true;
            }
        }
        if(!spaced) {
            if(end_tick + (instance->dit_delta * 7) < furi_get_tick()) {
                furi_string_push_back(instance->words, *SPACE);
                if(instance->callback)
                    instance->callback(instance->words, instance->callback_context);
                spaced = true;
            }
        }
    }
    return 0;
}

/* ---------- LED helpers ---------- */
static inline void led_blue_on(NotificationApp* n) {
    if(n) notification_message_block(n, &sequence_set_blue_255);
}
static inline void led_blue_off(NotificationApp* n) {
    if(n) notification_message_block(n, &sequence_reset_blue);
}
static inline void flash_red_once(NotificationApp* n) {
    if(!n) return;
    notification_message_block(n, &sequence_set_red_255);
    furi_delay_ms(120);
    notification_message_block(n, &sequence_reset_red);
}

/* ---------- playback primitive (blocking, internal) ---------- */
// static void play_tone_blocking(float freq, float vol, uint32_t ms, NotificationApp* n, bool flash_led) {
//     if(furi_hal_speaker_acquire(1000)) {
//         if(flash_led) led_blue_on(n);
//         furi_hal_speaker_start(freq, vol);
//         furi_delay_ms(ms);
//         furi_hal_speaker_stop();
//         furi_hal_speaker_release();
//         if(flash_led) led_blue_off(n);
//     } else {
//         furi_delay_ms(ms);
//     }
// }
static void gap_blocking(uint32_t ms) { furi_delay_ms(ms); }

/* ---------- async playback thread ---------- */
static int32_t morse_code_worker_playback_thread(void* context) {
    MorseCodeWorker* instance = context;
    /* copy params under mutex */
    furi_mutex_acquire(instance->pb_mutex, FuriWaitForever);
    const bool flash = instance->pb_flash_led;
    FuriString* text = furi_string_alloc_set(instance->pb_text);
    instance->pb_running = true;
    instance->pb_cancel = false;
    furi_mutex_release(instance->pb_mutex);

    const uint32_t dit = instance->dit_delta;
    const uint32_t dah = 3 * dit;
    const uint32_t intra = dit;
    const uint32_t inter_letter = 3 * dit;
    const uint32_t inter_word = 7 * dit;

    /* Make sure live keying isn't holding the speaker */
    instance->play = false;

    const char* s = furi_string_get_cstr(text);
    for(const char* p = s; *p; ++p) {
        if(instance->pb_cancel) { flash_red_once(instance->notification); break; }

        char c = *p;
        if(c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');

        if(c == ' ') {
            for(uint32_t t = 0; t < inter_word; t += 5) {
                if(instance->pb_cancel) { flash_red_once(instance->notification); goto done; }
                gap_blocking(5);
            }
            continue;
        }

        /* lookup morse */
        const char* code = NULL;
        for(size_t i = 0; i < 36; i++) {
            if(symbol_array[i] == c) { code = morse_array[i]; break; }
        }
        if(!code) {
            for(uint32_t t = 0; t < inter_letter; t += 5) {
                if(instance->pb_cancel) { flash_red_once(instance->notification); goto done; }
                gap_blocking(5);
            }
            continue;
        }

        /* play each element with cancel checks */
        for(size_t i = 0; code[i] != '\0'; i++) {
            uint32_t dur = (code[i] == '.') ? dit : dah;

            /* tone with small slices so cancel is responsive */
            uint32_t played = 0;
            if(furi_hal_speaker_acquire(1000)) {
                if(flash) led_blue_on(instance->notification);
                furi_hal_speaker_start(FREQUENCY, instance->volume);
                while(played < dur) {
                    if(instance->pb_cancel) {
                        furi_hal_speaker_stop();
                        furi_hal_speaker_release();
                        if(flash) led_blue_off(instance->notification);
                        flash_red_once(instance->notification);
                        goto done;
                    }
                    uint32_t slice = (dur - played) > 5 ? 5 : (dur - played);
                    furi_delay_ms(slice);
                    played += slice;
                }
                furi_hal_speaker_stop();
                furi_hal_speaker_release();
                if(flash) led_blue_off(instance->notification);
            } else {
                for(uint32_t t = 0; t < dur; t += 5) {
                    if(instance->pb_cancel) { flash_red_once(instance->notification); goto done; }
                    furi_delay_ms(5);
                }
            }

            /* intra-element gap if more elements */
            if(code[i + 1] != '\0') {
                for(uint32_t t = 0; t < intra; t += 5) {
                    if(instance->pb_cancel) { flash_red_once(instance->notification); goto done; }
                    furi_delay_ms(5);
                }
            }
        }

        /* inter-letter gap */
        for(uint32_t t = 0; t < inter_letter; t += 5) {
            if(instance->pb_cancel) { flash_red_once(instance->notification); goto done; }
            furi_delay_ms(5);
        }
    }

done:
    furi_mutex_acquire(instance->pb_mutex, FuriWaitForever);
    instance->pb_running = false;
    furi_mutex_release(instance->pb_mutex);
    furi_string_free(text);
    return 0;
}

/* ---------------- public API ---------------- */

MorseCodeWorker* morse_code_worker_alloc(void) {
    MorseCodeWorker* instance = malloc(sizeof(MorseCodeWorker));
    instance->thread = furi_thread_alloc();
    furi_thread_set_name(instance->thread, "MorseCodeWorker");
    furi_thread_set_stack_size(instance->thread, 1024);
    furi_thread_set_context(instance->thread, instance);
    furi_thread_set_callback(instance->thread, morse_code_worker_thread_callback);
    instance->play = false;
    instance->volume = 1.0f;
    instance->dit_delta = 150;
    instance->buffer = furi_string_alloc_set_str("");
    instance->words = furi_string_alloc_set_str("");
    instance->notification = furi_record_open(RECORD_NOTIFICATION);
    instance->is_running = false;
    instance->callback = NULL;
    instance->callback_context = NULL;

    /* async playback init */
    instance->pb_thread = NULL;
    instance->pb_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->pb_text = furi_string_alloc_set_str("");
    instance->pb_flash_led = true;
    instance->pb_cancel = false;
    instance->pb_running = false;
    return instance;
}

void morse_code_worker_free(MorseCodeWorker* instance) {
    furi_assert(instance);
    /* stop async playback thread if running */
    morse_code_worker_cancel_playback(instance);
    if(instance->pb_thread) {
        furi_thread_join(instance->pb_thread);
        furi_thread_free(instance->pb_thread);
        instance->pb_thread = NULL;
    }
    furi_mutex_free(instance->pb_mutex);
    furi_string_free(instance->pb_text);

    if(instance->notification) {
        notification_message_block(instance->notification, &sequence_reset_green);
        furi_record_close(RECORD_NOTIFICATION);
    }
    furi_string_free(instance->buffer);
    furi_string_free(instance->words);
    furi_thread_free(instance->thread);
    free(instance);
}

void morse_code_worker_set_callback(
    MorseCodeWorker* instance, MorseCodeWorkerCallback callback, void* context) {
    furi_assert(instance);
    instance->callback = callback;
    instance->callback_context = context;
}

void morse_code_worker_play(MorseCodeWorker* instance, bool play) {
    furi_assert(instance);
    instance->play = play;
}

void morse_code_worker_set_volume(MorseCodeWorker* instance, float level) {
    furi_assert(instance);
    instance->volume = level;
}

void morse_code_worker_set_dit_delta(MorseCodeWorker* instance, uint32_t delta) {
    furi_assert(instance);
    instance->dit_delta = delta;
}

void morse_code_worker_reset_text(MorseCodeWorker* instance) {
    furi_assert(instance);
    furi_string_reset(instance->buffer);
    furi_string_reset(instance->words);
    if(instance->callback) instance->callback(instance->words, instance->callback_context);
}

void morse_code_worker_set_text_cstr(MorseCodeWorker* instance, const char* s) {
    furi_assert(instance);
    if(!s) s = "";
    furi_string_set_str(instance->words, s);
    if(instance->callback) instance->callback(instance->words, instance->callback_context);
}

/* ----- async playback API ----- */
void morse_code_worker_playback_async(MorseCodeWorker* instance, const char* s, bool flash_led) {
    furi_assert(instance);
    if(!s) s = "";

    /* If a previous playback is running, cancel and join it first */
    if(instance->pb_thread) {
        morse_code_worker_cancel_playback(instance);
        furi_thread_join(instance->pb_thread);
        furi_thread_free(instance->pb_thread);
        instance->pb_thread = NULL;
    }

    furi_mutex_acquire(instance->pb_mutex, FuriWaitForever);
    furi_string_set_str(instance->pb_text, s);
    instance->pb_flash_led = flash_led;
    instance->pb_cancel = false;
    instance->pb_running = false;
    furi_mutex_release(instance->pb_mutex);

    instance->pb_thread = furi_thread_alloc();
    furi_thread_set_name(instance->pb_thread, "MorsePB");
    furi_thread_set_stack_size(instance->pb_thread, 1024);
    furi_thread_set_context(instance->pb_thread, instance);
    furi_thread_set_callback(instance->pb_thread, morse_code_worker_playback_thread);
    furi_thread_start(instance->pb_thread);
}

void morse_code_worker_cancel_playback(MorseCodeWorker* instance) {
    furi_assert(instance);
    instance->pb_cancel = true;
}

bool morse_code_worker_is_playback_active(MorseCodeWorker* instance) {
    furi_assert(instance);
    bool running = false;
    furi_mutex_acquire(instance->pb_mutex, FuriWaitForever);
    running = instance->pb_running;
    furi_mutex_release(instance->pb_mutex);
    return running;
}

/* ----- lifecycle ----- */
void morse_code_worker_start(MorseCodeWorker* instance) {
    furi_assert(instance && !instance->is_running);
    instance->is_running = true;
    furi_thread_start(instance->thread);
}

void morse_code_worker_stop(MorseCodeWorker* instance) {
    furi_assert(instance && instance->is_running);
    instance->play = false;
    instance->is_running = false;
    furi_thread_join(instance->thread);

    /* stop async playback if any */
    morse_code_worker_cancel_playback(instance);
    if(instance->pb_thread) {
        furi_thread_join(instance->pb_thread);
        furi_thread_free(instance->pb_thread);
        instance->pb_thread = NULL;
    }

    if(instance->notification) {
        notification_message_block(instance->notification, &sequence_reset_green);
    }
}
