#include "morse_code_worker.h"
#include <furi.h>
#include <gui/gui.h>
#include <gui/elements.h>
#include <input/input.h>
#include <stdlib.h>
#include <furi_hal.h>
#include <string.h>
#include <stdbool.h>

/* =========================
 *  Constants & Morse tables
 * ========================= */

static const char* LOOKUP_ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZ1234567890 ";
static const size_t LOOKUP_ALPHABET_LEN = 26 + 10 + 1;

/* Morse table (A–Z, 1–0) */
static const char MORSE_TABLE[36][6] = {
    ".-","-...","-.-.","-..",".","..-.",
    "--.","....","..",".---","-.-",".-..",
    "--","-.","---",".--.","--.-",".-.",
    "...","-","..-","...-",".--","-..-",
    "-.--","--..",".----","..---","...--","....-",
    ".....","-....","--...","---..","----.","-----"
};
static const char SYMBOL_TABLE[36] = {
    'A','B','C','D','E','F','G','H','I','J','K','L',
    'M','N','O','P','Q','R','S','T','U','V','W','X',
    'Y','Z','1','2','3','4','5','6','7','8','9','0'
};

static const float MORSE_CODE_VOLUMES[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};

/* =============
 *  App state
 * ============= */

typedef enum { STATE_MAIN = 0, STATE_MENU, STATE_LOOKUP } AppState;

typedef struct {
    FuriString* words;      /* live decoded / composed text */
    uint8_t volume;         /* 0..4 index into MORSE_CODE_VOLUMES */
    uint32_t dit_delta;     /* ms for dot */
    AppState state;
    uint8_t menu_index;     /* menu cursor 0..3 */
    uint8_t lookup_index;   /* index into LOOKUP_ALPHABET */
    bool back_guard;        /* swallow Back until release to prevent retrigger */
    bool lookup_ok_guard;   /* swallow OK right after entering LOOKUP */
} MorseCodeModel;

typedef struct {
    MorseCodeModel* model;
    FuriMutex* model_mutex;
    FuriMessageQueue* input_queue;
    ViewPort* view_port;
    Gui* gui;
    MorseCodeWorker* worker;
} MorseCode;

/* =============
 *  Helpers
 * ============= */

static void draw_simple_title(Canvas* c, const char* t){
    canvas_set_font(c, FontPrimary);
    canvas_draw_str(c, 4, 12, t);
    canvas_draw_line(c, 4, 14, 123, 14);
}

static const char* morse_for_char(char c) {
    if(c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    for(size_t i = 0; i < 36; i++) {
        if(SYMBOL_TABLE[i] == c) return MORSE_TABLE[i];
    }
    return NULL; /* no code (space or unknown) */
}

/* =============
 *  UI: Menu
 * ============= */

static void draw_menu(Canvas* canvas, MorseCodeModel* m) {
    draw_simple_title(canvas, "Morse Menu");
    canvas_set_font(canvas, FontSecondary);
    const char* items[4] = {"Erase", "Lookup", "Playback", "Exit"};

    int y = 24;
    const int step = 12;
    for(int i = 0; i < 4; i++) {
        if(m->menu_index == i) {
            canvas_draw_box(canvas, 4, y - 9, 120, 12);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_str(canvas, 8, y, items[i]);
            canvas_set_color(canvas, ColorBlack);
        } else {
            canvas_draw_str(canvas, 8, y, items[i]);
        }
        y += step;
    }
    /* No bottom hints here to keep all items visible on-screen */
}

/* =============
 *  UI: Lookup
 * ============= */

static void draw_lookup(Canvas* canvas, MorseCodeModel* m) {
    draw_simple_title(canvas, "Lookup");

    /* Selected symbol */
    char sym = LOOKUP_ALPHABET[m->lookup_index];

    /* Left: symbol label big */
    char left_label[16];
    if(sym == ' ') {
        strcpy(left_label, "[space]");
    } else {
        left_label[0] = sym;
        left_label[1] = '\0';
    }
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 8, 34, left_label);

    /* Right: small “.-” text at top-right */
    const char* code = morse_for_char(sym);
    canvas_set_font(canvas, FontSecondary);
    if(code) {
        canvas_draw_str_aligned(canvas, 120, 22, AlignRight, AlignCenter, code);
    } else {
        canvas_draw_str_aligned(canvas, 120, 22, AlignRight, AlignCenter, "(gap)");
    }

    /* Centered dot/dash bar near bottom */
    if(code) {
        int total_w = 0;
        for(const char* p = code; *p; ++p) total_w += (*p == '.') ? 8 : 14;
        if(total_w > 0) total_w -= 4;

        int x = (64 - total_w/2);
        if(x < 8) x = 8;
        int y = 48;
        for(const char* p = code; *p; ++p) {
            if(*p == '.') { canvas_draw_box(canvas, x, y-2, 4, 4); x += 8; }
            else          { canvas_draw_box(canvas, x, y-2, 10, 4); x += 14; }
            if(x > 120) break;
        }
    }

    /* Bottom hints for lookup controls */
    elements_button_left(canvas, "Back");
    elements_button_center(canvas, "Add");
    elements_button_right(canvas, "Play");
}

/* =============
 *  Worker -> UI
 * ============= */

static void worker_ui_cb(FuriString* words, void* ctx) {
    MorseCode* app = ctx;
    if(furi_mutex_acquire(app->model_mutex, FuriWaitForever) != FuriStatusOk) return;
    furi_string_set(app->model->words, words);
    furi_mutex_release(app->model_mutex);
    view_port_update(app->view_port);
}

/* =============
 *  Viewport
 * ============= */

static void render_callback(Canvas* canvas, void* ctx) {
    MorseCode* app = ctx;

    canvas_clear(canvas);

    furi_check(furi_mutex_acquire(app->model_mutex, FuriWaitForever) == FuriStatusOk);
    MorseCodeModel* m = app->model;

    if(m->state == STATE_MENU) {
        draw_menu(canvas, m);
        furi_mutex_release(app->model_mutex);
        return;
    }
    if(m->state == STATE_LOOKUP) {
        draw_lookup(canvas, m);
        furi_mutex_release(app->model_mutex);
        return;
    }

    /* STATE_MAIN */
    canvas_set_font(canvas, FontPrimary);
    elements_multiline_text_aligned(
        canvas, 64, 30, AlignCenter, AlignCenter, furi_string_get_cstr(m->words));

    /* volume bar */
    const uint8_t vol_bar_x_pos = 124, vol_bar_y_pos = 0;
    const uint8_t volume_h = (uint8_t)((64 / (5 - 1)) * m->volume);
    canvas_draw_frame(canvas, vol_bar_x_pos, vol_bar_y_pos, 4, 64);
    canvas_draw_box(canvas, vol_bar_x_pos, (uint8_t)(vol_bar_y_pos + (64 - volume_h)), 4, volume_h);

    /* dit label */
    FuriString* dit = furi_string_alloc_printf("Dit: %ld ms", m->dit_delta);
    canvas_draw_str_aligned(canvas, 0, 10, AlignLeft, AlignCenter, furi_string_get_cstr(dit));
    furi_string_free(dit);

    /* controls */
    elements_button_left(canvas, "Menu");

    furi_mutex_release(app->model_mutex);
}

static void input_callback(InputEvent* e, void* ctx) {
    MorseCode* app = ctx;
    furi_message_queue_put(app->input_queue, e, FuriWaitForever);
}

/* =============
 *  Lifecycle
 * ============= */

static MorseCode* morse_code_alloc(void) {
    MorseCode* inst = malloc(sizeof(MorseCode));

    inst->model = malloc(sizeof(MorseCodeModel));
    inst->model->words = furi_string_alloc_set_str("");
    inst->model->volume = 3;
    inst->model->dit_delta = 150;
    inst->model->state = STATE_MAIN;
    inst->model->menu_index = 0;
    inst->model->lookup_index = 0;
    inst->model->back_guard = false;
    inst->model->lookup_ok_guard = false;

    inst->model_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    inst->input_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    inst->worker = morse_code_worker_alloc();
    morse_code_worker_set_callback(inst->worker, worker_ui_cb, inst);

    inst->view_port = view_port_alloc();
    view_port_draw_callback_set(inst->view_port, render_callback, inst);
    view_port_input_callback_set(inst->view_port, input_callback, inst);

    inst->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(inst->gui, inst->view_port, GuiLayerFullscreen);
    return inst;
}

static void morse_code_free(MorseCode* inst) {
    gui_remove_view_port(inst->gui, inst->view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(inst->view_port);

    morse_code_worker_free(inst->worker);

    furi_message_queue_free(inst->input_queue);
    furi_mutex_free(inst->model_mutex);

    furi_string_free(inst->model->words);
    free(inst->model);
    free(inst);
}

/* =============
 *  Entry
 * ============= */

int32_t morse_code_plus_app(void) {
    MorseCode* app = morse_code_alloc();
    InputEvent in;

    morse_code_worker_start(app->worker);
    morse_code_worker_set_volume(app->worker, MORSE_CODE_VOLUMES[app->model->volume]);
    morse_code_worker_set_dit_delta(app->worker, app->model->dit_delta);

    while(furi_message_queue_get(app->input_queue, &in, FuriWaitForever) == FuriStatusOk) {
        bool start_playback = false;
        char playback_buf[128]; playback_buf[0] = '\0';

        bool do_set_text = false;
        char set_text_buf[128]; set_text_buf[0] = '\0';

        furi_check(furi_mutex_acquire(app->model_mutex, FuriWaitForever) == FuriStatusOk);
        MorseCodeModel* m = app->model;
        const AppState state_now = m->state;

        /* global exit (long back) */
        if(in.key == InputKeyBack && in.type == InputTypeLong) {
            furi_mutex_release(app->model_mutex);
            break;
        }

        /* If a playback is running, Back cancels it (do NOT set back_guard here). */
        if(morse_code_worker_is_playback_active(app->worker)) {
            if(in.key == InputKeyBack && in.type == InputTypePress) {
                morse_code_worker_cancel_playback(app->worker); /* flashes red, stops */
                /* No back_guard latch here, so OK/tones work immediately after cancel */
                furi_mutex_release(app->model_mutex);
                view_port_update(app->view_port);
                continue;
            }
            /* While playing back, ignore other UI changes */
            furi_mutex_release(app->model_mutex);
            view_port_update(app->view_port);
            continue;
        }

        /* Back guard for normal screens */
        if(m->back_guard) {
            if(in.key == InputKeyBack && in.type == InputTypeRelease) m->back_guard = false;
            furi_mutex_release(app->model_mutex);
            continue;
        }

        if(state_now == STATE_MENU) {
            if(in.type == InputTypePress) {
                if(in.key == InputKeyUp) {
                    m->menu_index = (m->menu_index == 0) ? 3 : (uint8_t)(m->menu_index - 1);
                } else if(in.key == InputKeyDown) {
                    m->menu_index = (uint8_t)((m->menu_index + 1) % 4);
                } else if(in.key == InputKeyBack || in.key == InputKeyLeft) {
                    m->state = STATE_MAIN;
                    m->back_guard = true;
                } else if(in.key == InputKeyOk) {
                    switch(m->menu_index) {
                        case 0: /* Erase */
                            furi_string_reset(m->words);
                            set_text_buf[0] = '\0';
                            do_set_text = true;
                            m->state = STATE_MAIN;
                            break;
                        case 1: /* Lookup */
                            m->state = STATE_LOOKUP;
                            m->lookup_ok_guard = true;
                            break;
                        case 2: /* Playback */
                            strlcpy(playback_buf, furi_string_get_cstr(m->words), sizeof(playback_buf));
                            start_playback = true;           /* async */
                            m->state = STATE_MAIN;
                            break;
                        case 3: /* Exit */
                            furi_mutex_release(app->model_mutex);
                            goto exit_loop;
                    }
                }
            }

        } else if(state_now == STATE_LOOKUP) {
            /* Swallow lingering OK from entering Lookup until OK is released */
            if(m->lookup_ok_guard && in.key == InputKeyOk) {
                if(in.type == InputTypeRelease) m->lookup_ok_guard = false;
                furi_mutex_release(app->model_mutex);
                continue;
            }

            if(in.type == InputTypePress) {
                if(in.key == InputKeyUp) {
                    m->lookup_index = (m->lookup_index == 0)
                                        ? (uint8_t)(LOOKUP_ALPHABET_LEN - 1)
                                        : (uint8_t)(m->lookup_index - 1);
                } else if(in.key == InputKeyDown) {
                    m->lookup_index = (uint8_t)((m->lookup_index + 1) % (uint8_t)LOOKUP_ALPHABET_LEN);
                } else if(in.key == InputKeyLeft || in.key == InputKeyBack) {
                    m->state = STATE_MENU;
                    m->back_guard = (in.key == InputKeyBack);
                } else if(in.key == InputKeyRight) {
                    char sym = LOOKUP_ALPHABET[m->lookup_index];
                    playback_buf[0] = sym;
                    playback_buf[1] = '\0';
                    start_playback = true;                 /* async single char */
                }
            }
            if(in.key == InputKeyOk && in.type == InputTypeShort && !m->lookup_ok_guard) {
                char sym = LOOKUP_ALPHABET[m->lookup_index];
                furi_string_push_back(m->words, sym);
                strlcpy(set_text_buf, furi_string_get_cstr(m->words), sizeof(set_text_buf));
                do_set_text = true;
            }

        } else { /* STATE_MAIN */
            if(in.key == InputKeyBack && in.type == InputTypeShort) {
                m->state = STATE_MENU;
                m->menu_index = 0;
            } else if(in.key == InputKeyOk) {
                /* handled below via worker_play on press/release */
            } else if(in.key == InputKeyUp && in.type == InputTypePress) {
                if(m->volume < 4) m->volume++;
            } else if(in.key == InputKeyDown && in.type == InputTypePress) {
                if(m->volume > 0) m->volume--;
            } else if(in.key == InputKeyLeft && in.type == InputTypePress) {
                if(m->dit_delta > 10) m->dit_delta -= 10;
            } else if(in.key == InputKeyRight && in.type == InputTypePress) {
                if(m->dit_delta >= 10) m->dit_delta += 10;
            }
        }

        /* capture params + ok states for audio (main only) */
        const uint8_t volume_idx = m->volume;
        const uint32_t dit = m->dit_delta;
        const bool ok_press_main =
            (state_now == STATE_MAIN && in.key == InputKeyOk && in.type == InputTypePress);
        const bool ok_release_main =
            (state_now == STATE_MAIN && in.key == InputKeyOk && in.type == InputTypeRelease);

        furi_mutex_release(app->model_mutex);

        /* ---- worker calls AFTER unlock ---- */
        morse_code_worker_set_volume(app->worker, MORSE_CODE_VOLUMES[volume_idx]);
        morse_code_worker_set_dit_delta(app->worker, dit);

        if(ok_press_main)  morse_code_worker_play(app->worker, true);
        if(ok_release_main) morse_code_worker_play(app->worker, false);

        if(start_playback && playback_buf[0] != '\0') {
            morse_code_worker_playback_async(app->worker, playback_buf, true);
        }

        if(do_set_text) {
            morse_code_worker_set_text_cstr(app->worker, set_text_buf);
        }

        view_port_update(app->view_port);
    }

exit_loop:
    morse_code_worker_stop(app->worker);
    morse_code_free(app);
    return 0;
}
