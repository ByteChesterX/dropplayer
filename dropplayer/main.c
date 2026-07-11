#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include <gtk/gtk.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <signal.h>
#include <execinfo.h>
#include <stdio.h>
#include <stdarg.h>

#define MAX_TRACKS 512
#define LOG_FILE "/tmp/dropplayer.log"

/* ── logging ── */
static FILE *g_logfile = NULL;

static void log_msg(const char *level, const char *func, int line, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char ts[32];
    snprintf(ts, sizeof(ts), "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);

    fprintf(stderr, "[%s] %s %s:%d: %s\n", ts, level, func, line, buf);
    fflush(stderr);

    if (g_logfile) {
        fprintf(g_logfile, "[%s] %s %s:%d: %s\n", ts, level, func, line, buf);
        fflush(g_logfile);
    }
}

#define LOG_I(fmt, ...) log_msg("INFO ", __func__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_W(fmt, ...) log_msg("WARN ", __func__, __LINE__, fmt, ##__VA_ARGS__)
#define LOG_E(fmt, ...) log_msg("ERROR", __func__, __LINE__, fmt, ##__VA_ARGS__)

/* ── crash handler ── */
static void crash_handler(int sig) {
    void *array[32];
    int n = backtrace(array, 32);
    fprintf(stderr, "\n=== CRASH signal %d ===\n", sig);
    backtrace_symbols_fd(array, n, 2);
    if (g_logfile) {
        fprintf(g_logfile, "\n=== CRASH signal %d ===\n", sig);
        void *arr2[32];
        int n2 = backtrace(arr2, 32);
        char **syms = backtrace_symbols(arr2, n2);
        if (syms) {
            for (int i = 0; i < n2; i++) fprintf(g_logfile, "  %s\n", syms[i]);
            free(syms);
        }
        fflush(g_logfile);
    }
    _exit(1);
}

typedef enum { MODE_SINGLE, MODE_LOOP, MODE_RANDOM } PlayMode;

typedef struct {
    char *path;
    char *name;
} Track;

typedef struct {
    /* miniaudio */
    ma_engine engine;
    ma_sound  sound;
    ma_bool32 sound_loaded;

    /* playlist */
    Track tracks[MAX_TRACKS];
    int   track_count;
    int   current;
    PlayMode mode;
    float volume;
    gboolean playing;

    /* ui refs */
    GtkWidget *window;
    GtkWidget *track_list;
    GtkWidget *btn_play;
    GtkWidget *btn_prev;
    GtkWidget *btn_next;
    GtkWidget *btn_stop;
    GtkWidget *btn_mode;
    GtkWidget *lbl_now;
    GtkWidget *lbl_count;
    GtkWidget *scale_vol;
    GtkWidget *btn_theme;
    GtkWidget *drop_box;
    GtkWidget *scroll;
    GtkWidget *center_stack;
    GtkWidget *controls_box;
    GtkWidget *scale_seek;
    GtkWidget *lbl_time;
    gboolean  dark;
    gboolean  seeking;
    GtkCssProvider *css;
} AppState;

static void apply_theme(AppState *app);
static void refresh_track_list(AppState *app);
static void play_index(AppState *app, int idx);
static void update_now_label(AppState *app);
static void stop_playback(AppState *app);
static void play_next(AppState *app);
static void on_seek(GtkWidget *w, gpointer data);

/* ── helpers ── */
static const char *basename_of(const char *path) {
    const char *p = strrchr(path, '/');
    return p ? p + 1 : path;
}

static char *strdup_safe(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *d = malloc(len + 1);
    memcpy(d, s, len + 1);
    return d;
}

static gboolean has_audio_ext(const char *name) {
    const char *ext = strrchr(name, '.');
    if (!ext) return FALSE;
    ext++;
    return (!g_ascii_strcasecmp(ext, "mp3") ||
            !g_ascii_strcasecmp(ext, "wav") ||
            !g_ascii_strcasecmp(ext, "ogg") ||
            !g_ascii_strcasecmp(ext, "flac") ||
            !g_ascii_strcasecmp(ext, "aac") ||
            !g_ascii_strcasecmp(ext, "m4a") ||
            !g_ascii_strcasecmp(ext, "opus") ||
            !g_ascii_strcasecmp(ext, "wma"));
}

/* ── miniaudio sound-end callback ── */
static gboolean idle_play_next(gpointer data) {
    LOG_I("idle_play_next triggered");
    AppState *app = data;
    play_next(app);
    return G_SOURCE_REMOVE;
}

static void sound_end_callback(void *ud, ma_sound *snd) {
    (void)snd;
    AppState *app = ud;
    LOG_I("sound_end_callback fired (audio thread)");
    g_idle_add(idle_play_next, app);
}

/* ── playback core ── */
static void stop_playback(AppState *app) {
    LOG_I("stop_playback: sound_loaded=%d", (int)app->sound_loaded);
    if (app->sound_loaded) {
        ma_sound_uninit(&app->sound);
        app->sound_loaded = MA_FALSE;
        LOG_I("stop_playback: sound uninited");
    }
    app->playing = FALSE;
}

static void play_index(AppState *app, int idx) {
    LOG_I("play_index(%d) called, track_count=%d", idx, app->track_count);
    if (idx < 0 || idx >= app->track_count) {
        LOG_W("play_index: invalid index %d", idx);
        return;
    }

    stop_playback(app);

    LOG_I("play_index: loading file: %s", app->tracks[idx].path);
    ma_result res = ma_sound_init_from_file(&app->engine,
                                             app->tracks[idx].path,
                                             MA_SOUND_FLAG_DECODE | MA_SOUND_FLAG_STREAM,
                                             NULL, NULL, &app->sound);
    if (res != MA_SUCCESS) {
        LOG_E("play_index: ma_sound_init_from_file FAILED: %d", res);
        return;
    }

    LOG_I("play_index: sound loaded OK, setting volume=%.2f", app->volume);
    ma_sound_set_volume(&app->sound, app->volume);
    ma_sound_set_end_callback(&app->sound, sound_end_callback, app);
    ma_sound_start(&app->sound);

    app->sound_loaded = MA_TRUE;
    app->current = idx;
    app->playing = TRUE;

    /* reset seek bar */
    if (app->scale_seek) {
        g_signal_handlers_block_by_func(app->scale_seek, on_seek, app);
        gtk_range_set_value(GTK_RANGE(app->scale_seek), 0.0);
        g_signal_handlers_unblock_by_func(app->scale_seek, on_seek, app);
    }
    if (app->lbl_time)
        gtk_label_set_text(GTK_LABEL(app->lbl_time), "0:00");

    update_now_label(app);
    refresh_track_list(app);
    LOG_I("play_index(%d): playback started", idx);
}

static void play_next(AppState *app) {
    LOG_I("play_next: mode=%d, current=%d, track_count=%d", app->mode, app->current, app->track_count);
    if (app->track_count == 0) return;
    stop_playback(app);

    switch (app->mode) {
    case MODE_SINGLE:
        LOG_I("play_next: MODE_SINGLE, stopping");
        app->playing = FALSE;
        update_now_label(app);
        refresh_track_list(app);
        return;
    case MODE_LOOP: {
        int next = (app->current + 1) % app->track_count;
        LOG_I("play_next: MODE_LOOP, next=%d", next);
        play_index(app, next);
    } break;
    case MODE_RANDOM: {
        int next;
        do { next = rand() % app->track_count; } while (next == app->current && app->track_count > 1);
        LOG_I("play_next: MODE_RANDOM, next=%d", next);
        play_index(app, next);
    } break;
    }
}

static void toggle_pause(AppState *app) {
    LOG_I("toggle_pause: sound_loaded=%d, playing=%d", (int)app->sound_loaded, (int)app->playing);
    if (!app->sound_loaded) return;
    if (app->playing) {
        ma_sound_stop(&app->sound);
        app->playing = FALSE;
        LOG_I("toggle_pause: paused");
    } else {
        ma_sound_start(&app->sound);
        app->playing = TRUE;
        LOG_I("toggle_pause: resumed");
    }
    refresh_track_list(app);
}

/* ── theme CSS ── */
static const char *DARK_CSS =
    ".window { background-color: #12121a; }"
    ".topbar { background-color: #1a1a28; border-bottom: 1px solid #2a2a3e; }"
    ".tracklist { background-color: #14141e; }"
    ".track-item { background-color: #1a1a28; border-radius: 8px; padding: 6px 10px; margin: 2px 8px; }"
    ".track-item:hover { background-color: #24243a; }"
    ".track-active { background-color: #251e45; border-left: 3px solid #7c5cbf; }"
    ".track-active:hover { background-color: #2e2550; }"
    ".controls { background-color: #1a1a28; border-top: 1px solid #2a2a3e; padding: 8px; }"
    ".subtitle { color: #666680; font-size: 12px; }"
    ".now-playing { color: #7c5cbf; font-style: italic; font-size: 12px; }"
    ".track-name { color: #c8c8d8; font-size: 13px; }"
    ".track-name-active { color: #b49aff; font-size: 13px; font-weight: bold; }"
    ".track-num { color: #555570; font-size: 12px; }"
    "button.ctrl { background-color: #28283c; color: #c8c8d8; border: none; border-radius: 10px; min-width: 42px; min-height: 36px; font-size: 16px; }"
    "button.ctrl:hover { background-color: #36364e; }"
    "button.ctrl:active { background-color: #44445e; }"
    "button.play-btn { background-color: #7c5cbf; color: white; }"
    "button.play-btn:hover { background-color: #8e6dd4; }"
    "button.mode-btn { background-color: transparent; color: #666680; border: 1px solid #2a2a3e; border-radius: 8px; font-size: 12px; padding: 4px 12px; }"
    "button.mode-btn:hover { background-color: #28283c; color: #c8c8d8; }"
    "button.mode-active { background-color: #7c5cbf; color: white; border-color: #7c5cbf; }"
    "button.mode-active:hover { background-color: #8e6dd4; }"
    "button.icon-btn { background-color: transparent; color: #7c5cbf; border: none; font-size: 18px; padding: 4px; }"
    "button.icon-btn:hover { color: #b49aff; }"
    "button.danger-btn { background-color: transparent; color: #c85050; border: none; font-size: 12px; }"
    "button.danger-btn:hover { color: #ff6060; }"
    "button.theme-btn { background-color: #28283c; color: #f0c040; border: none; border-radius: 50%; min-width: 34px; min-height: 34px; font-size: 16px; }"
    "button.theme-btn:hover { background-color: #36364e; }"
    "button.add-btn { background-color: transparent; color: #7c5cbf; border: none; font-size: 13px; }"
    "button.add-btn:hover { color: #b49aff; }"
    "button.skip-btn { font-size: 11px; min-width: 42px; color: #8888a0; }"
    "button.skip-btn:hover { background-color: #36364e; color: #c8c8d8; }"
    "scale trough { background-color: #28283c; border-radius: 4px; min-height: 6px; }"
    "scale slider { background-color: #7c5cbf; border-radius: 50%; min-width: 16px; min-height: 16px; }"
    "scale highlight { background-color: #7c5cbf; }"
    ".drop-area { background-color: #161620; border: 2px dashed #3a3a52; border-radius: 16px; padding: 40px; }"
    ".drop-label { color: #555570; font-size: 15px; }"
    ".drop-icon { font-size: 42px; }"
    ".drop-or { color: #444458; font-size: 11px; }"
    "button.browse-btn { background-color: #7c5cbf; color: white; border: none; border-radius: 10px; padding: 8px 20px; font-size: 13px; }"
    "button.browse-btn:hover { background-color: #8e6dd4; }"
    "scrollbar { background-color: transparent; }"
    "scrollbar slider { background-color: #3a3a52; border-radius: 4px; min-width: 6px; }"
;

static const char *LIGHT_CSS =
    ".window { background-color: #f0f0f5; }"
    ".topbar { background-color: #ffffff; border-bottom: 1px solid #d8d8e0; }"
    ".tracklist { background-color: #f4f4f8; }"
    ".track-item { background-color: #ffffff; border-radius: 8px; padding: 6px 10px; margin: 2px 8px; }"
    ".track-item:hover { background-color: #eaeaf0; }"
    ".track-active { background-color: #e8e4f5; border-left: 3px solid #6b4fb5; }"
    ".track-active:hover { background-color: #ded8f0; }"
    ".controls { background-color: #ffffff; border-top: 1px solid #d8d8e0; padding: 8px; }"
    ".subtitle { color: #999aaa; font-size: 12px; }"
    ".now-playing { color: #6b4fb5; font-style: italic; font-size: 12px; }"
    ".track-name { color: #333340; font-size: 13px; }"
    ".track-name-active { color: #5b3fb5; font-size: 13px; font-weight: bold; }"
    ".track-num { color: #999aaa; font-size: 12px; }"
    "button.ctrl { background-color: #e8e8f0; color: #333340; border: none; border-radius: 10px; min-width: 42px; min-height: 36px; font-size: 16px; }"
    "button.ctrl:hover { background-color: #d8d8e2; }"
    "button.ctrl:active { background-color: #c8c8d4; }"
    "button.play-btn { background-color: #6b4fb5; color: white; }"
    "button.play-btn:hover { background-color: #7d5fcc; }"
    "button.mode-btn { background-color: transparent; color: #999aaa; border: 1px solid #d0d0da; border-radius: 8px; font-size: 12px; padding: 4px 12px; }"
    "button.mode-btn:hover { background-color: #e8e8f0; color: #333340; }"
    "button.mode-active { background-color: #6b4fb5; color: white; border-color: #6b4fb5; }"
    "button.mode-active:hover { background-color: #7d5fcc; }"
    "button.icon-btn { background-color: transparent; color: #6b4fb5; border: none; font-size: 18px; padding: 4px; }"
    "button.icon-btn:hover { color: #5b3fb5; }"
    "button.danger-btn { background-color: transparent; color: #c04040; border: none; font-size: 12px; }"
    "button.danger-btn:hover { color: #e04040; }"
    "button.theme-btn { background-color: #e8e8f0; color: #4060c0; border: none; border-radius: 50%; min-width: 34px; min-height: 34px; font-size: 16px; }"
    "button.theme-btn:hover { background-color: #d8d8e2; }"
    "button.add-btn { background-color: transparent; color: #6b4fb5; border: none; font-size: 13px; }"
    "button.add-btn:hover { color: #5b3fb5; }"
    "button.skip-btn { font-size: 11px; min-width: 42px; color: #888899; }"
    "button.skip-btn:hover { background-color: #d8d8e2; color: #333340; }"
    "scale trough { background-color: #e0e0ea; border-radius: 4px; min-height: 6px; }"
    "scale slider { background-color: #6b4fb5; border-radius: 50%; min-width: 16px; min-height: 16px; }"
    "scale highlight { background-color: #6b4fb5; }"
    ".drop-area { background-color: #f8f8fc; border: 2px dashed #c0c0d0; border-radius: 16px; padding: 40px; }"
    ".drop-label { color: #999aaa; font-size: 15px; }"
    ".drop-icon { font-size: 42px; }"
    ".drop-or { color: #aab; font-size: 11px; }"
    "button.browse-btn { background-color: #6b4fb5; color: white; border: none; border-radius: 10px; padding: 8px 20px; font-size: 13px; }"
    "button.browse-btn:hover { background-color: #7d5fcc; }"
    "scrollbar { background-color: transparent; }"
    "scrollbar slider { background-color: #c0c0d0; border-radius: 4px; min-width: 6px; }"
;

static void apply_theme(AppState *app) {
    LOG_I("apply_theme: dark=%d", (int)app->dark);
    if (app->css) {
        gtk_style_context_remove_provider_for_screen(
            gdk_screen_get_default(),
            GTK_STYLE_PROVIDER(app->css));
        g_object_unref(app->css);
    }
    app->css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(app->css,
        app->dark ? DARK_CSS : LIGHT_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(
        gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(app->css),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

    if (app->btn_theme)
        gtk_button_set_label(GTK_BUTTON(app->btn_theme),
            app->dark ? "\u2600" : "\u263E");
}

/* ── UI callbacks ── */
static void on_play(GtkWidget *w, gpointer data) {
    (void)w;
    AppState *app = data;
    LOG_I("on_play clicked");
    if (app->track_count == 0) return;
    if (app->sound_loaded) {
        toggle_pause(app);
    } else {
        play_index(app, app->current >= 0 ? app->current : 0);
    }
}

static void on_prev(GtkWidget *w, gpointer data) {
    (void)w;
    AppState *app = data;
    if (app->track_count == 0) return;
    int prev = (app->current - 1 + app->track_count) % app->track_count;
    LOG_I("on_prev: going to %d", prev);
    play_index(app, prev);
}

static void on_next(GtkWidget *w, gpointer data) {
    (void)w;
    LOG_I("on_next clicked");
    AppState *app = data;
    play_next(app);
}

static void on_stop(GtkWidget *w, gpointer data) {
    (void)w;
    LOG_I("on_stop clicked");
    AppState *app = data;
    stop_playback(app);
    app->current = -1;
    update_now_label(app);
    refresh_track_list(app);
}

static void on_mode(GtkWidget *w, gpointer data) {
    (void)w;
    AppState *app = data;
    app->mode = (app->mode + 1) % 3;
    const char *labels[] = { "\u25B6 Tek", "\U0001F501 D\u00f6ng\u00fc", "\U0001F3B2 Rastgele" };
    LOG_I("on_mode: mode now %d (%s)", app->mode, labels[app->mode]);
    gtk_button_set_label(GTK_BUTTON(app->btn_mode), labels[app->mode]);
}

static void on_theme(GtkWidget *w, gpointer data) {
    (void)w;
    AppState *app = data;
    app->dark = !app->dark;
    LOG_I("on_theme: dark=%d", (int)app->dark);
    apply_theme(app);
}

static void on_volume(GtkWidget *w, gpointer data) {
    (void)w;
    AppState *app = data;
    app->volume = (float)gtk_range_get_value(GTK_RANGE(app->scale_vol));
    LOG_I("on_volume: %.2f", app->volume);
    if (app->sound_loaded)
        ma_sound_set_volume(&app->sound, app->volume);
}

/* ── seek ── */
static void on_seek(GtkWidget *w, gpointer data) {
    (void)w;
    AppState *app = data;
    LOG_I("on_seek: sound_loaded=%d, seeking=%d", (int)app->sound_loaded, (int)app->seeking);
    if (!app->sound_loaded || app->seeking) return;
    float pos = (float)gtk_range_get_value(GTK_RANGE(app->scale_seek));
    float total = 0;
    ma_sound_get_length_in_seconds(&app->sound, &total);
    LOG_I("on_seek: pos=%.4f, total=%.2f", pos, total);
    if (total <= 0) return;
    ma_result res = ma_sound_seek_to_second(&app->sound, pos * total);
    LOG_I("on_seek: seek result=%d", res);
}

static void on_skip(GtkWidget *w, gpointer data) {
    AppState *app = data;
    const char *label = gtk_button_get_label(GTK_BUTTON(w));
    LOG_I("on_skip: label='%s', sound_loaded=%d", label, (int)app->sound_loaded);
    if (!app->sound_loaded) return;
    int dir = (label[0] == '+') ? 1 : -1;
    float total = 0, cur = 0;
    ma_sound_get_length_in_seconds(&app->sound, &total);
    ma_sound_get_cursor_in_seconds(&app->sound, &cur);
    float target = cur + dir * 10.0f;
    if (target < 0) target = 0;
    if (target > total) target = total;
    LOG_I("on_skip: cur=%.2f, total=%.2f, target=%.2f", cur, total, target);
    ma_sound_seek_to_second(&app->sound, target);
}

/* ── file adding ── */
static void add_files(AppState *app, GList *file_list) {
    gboolean added = FALSE;
    for (GList *l = file_list; l; l = l->next) {
        char *uri = l->data;
        if (!g_str_has_prefix(uri, "file://")) continue;
        char *path = g_filename_from_uri(uri, NULL, NULL);
        if (!path) continue;
        const char *name = basename_of(path);
        if (!has_audio_ext(name)) { g_free(path); continue; }
        if (app->track_count >= MAX_TRACKS) { g_free(path); break; }
        LOG_I("add_files: adding '%s'", name);
        app->tracks[app->track_count].path = path;
        app->tracks[app->track_count].name = strdup_safe(name);
        app->track_count++;
        added = TRUE;
    }
    if (added) {
        refresh_track_list(app);
        update_now_label(app);
    }
}

static void add_paths_to_app(AppState *app, GList *paths) {
    gboolean added = FALSE;
    for (GList *l = paths; l; l = l->next) {
        const char *path = l->data;
        const char *name = basename_of(path);
        if (!has_audio_ext(name)) continue;
        if (app->track_count >= MAX_TRACKS) break;
        LOG_I("add_paths: adding '%s'", name);
        app->tracks[app->track_count].path = strdup_safe(path);
        app->tracks[app->track_count].name = strdup_safe(name);
        app->track_count++;
        added = TRUE;
    }
    if (added) {
        refresh_track_list(app);
        update_now_label(app);
    }
}

/* ── drag & drop ── */
static void on_drag_data_received(GtkWidget *w, GdkDragContext *ctx,
                                  gint x, gint y,
                                  GtkSelectionData *data,
                                  guint info, guint time,
                                  gpointer user_data) {
    (void)w; (void)x; (void)y; (void)info;
    AppState *app = user_data;
    char **uris = gtk_selection_data_get_uris(data);
    LOG_I("on_drag_data_received");
    if (uris) {
        GList *list = NULL;
        for (int i = 0; uris[i]; i++)
            list = g_list_append(list, uris[i]);
        add_files(app, list);
        g_free(uris);
    }
    gtk_drag_finish(ctx, TRUE, FALSE, time);
}

/* ── browse button ── */
static void on_browse(GtkWidget *w, gpointer data) {
    (void)w;
    AppState *app = data;
    LOG_I("on_browse: opening file dialog");
    GtkWidget *dlg = gtk_file_chooser_dialog_new(
        "Ses Dosyalari Sec",
        GTK_WINDOW(app->window),
        GTK_FILE_CHOOSER_ACTION_OPEN,
        "_Iptal", GTK_RESPONSE_CANCEL,
        "_Ac", GTK_RESPONSE_ACCEPT,
        NULL);

    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Ses Dosyalari");
    gtk_file_filter_add_pattern(filter, "*.mp3");
    gtk_file_filter_add_pattern(filter, "*.wav");
    gtk_file_filter_add_pattern(filter, "*.ogg");
    gtk_file_filter_add_pattern(filter, "*.flac");
    gtk_file_filter_add_pattern(filter, "*.aac");
    gtk_file_filter_add_pattern(filter, "*.m4a");
    gtk_file_filter_add_pattern(filter, "*.opus");
    gtk_file_filter_add_pattern(filter, "*.wma");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dlg), filter);

    gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dlg), TRUE);

    if (gtk_dialog_run(GTK_DIALOG(dlg)) == GTK_RESPONSE_ACCEPT) {
        GSList *files = gtk_file_chooser_get_filenames(GTK_FILE_CHOOSER(dlg));
        GList *paths = NULL;
        for (GSList *l = files; l; l = l->next)
            paths = g_list_append(paths, l->data);
        add_paths_to_app(app, paths);
        g_slist_free_full(files, g_free);
    }
    gtk_widget_destroy(dlg);
}

/* ── track list click ── */
static void on_track_click(GtkWidget *row, GdkEventButton *ev, gpointer data) {
    AppState *app = data;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row), "idx"));
    if (ev->type == GDK_2BUTTON_PRESS) {
        LOG_I("on_track_click: double-click on idx=%d", idx);
        play_index(app, idx);
    }
}

static void on_play_btn(GtkWidget *btn, gpointer data) {
    AppState *app = data;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "idx"));
    LOG_I("on_play_btn: idx=%d", idx);
    play_index(app, idx);
}

static void on_remove_btn(GtkWidget *btn, gpointer data) {
    AppState *app = data;
    int idx = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(btn), "idx"));
    LOG_I("on_remove_btn: idx=%d", idx);
    if (idx < 0 || idx >= app->track_count) return;

    gboolean was_current = (app->current == idx);

    free(app->tracks[idx].path);
    free(app->tracks[idx].name);
    for (int i = idx; i < app->track_count - 1; i++)
        app->tracks[i] = app->tracks[i + 1];
    app->track_count--;

    if (was_current) {
        stop_playback(app);
        app->current = -1;
    } else if (app->current > idx) {
        app->current--;
    }

    refresh_track_list(app);
    update_now_label(app);
}

static void on_clear_all(GtkWidget *w, gpointer data) {
    (void)w;
    AppState *app = data;
    LOG_I("on_clear_all");
    stop_playback(app);
    for (int i = 0; i < app->track_count; i++) {
        free(app->tracks[i].path);
        free(app->tracks[i].name);
    }
    app->track_count = 0;
    app->current = -1;
    refresh_track_list(app);
    update_now_label(app);
}

/* ── UI building ── */
static void update_now_label(AppState *app) {
    if (app->current >= 0 && app->current < app->track_count && app->playing) {
        char buf[256];
        snprintf(buf, sizeof(buf), "\u25B6 %s", app->tracks[app->current].name);
        if (strlen(buf) > 40) buf[39] = '\0';
        gtk_label_set_text(GTK_LABEL(app->lbl_now), buf);
    } else if (app->current >= 0 && app->current < app->track_count) {
        char buf[256];
        snprintf(buf, sizeof(buf), "\u25B8 %s", app->tracks[app->current].name);
        if (strlen(buf) > 40) buf[39] = '\0';
        gtk_label_set_text(GTK_LABEL(app->lbl_now), buf);
    } else {
        gtk_label_set_text(GTK_LABEL(app->lbl_now), "");
    }
}

static void refresh_track_list(AppState *app) {
    LOG_I("refresh_track_list: track_count=%d, current=%d, playing=%d",
          app->track_count, app->current, (int)app->playing);

    /* clear existing children */
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->track_list));
    int n = g_list_length(children);
    for (GList *l = children; l; l = l->next)
        gtk_widget_destroy(GTK_WIDGET(l->data));
    g_list_free(children);
    LOG_I("refresh_track_list: destroyed %d old children", n);

    char count_buf[64];
    snprintf(count_buf, sizeof(count_buf), "%d dosya", app->track_count);
    gtk_label_set_text(GTK_LABEL(app->lbl_count), count_buf);

    if (app->track_count == 0) {
        LOG_I("refresh_track_list: switching to drop view");
        gtk_stack_set_visible_child_name(GTK_STACK(app->center_stack), "drop");
        return;
    }

    LOG_I("refresh_track_list: switching to tracks view");
    gtk_stack_set_visible_child_name(GTK_STACK(app->center_stack), "tracks");

    for (int i = 0; i < app->track_count; i++) {
        gboolean active = (app->current == i);

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_set_name(row, "track-row");
        GtkStyleContext *ctx = gtk_widget_get_style_context(row);
        gtk_style_context_add_class(ctx, "track-item");
        if (active) gtk_style_context_add_class(ctx, "track-active");
        gtk_widget_set_events(row, GDK_BUTTON_PRESS_MASK);
        g_signal_connect(row, "button-press-event", G_CALLBACK(on_track_click), app);
        g_object_set_data(G_OBJECT(row), "idx", GINT_TO_POINTER(i));

        /* number/indicator */
        GtkWidget *lbl_num = gtk_label_new(active ? (app->playing ? "\u266B" : "\u25B8") : NULL);
        GtkStyleContext *num_ctx = gtk_widget_get_style_context(lbl_num);
        gtk_style_context_add_class(num_ctx, "track-num");
        if (!active) {
            char nb[8];
            snprintf(nb, sizeof(nb), "%d", i + 1);
            gtk_label_set_text(GTK_LABEL(lbl_num), nb);
        }
        gtk_box_pack_start(GTK_BOX(row), lbl_num, FALSE, FALSE, 4);

        /* play mini button */
        GtkWidget *btn_play_mini = gtk_button_new_with_label("\u25B6");
        GtkStyleContext *pctx = gtk_widget_get_style_context(btn_play_mini);
        gtk_style_context_add_class(pctx, "icon-btn");
        g_object_set_data(G_OBJECT(btn_play_mini), "idx", GINT_TO_POINTER(i));
        g_signal_connect(btn_play_mini, "clicked", G_CALLBACK(on_play_btn), app);
        gtk_box_pack_start(GTK_BOX(row), btn_play_mini, FALSE, FALSE, 0);

        /* name */
        GtkWidget *lbl_name = gtk_label_new(app->tracks[i].name);
        gtk_label_set_ellipsize(GTK_LABEL(lbl_name), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(lbl_name), 40);
        GtkStyleContext *nctx = gtk_widget_get_style_context(lbl_name);
        gtk_style_context_add_class(nctx, active ? "track-name-active" : "track-name");
        gtk_box_pack_start(GTK_BOX(row), lbl_name, TRUE, TRUE, 0);

        /* remove button */
        GtkWidget *btn_rm = gtk_button_new_with_label("\u2715");
        GtkStyleContext *rctx = gtk_widget_get_style_context(btn_rm);
        gtk_style_context_add_class(rctx, "danger-btn");
        g_object_set_data(G_OBJECT(btn_rm), "idx", GINT_TO_POINTER(i));
        g_signal_connect(btn_rm, "clicked", G_CALLBACK(on_remove_btn), app);
        gtk_box_pack_end(GTK_BOX(row), btn_rm, FALSE, FALSE, 0);

        gtk_container_add(GTK_CONTAINER(app->track_list), row);
    }

    gtk_widget_show_all(app->track_list);
    /* make sure controls stay visible */
    gtk_widget_show_all(app->controls_box);
    LOG_I("refresh_track_list: done, showed track_list and controls");
}

static void build_ui(AppState *app) {
    LOG_I("build_ui: starting");
    app->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(app->window), "DropPlayer");
    gtk_window_set_default_size(GTK_WINDOW(app->window), 500, 560);
    gtk_window_set_position(GTK_WINDOW(app->window), GTK_WIN_POS_CENTER);
    GtkStyleContext *wctx = gtk_widget_get_style_context(app->window);
    gtk_style_context_add_class(wctx, "window");
    g_signal_connect(app->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

    /* drag & drop */
    GtkTargetEntry targets[] = { { "text/uri-list", 0, 0 } };
    gtk_drag_dest_set(app->window, GTK_DEST_DEFAULT_ALL, targets, 1, GDK_ACTION_COPY);
    g_signal_connect(app->window, "drag-data-received", G_CALLBACK(on_drag_data_received), app);

    /* main vertical box */
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(app->window), vbox);

    /* ── TOP BAR ── */
    GtkWidget *topbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkStyleContext *tbctx = gtk_widget_get_style_context(topbar);
    gtk_style_context_add_class(tbctx, "topbar");
    gtk_widget_set_margin_start(topbar, 14);
    gtk_widget_set_margin_end(topbar, 14);
    gtk_widget_set_margin_top(topbar, 10);
    gtk_widget_set_margin_bottom(topbar, 10);
    gtk_box_pack_start(GTK_BOX(vbox), topbar, FALSE, FALSE, 0);

    GtkWidget *lbl_title = gtk_label_new(NULL);
    gtk_label_set_markup(GTK_LABEL(lbl_title),
        "<span foreground='#7c5cbf' size='x-large' weight='bold'>\u266B DropPlayer</span>");
    gtk_box_pack_start(GTK_BOX(topbar), lbl_title, FALSE, FALSE, 0);

    GtkWidget *spacer = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(topbar), spacer, TRUE, TRUE, 0);

    app->btn_theme = gtk_button_new_with_label("\u2600");
    GtkStyleContext *thctx = gtk_widget_get_style_context(app->btn_theme);
    gtk_style_context_add_class(thctx, "theme-btn");
    g_signal_connect(app->btn_theme, "clicked", G_CALLBACK(on_theme), app);
    gtk_box_pack_end(GTK_BOX(topbar), app->btn_theme, FALSE, FALSE, 0);

    /* ── count + add bar ── */
    GtkWidget *info_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_start(info_bar, 14);
    gtk_widget_set_margin_end(info_bar, 14);
    gtk_widget_set_margin_top(info_bar, 6);
    gtk_box_pack_start(GTK_BOX(vbox), info_bar, FALSE, FALSE, 0);

    app->lbl_count = gtk_label_new("0 dosya");
    GtkStyleContext *cctx = gtk_widget_get_style_context(app->lbl_count);
    gtk_style_context_add_class(cctx, "subtitle");
    gtk_box_pack_start(GTK_BOX(info_bar), app->lbl_count, FALSE, FALSE, 0);

    GtkWidget *btn_add = gtk_button_new_with_label("+ Dosya Ekle");
    GtkStyleContext *actx = gtk_widget_get_style_context(btn_add);
    gtk_style_context_add_class(actx, "add-btn");
    g_signal_connect(btn_add, "clicked", G_CALLBACK(on_browse), app);
    gtk_box_pack_end(GTK_BOX(info_bar), btn_add, FALSE, FALSE, 0);

    GtkWidget *btn_clear = gtk_button_new_with_label("Temizle");
    GtkStyleContext *clctx = gtk_widget_get_style_context(btn_clear);
    gtk_style_context_add_class(clctx, "danger-btn");
    g_signal_connect(btn_clear, "clicked", G_CALLBACK(on_clear_all), app);
    gtk_box_pack_end(GTK_BOX(info_bar), btn_clear, FALSE, FALSE, 0);

    /* separator */
    GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_margin_start(sep1, 8);
    gtk_widget_set_margin_end(sep1, 8);
    gtk_box_pack_start(GTK_BOX(vbox), sep1, FALSE, FALSE, 0);

    /* ── CENTER: track list / drop zone via GtkStack ── */
    app->scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(app->scroll),
        GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), app->scroll, TRUE, TRUE, 0);

    app->center_stack = gtk_stack_new();
    gtk_stack_set_transition_type(GTK_STACK(app->center_stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(app->center_stack), 200);

    /* drop box */
    app->drop_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    GtkStyleContext *dbctx = gtk_widget_get_style_context(app->drop_box);
    gtk_style_context_add_class(dbctx, "drop-area");
    gtk_widget_set_halign(app->drop_box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(app->drop_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(app->drop_box, 40);
    gtk_widget_set_margin_end(app->drop_box, 40);

    GtkWidget *drop_icon = gtk_label_new("\U0001F4C2");
    GtkStyleContext *di_ctx = gtk_widget_get_style_context(drop_icon);
    gtk_style_context_add_class(di_ctx, "drop-icon");
    gtk_box_pack_start(GTK_BOX(app->drop_box), drop_icon, FALSE, FALSE, 0);

    GtkWidget *drop_lbl = gtk_label_new("Ses dosyalarini buraya surukle");
    GtkStyleContext *dlctx = gtk_widget_get_style_context(drop_lbl);
    gtk_style_context_add_class(dlctx, "drop-label");
    gtk_box_pack_start(GTK_BOX(app->drop_box), drop_lbl, FALSE, FALSE, 0);

    GtkWidget *drop_or = gtk_label_new("veya");
    GtkStyleContext *doctx = gtk_widget_get_style_context(drop_or);
    gtk_style_context_add_class(doctx, "drop-or");
    gtk_box_pack_start(GTK_BOX(app->drop_box), drop_or, FALSE, FALSE, 0);

    GtkWidget *btn_browse = gtk_button_new_with_label("Dosya Sec");
    GtkStyleContext *bbctx = gtk_widget_get_style_context(btn_browse);
    gtk_style_context_add_class(bbctx, "browse-btn");
    g_signal_connect(btn_browse, "clicked", G_CALLBACK(on_browse), app);
    gtk_box_pack_start(GTK_BOX(app->drop_box), btn_browse, FALSE, FALSE, 4);

    /* track list */
    app->track_list = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkStyleContext *tlctx = gtk_widget_get_style_context(app->track_list);
    gtk_style_context_add_class(tlctx, "tracklist");

    gtk_stack_add_named(GTK_STACK(app->center_stack), app->drop_box, "drop");
    gtk_stack_add_named(GTK_STACK(app->center_stack), app->track_list, "tracks");
    gtk_stack_set_visible_child_name(GTK_STACK(app->center_stack), "drop");

    gtk_container_add(GTK_CONTAINER(app->scroll), app->center_stack);

    /* ── BOTTOM CONTROLS ── */
    app->controls_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkStyleContext *ctlctx = gtk_widget_get_style_context(app->controls_box);
    gtk_style_context_add_class(ctlctx, "controls");
    gtk_widget_set_margin_start(app->controls_box, 12);
    gtk_widget_set_margin_end(app->controls_box, 12);
    gtk_widget_set_margin_top(app->controls_box, 8);
    gtk_widget_set_margin_bottom(app->controls_box, 10);
    gtk_box_pack_start(GTK_BOX(vbox), app->controls_box, FALSE, FALSE, 0);

    /* seek bar row */
    GtkWidget *seek_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(seek_row, 4);
    gtk_widget_set_margin_end(seek_row, 4);
    gtk_box_pack_start(GTK_BOX(app->controls_box), seek_row, FALSE, FALSE, 0);

    app->lbl_time = gtk_label_new("0:00");
    GtkStyleContext *ltctx = gtk_widget_get_style_context(app->lbl_time);
    gtk_style_context_add_class(ltctx, "subtitle");
    gtk_box_pack_start(GTK_BOX(seek_row), app->lbl_time, FALSE, FALSE, 0);

    app->scale_seek = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.001);
    gtk_scale_set_draw_value(GTK_SCALE(app->scale_seek), FALSE);
    gtk_widget_set_size_request(app->scale_seek, -1, 10);
    g_signal_connect(app->scale_seek, "value-changed", G_CALLBACK(on_seek), app);
    gtk_box_pack_start(GTK_BOX(seek_row), app->scale_seek, TRUE, TRUE, 0);

    /* volume row */
    GtkWidget *vol_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(app->controls_box), vol_box, FALSE, FALSE, 0);

    GtkWidget *vol_icon = gtk_label_new("\U0001F50A");
    gtk_box_pack_start(GTK_BOX(vol_box), vol_icon, FALSE, FALSE, 4);

    app->scale_vol = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 1.0, 0.01);
    gtk_range_set_value(GTK_RANGE(app->scale_vol), app->volume);
    gtk_scale_set_draw_value(GTK_SCALE(app->scale_vol), FALSE);
    gtk_widget_set_size_request(app->scale_vol, 130, -1);
    g_signal_connect(app->scale_vol, "value-changed", G_CALLBACK(on_volume), app);
    gtk_box_pack_start(GTK_BOX(vol_box), app->scale_vol, FALSE, FALSE, 0);

    /* controls row: prev -10s play +10s stop next ... mode */
    GtkWidget *ctrl_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(app->controls_box), ctrl_row, FALSE, FALSE, 0);

    app->btn_prev = gtk_button_new_with_label("\u23EE");
    gtk_style_context_add_class(gtk_widget_get_style_context(app->btn_prev), "ctrl");
    g_signal_connect(app->btn_prev, "clicked", G_CALLBACK(on_prev), app);
    gtk_box_pack_start(GTK_BOX(ctrl_row), app->btn_prev, FALSE, FALSE, 0);

    GtkWidget *btn_skip_back = gtk_button_new_with_label("-10s");
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_skip_back), "ctrl");
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_skip_back), "skip-btn");
    g_signal_connect(btn_skip_back, "clicked", G_CALLBACK(on_skip), app);
    gtk_box_pack_start(GTK_BOX(ctrl_row), btn_skip_back, FALSE, FALSE, 0);

    app->btn_play = gtk_button_new_with_label("\u25B6");
    gtk_style_context_add_class(gtk_widget_get_style_context(app->btn_play), "ctrl");
    gtk_style_context_add_class(gtk_widget_get_style_context(app->btn_play), "play-btn");
    g_signal_connect(app->btn_play, "clicked", G_CALLBACK(on_play), app);
    gtk_box_pack_start(GTK_BOX(ctrl_row), app->btn_play, FALSE, FALSE, 0);

    GtkWidget *btn_skip_fwd = gtk_button_new_with_label("+10s");
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_skip_fwd), "ctrl");
    gtk_style_context_add_class(gtk_widget_get_style_context(btn_skip_fwd), "skip-btn");
    g_signal_connect(btn_skip_fwd, "clicked", G_CALLBACK(on_skip), app);
    gtk_box_pack_start(GTK_BOX(ctrl_row), btn_skip_fwd, FALSE, FALSE, 0);

    app->btn_stop = gtk_button_new_with_label("\u23F9");
    gtk_style_context_add_class(gtk_widget_get_style_context(app->btn_stop), "ctrl");
    g_signal_connect(app->btn_stop, "clicked", G_CALLBACK(on_stop), app);
    gtk_box_pack_start(GTK_BOX(ctrl_row), app->btn_stop, FALSE, FALSE, 0);

    app->btn_next = gtk_button_new_with_label("\u23ED");
    gtk_style_context_add_class(gtk_widget_get_style_context(app->btn_next), "ctrl");
    g_signal_connect(app->btn_next, "clicked", G_CALLBACK(on_next), app);
    gtk_box_pack_start(GTK_BOX(ctrl_row), app->btn_next, FALSE, FALSE, 0);

    GtkWidget *ctrl_spacer = gtk_label_new(NULL);
    gtk_box_pack_start(GTK_BOX(ctrl_row), ctrl_spacer, TRUE, TRUE, 0);

    app->btn_mode = gtk_button_new_with_label("\U0001F501 D\u00f6ng\u00fc");
    gtk_style_context_add_class(gtk_widget_get_style_context(app->btn_mode), "mode-btn");
    g_signal_connect(app->btn_mode, "clicked", G_CALLBACK(on_mode), app);
    gtk_box_pack_start(GTK_BOX(ctrl_row), app->btn_mode, FALSE, FALSE, 0);

    /* now-playing label */
    GtkWidget *now_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(app->controls_box), now_row, FALSE, FALSE, 0);

    app->lbl_now = gtk_label_new("");
    GtkStyleContext *nwctx = gtk_widget_get_style_context(app->lbl_now);
    gtk_style_context_add_class(nwctx, "now-playing");
    gtk_label_set_ellipsize(GTK_LABEL(app->lbl_now), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(app->lbl_now), 45);
    gtk_box_pack_start(GTK_BOX(now_row), app->lbl_now, FALSE, FALSE, 0);

    apply_theme(app);
    LOG_I("build_ui: done");
}

/* ── periodic update ── */
static gboolean check_playback(gpointer data) {
    AppState *app = data;

    /* check if track ended */
    if (app->sound_loaded && app->playing) {
        /* use ma_sound_at_end to detect finished playback */
        if (ma_sound_at_end(&app->sound)) {
            LOG_I("check_playback: track ended");
            app->playing = FALSE;
            refresh_track_list(app);
            update_now_label(app);
            /* auto-advance is handled by sound_end_callback */
        }
    }

    /* update play button label */
    if (app->btn_play) {
        gtk_button_set_label(GTK_BUTTON(app->btn_play),
            app->playing ? "\u23F8" : "\u25B6");
    }

    /* update seek bar */
    if (app->sound_loaded && !app->seeking) {
        float total = 0, cur = 0;
        ma_sound_get_length_in_seconds(&app->sound, &total);
        ma_sound_get_cursor_in_seconds(&app->sound, &cur);
        if (total > 0) {
            float pos = cur / total;
            char buf[32];
            snprintf(buf, sizeof(buf), "%d:%02d / %d:%02d",
                (int)cur / 60, (int)cur % 60,
                (int)total / 60, (int)total % 60);
            gtk_label_set_text(GTK_LABEL(app->lbl_time), buf);
            g_signal_handlers_block_by_func(app->scale_seek, on_seek, app);
            gtk_range_set_value(GTK_RANGE(app->scale_seek), pos);
            g_signal_handlers_unblock_by_func(app->scale_seek, on_seek, app);
        }
    }

    /* update mode button style */
    if (app->btn_mode) {
        GtkStyleContext *mctx = gtk_widget_get_style_context(app->btn_mode);
        if (!gtk_style_context_has_class(mctx, "mode-active"))
            gtk_style_context_add_class(mctx, "mode-active");
    }

    return G_SOURCE_CONTINUE;
}

int main(int argc, char *argv[]) {
    /* setup logging */
    g_logfile = fopen(LOG_FILE, "w");
    LOG_I("=== DropPlayer starting ===");
    LOG_I("pid=%d", getpid());

    /* setup crash handler */
    signal(SIGSEGV, crash_handler);
    signal(SIGABRT, crash_handler);
    signal(SIGFPE, crash_handler);
    signal(SIGBUS, crash_handler);

    srand((unsigned)time(NULL));
    gtk_init(&argc, &argv);

    AppState app = {0};
    app.current = -1;
    app.volume = 0.7f;
    app.dark = TRUE;
    app.mode = MODE_LOOP;

    /* init miniaudio engine */
    ma_engine_config ec = ma_engine_config_init();
    ec.channels = 2;
    ec.sampleRate = 44100;
    LOG_I("Initializing miniaudio engine...");
    if (ma_engine_init(&ec, &app.engine) != MA_SUCCESS) {
        LOG_E("miniaudio engine init FAILED");
        return 1;
    }
    LOG_I("miniaudio engine OK");

    build_ui(&app);
    gtk_widget_show_all(app.window);
    LOG_I("Window shown");

    /* start update timer */
    g_timeout_add(250, check_playback, &app);

    /* handle file arguments */
    if (argc > 1) {
        LOG_I("Got %d file arguments", argc - 1);
        GList *paths = NULL;
        for (int i = 1; i < argc; i++) {
            LOG_I("  arg[%d] = %s", i, argv[i]);
            paths = g_list_append(paths, argv[i]);
        }
        add_paths_to_app(&app, paths);
        g_list_free(paths);
        if (app.track_count > 0)
            play_index(&app, 0);
    }

    gtk_main();
    LOG_I("gtk_main returned, cleaning up");
    stop_playback(&app);
    ma_engine_uninit(&app.engine);

    for (int i = 0; i < app.track_count; i++) {
        free(app.tracks[i].path);
        free(app.tracks[i].name);
    }

    if (g_logfile) fclose(g_logfile);
    return 0;
}
