/**
 * Quadrature Indexer Bridge
 *
 * Indexer signal handlers, phase state machine, progress UI.
 * Manages the visual progress display for library scanning phases.
 */

#define G_LOG_DOMAIN "quadrature"

#include "../internal.h"
#include "internal.h"
#include "../search/internal.h"
#include <string.h>


/* ═══════════════════════════════════════════════════════════════════════════
 * Progress Display Helpers
 * ═══════════════════════════════════════════════════════════════════════════ */

static void format_rate_eta(double rate_smoothed,
                            size_t processed, size_t total,
                            const char *unit,
                            char *out_buf, size_t buf_size) {
    if (rate_smoothed <= 0.0 || processed == 0) {
        snprintf(out_buf, buf_size, "Calculating...");
        return;
    }
    size_t remaining = total - processed;
    double eta_sec = remaining / rate_smoothed;
    if (eta_sec < 60)
        snprintf(out_buf, buf_size, "%.1f %s/sec · ~%.0fs remaining", rate_smoothed, unit, eta_sec);
    else
        snprintf(out_buf, buf_size, "%.1f %s/sec · ~%.1fm remaining", rate_smoothed, unit, eta_sec / 60.0);
}

/* ── PhaseRow state machine transitions ──────────────────────────────────── */

static void set_phase_css(PhaseRow *ph, const char *class) {
    if (!ph->container) return;
    gtk_widget_remove_css_class(ph->container, "progress-phase-dim");
    gtk_widget_remove_css_class(ph->container, "progress-phase-active");
    gtk_widget_remove_css_class(ph->container, "progress-phase-complete");
    gtk_widget_remove_css_class(ph->container, "progress-phase-error");
    gtk_widget_remove_css_class(ph->container, "progress-phase-skipped");
    if (class && class[0])
        gtk_widget_add_css_class(ph->container, class);
}

static void phase_reset(PhaseRow *ph) {
    ph->state    = PHASE_WAITING;
    ph->start_us = 0;
    ph->end_us   = 0;
    ph->prev_count = 0;
    ph->prev_time  = 0;
    ph->rate_ema   = 0.0;
    if (!ph->container) return;
    set_phase_css(ph, "progress-phase-dim");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ph->bar), 0.0);
    gtk_label_set_text(GTK_LABEL(ph->label), "Waiting");
    gtk_label_set_text(GTK_LABEL(ph->rate_label), "");
    gtk_widget_set_visible(ph->rate_label, FALSE);
}

static void phase_activate(PhaseRow *ph) {
    if (ph->state != PHASE_WAITING && ph->state != PHASE_STARTUP) return;
    ph->state    = PHASE_ACTIVE;
    if (ph->start_us == 0)
        ph->start_us = g_get_monotonic_time();
    set_phase_css(ph, "progress-phase-active");
}

static void phase_startup(PhaseRow *ph, const char *message) {
    if (ph->state != PHASE_WAITING) return;
    ph->state    = PHASE_STARTUP;
    ph->start_us = g_get_monotonic_time();
    set_phase_css(ph, "progress-phase-active");
    gtk_label_set_text(GTK_LABEL(ph->label), message ? message : "Starting...");
    gtk_widget_set_visible(ph->rate_label, FALSE);
}

static void phase_complete(PhaseRow *ph, size_t total, const char *unit) {
    if (ph->state == PHASE_COMPLETE || ph->state == PHASE_SKIPPED
        || ph->state == PHASE_ERROR)
        return;
    ph->state = PHASE_COMPLETE;
    set_phase_css(ph, "progress-phase-complete");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ph->bar), 1.0);

    /* Always show count when available */
    if (total > 0) {
        char count_buf[64];
        snprintf(count_buf, sizeof(count_buf), "%zu %s", total, unit);
        gtk_label_set_text(GTK_LABEL(ph->label), count_buf);
    } else {
        gtk_label_set_text(GTK_LABEL(ph->label), "Complete");
    }

    /* Show rate/duration only when we have valid timing */
    if (ph->start_us > 0 && total > 0) {
        if (ph->end_us == 0)
            ph->end_us = g_get_monotonic_time();
        int64_t elapsed_us = ph->end_us - ph->start_us;
        double elapsed_s   = elapsed_us / 1e6;
        double avg_rate    = elapsed_s > 0.0 ? (double)total / elapsed_s : 0.0;

        char rate_buf[80];
        if (elapsed_s < 60)
            snprintf(rate_buf, sizeof(rate_buf), "%.0fs · %.1f %s/sec",
                     elapsed_s, avg_rate, unit);
        else
            snprintf(rate_buf, sizeof(rate_buf), "%.1fm · %.1f %s/sec",
                     elapsed_s / 60.0, avg_rate, unit);
        gtk_label_set_text(GTK_LABEL(ph->rate_label), rate_buf);
        gtk_widget_set_visible(ph->rate_label, TRUE);
    } else {
        gtk_widget_set_visible(ph->rate_label, FALSE);
    }
}

static void phase_skip(PhaseRow *ph) {
    if (ph->state == PHASE_COMPLETE || ph->state == PHASE_SKIPPED
        || ph->state == PHASE_ERROR)
        return;
    ph->state = PHASE_SKIPPED;
    set_phase_css(ph, "progress-phase-skipped");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ph->bar), 0.0);
    gtk_label_set_text(GTK_LABEL(ph->label), "Up to date");
    gtk_widget_set_visible(ph->rate_label, FALSE);
}

static void phase_error(PhaseRow *ph, const char *msg) {
    ph->state = PHASE_ERROR;
    set_phase_css(ph, "progress-phase-error");
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ph->bar), 1.0);
    gtk_label_set_text(GTK_LABEL(ph->label), msg ? msg : "Error");
    gtk_widget_set_visible(ph->rate_label, FALSE);
}

static void phase_update(PhaseRow *ph, size_t processed, size_t total,
                          const char *unit) {
    if (ph->state != PHASE_ACTIVE) return;

    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ph->bar),
        total > 0 ? (double)processed / total : 0.0);

    char buf[128];
    snprintf(buf, sizeof(buf), "%zu/%zu", processed, total);
    gtk_label_set_text(GTK_LABEL(ph->label), buf);

    /* Cumulative average rate: total_processed / total_elapsed.
     * More stable than EMA for bursty batch workloads (e.g. MB resolver
     * fetches 50 albums from PG, then writes them rapidly). */
    int64_t now = g_get_monotonic_time();
    double elapsed_s = (now - ph->start_us) / 1e6;
    if (elapsed_s > 2.0 && processed > 0)
        ph->rate_ema = (double)processed / elapsed_s;
    ph->prev_count = processed;
    ph->prev_time  = now;

    char rate_buf[128];
    format_rate_eta(ph->rate_ema, processed, total, unit, rate_buf, sizeof(rate_buf));
    gtk_label_set_text(GTK_LABEL(ph->rate_label), rate_buf);
    gtk_widget_set_visible(ph->rate_label, TRUE);
}

/* Safety net: finalize a phase that may still be WAITING/ACTIVE when
 * LIBRARY_READY arrives before the final throttled progress tick.
 * indexer_start_us: the indexer's phase_start_times[] value for this phase.
 *   If the phase was never activated in the UI (start_us == 0), this seeds
 *   the start time so phase_complete can compute rate/duration. Pass 0 if
 *   unavailable. */
static void finalize_phase(PhaseRow *ph, size_t count, const char *unit,
                           int64_t indexer_start_us) {
    if (ph->state == PHASE_COMPLETE || ph->state == PHASE_SKIPPED
        || ph->state == PHASE_ERROR)
        return;
    /* Backfill start time from indexer when UI never saw a progress tick */
    if (ph->start_us == 0 && indexer_start_us > 0)
        ph->start_us = indexer_start_us;
    if (count > 0)
        phase_complete(ph, count, unit);
    else
        phase_skip(ph);
}

/* Build a phase row widget and fill in the ProgressPhaseWidgets struct */

/* Format last-indexed timestamp as "Last scanned Feb 18, 2026" */
static void format_last_indexed(int64_t unix_time, char *buf, size_t size) {
    if (unix_time <= 0) { snprintf(buf, size, "Never scanned"); return; }
    time_t t = (time_t)unix_time;
    struct tm *tm_info = localtime(&t);
    char date_buf[64];
    strftime(date_buf, sizeof(date_buf), "%b %e, %Y", tm_info);
    snprintf(buf, size, "Last scanned %s", date_buf);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Libraries
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Async library stats loading — runs DB queries off the main thread */

typedef struct {
    char *db_root;       /* Data root for DB path (owned) */
    char *lib_path;      /* Library root path for re-lookup (owned) */
    UiWindow *window;    /* Window ref for re-lookup (never freed during app) */
    size_t tracks;
    size_t albums;
    size_t artists;
    int64_t last_indexed_time;
    size_t errors;
} LibStatsData;

static void lib_stats_data_free(LibStatsData *d) {
    g_free(d->db_root);
    g_free(d->lib_path);
    g_free(d);
}

static void lib_stats_thread(GTask *task, gpointer src, gpointer data, GCancellable *c) {
    (void)src; (void)c;
    LibStatsData *d = data;
    char *dbpath = g_build_filename(d->db_root, "quadrature.sqlite", NULL);
    quadrature_db_t *lib_db = NULL;

    if (g_file_test(dbpath, G_FILE_TEST_EXISTS) &&
        db_open_readonly(dbpath, &lib_db) == QUADRATURE_OK) {
        db_get_total_track_count(lib_db, &d->tracks);
        db_get_total_album_count(lib_db, &d->albums);
        db_get_total_artist_count(lib_db, &d->artists);
        db_get_last_indexed_time(lib_db, &d->last_indexed_time);
        db_get_error_count(lib_db, "", &d->errors);
        db_close(lib_db);
    }
    g_free(dbpath);
    g_task_return_pointer(task, d, NULL);
}

static void lib_stats_done(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src; (void)data;
    GError *err = NULL;
    LibStatsData *d = g_task_propagate_pointer(G_TASK(res), &err);
    if (err) { g_error_free(err); lib_stats_data_free(d); return; }

    /* Re-lookup the LibEntry by path — the original pointer may be stale
     * if libs_load() was called between task start and completion. */
    LibEntry *e = find_lib_entry(d->window, d->lib_path);
    if (!e) {
        /* Library was removed while stats were loading — discard */
        lib_stats_data_free(d);
        return;
    }

    e->tracks = d->tracks;
    e->albums = d->albums;
    e->artists = d->artists;
    e->last_indexed_time = d->last_indexed_time;
    e->errors = d->errors;
    update_card_stats_labels(e);

    /* Show deferred "Library Loaded" toast now that stats are populated */
    if (e->pending_load_toast) {
        e->pending_load_toast = FALSE;
        char *folder = g_path_get_basename(e->path);
        char *markup = g_markup_printf_escaped(
            "<b>Library \"%s\" Loaded</b>\n"
            "<span size=\"small\" alpha=\"70%%\">%zu Albums · %zu Tracks · Processing artwork…</span>",
            folder, e->albums, e->tracks);
        g_free(folder);
        ui_window_show_toast_markup(d->window, markup, TOAST_SUCCESS, 5000);
        g_free(markup);
    }

    lib_stats_data_free(d);
}

void libs_load_entry_stats(LibEntry *e, UiWindow *w) {
    /* Reset stats immediately so stale values never persist */
    e->tracks = 0;
    e->albums = 0;
    e->artists = 0;
    e->last_indexed_time = 0;
    e->errors = 0;

    LibStatsData *d = g_new0(LibStatsData, 1);
    d->db_root  = g_strdup(e->data_path ? e->data_path : e->path);
    d->lib_path = g_strdup(e->path);
    d->window   = w;

    GTask *task = g_task_new(NULL, NULL, lib_stats_done, NULL);
    g_task_set_task_data(task, d, NULL);
    g_task_run_in_thread(task, lib_stats_thread);
    g_object_unref(task);
}

/* Push current LibEntry stat values into the stats panel labels */
void update_card_stats_labels(LibEntry *e) {
    char buf[128];

    snprintf(buf, sizeof(buf), "%zu tracks", e->tracks);
    gtk_label_set_text(GTK_LABEL(e->stat_tracks), buf);

    snprintf(buf, sizeof(buf), "%zu albums", e->albums);
    gtk_label_set_text(GTK_LABEL(e->stat_albums), buf);

    snprintf(buf, sizeof(buf), "%zu artists", e->artists);
    gtk_label_set_text(GTK_LABEL(e->stat_artists), buf);

    format_last_indexed(e->last_indexed_time, buf, sizeof(buf));
    gtk_label_set_text(GTK_LABEL(e->stat_last_scanned), buf);

    if (e->errors > 0) {
        snprintf(buf, sizeof(buf), "⚠ %zu errors →", e->errors);
        gtk_button_set_label(GTK_BUTTON(e->stat_errors_btn), buf);
        gtk_widget_set_visible(e->stat_errors_btn, TRUE);
    } else {
        gtk_widget_set_visible(e->stat_errors_btn, FALSE);
    }
}


/* Per-card pulse timer — pulses scan phase (always) + any STARTUP phases */
static gboolean on_card_pulse(gpointer data) {
    LibEntry *e = (LibEntry *)data;
    if (e->phases[0].state == PHASE_ACTIVE || e->phases[0].state == PHASE_STARTUP)
        gtk_progress_bar_pulse(GTK_PROGRESS_BAR(e->phases[0].bar));
    for (int i = 1; i < 7; i++) {
        if (e->phases[i].state == PHASE_STARTUP)
            gtk_progress_bar_pulse(GTK_PROGRESS_BAR(e->phases[i].bar));
    }
    return G_SOURCE_CONTINUE;
}

/* Called 5s after indexing completes — crossfade back to stats page */
static gboolean on_card_hide_progress(gpointer data) {
    LibEntry *e = (LibEntry *)data;
    e->hide_timer = 0;
    if (e->progress_revealer)
        gtk_revealer_set_reveal_child(GTK_REVEALER(e->progress_revealer), FALSE);
    return G_SOURCE_REMOVE;
}

/* Update the per-card phase panel for an incoming progress report */
static void update_card_phase_panel(LibEntry *e, const indexer_progress_t *p) {
    PhaseRow *ph = e->phases;
    char buf[128];
    char rate_buf[128];

    switch (p->phase) {
    case INDEXER_PHASE_SCANNING:
        if (ph[0].state == PHASE_WAITING)
            phase_activate(&ph[0]);

        if (p->current_path && p->current_path[0]) {
            const char *dir = strrchr(p->current_path, '/');
            dir = dir ? dir + 1 : p->current_path;
            snprintf(buf, sizeof(buf), "%.56s", dir);
        } else {
            snprintf(buf, sizeof(buf), "%zu dirs scanned", p->dirs_scanned);
        }
        gtk_label_set_text(GTK_LABEL(ph[0].label), buf);

        /* Live elapsed + dirs/sec in the scan phase rate label */
        if (ph[0].start_us > 0 && p->dirs_scanned > 0) {
            int64_t elapsed_us = g_get_monotonic_time() - ph[0].start_us;
            double elapsed_s   = elapsed_us / 1e6;
            double dirs_sec    = elapsed_s > 0.0 ? (double)p->dirs_scanned / elapsed_s : 0.0;
            if (elapsed_s < 60)
                snprintf(rate_buf, sizeof(rate_buf), "%.0fs · %.0f dirs/sec",
                         elapsed_s, dirs_sec);
            else
                snprintf(rate_buf, sizeof(rate_buf), "%.1fm · %.0f dirs/sec",
                         elapsed_s / 60.0, dirs_sec);
            gtk_label_set_text(GTK_LABEL(ph[0].rate_label), rate_buf);
            gtk_widget_set_visible(ph[0].rate_label, TRUE);
        }
        break;

    case INDEXER_PHASE_METADATA:
        if (e->pulse_timer) { g_source_remove(e->pulse_timer); e->pulse_timer = 0; }
        finalize_phase(&ph[0], p->dirs_scanned, "dirs",
                       p->phase_start_times[INDEXER_PHASE_SCANNING]);

        if (p->files_total == 0) {
            phase_skip(&ph[1]);
            break;
        }
        if (ph[1].state == PHASE_WAITING) phase_activate(&ph[1]);
        phase_update(&ph[1], p->files_processed, p->files_total, "tracks");
        break;

    case INDEXER_PHASE_ARTWORK:
        finalize_phase(&ph[1], p->files_total, "tracks",
                       p->phase_start_times[INDEXER_PHASE_METADATA]);

        if (p->albums_total == 0) {
            phase_skip(&ph[2]);
            break;
        }
        if (ph[2].state == PHASE_WAITING) phase_activate(&ph[2]);
        phase_update(&ph[2], p->albums_processed, p->albums_total, "albums");
        break;

    case INDEXER_PHASE_FINGERPRINT:
        finalize_phase(&ph[2], p->albums_total, "albums",
                       p->phase_start_times[INDEXER_PHASE_ARTWORK]);

        /* Phase 3: fingerprinting progress */
        if (p->fingerprint_total > 0) {
            if (ph[3].state == PHASE_WAITING || ph[3].state == PHASE_STARTUP)
                phase_activate(&ph[3]);
            phase_update(&ph[3], p->fingerprint_processed,
                         p->fingerprint_total, "albums");
        } else if (p->albums_total == 0) {
            /* Triage hasn't finished yet — PG connections being established */
            if (ph[3].state == PHASE_WAITING)
                phase_startup(&ph[3], "Connecting...");
            if (ph[4].state == PHASE_WAITING)
                phase_startup(&ph[4], "Connecting...");
            /* Restart pulse timer — it was killed during the metadata→artwork transition */
            if (e->pulse_timer == 0)
                e->pulse_timer = g_timeout_add(100, on_card_pulse, e);
        } else {
            phase_skip(&ph[3]);  /* All albums have tags, no fingerprinting needed */
        }
        break;

    case INDEXER_PHASE_RESOLVE:
        finalize_phase(&ph[2], p->albums_total, "albums",
                       p->phase_start_times[INDEXER_PHASE_ARTWORK]);

        /* Finalize fingerprinting if complete */
        if (p->fingerprint_total > 0
            && p->fingerprint_processed >= p->fingerprint_total
            && ph[3].state == PHASE_ACTIVE) {
            finalize_phase(&ph[3], p->fingerprint_total, "albums",
                           p->phase_start_times[INDEXER_PHASE_FINGERPRINT]);
        } else if (p->fingerprint_total == 0) {
            phase_skip(&ph[3]);
        } else if (ph[3].state == PHASE_WAITING || ph[3].state == PHASE_STARTUP) {
            phase_activate(&ph[3]);
        }
        /* Update fingerprint progress even during resolve (concurrent) */
        if (ph[3].state == PHASE_ACTIVE && p->fingerprint_total > 0) {
            phase_update(&ph[3], p->fingerprint_processed,
                         p->fingerprint_total, "albums");
        }

        /* Phase 4: MB resolution */
        if (ph[4].state == PHASE_WAITING || ph[4].state == PHASE_STARTUP)
            phase_activate(&ph[4]);
        if (p->albums_total > 0) {
            phase_update(&ph[4], p->albums_processed, p->albums_total, "albums");
        } else {
            gtk_progress_bar_pulse(GTK_PROGRESS_BAR(ph[4].bar));
            gtk_label_set_text(GTK_LABEL(ph[4].label), "Resolving...");
            gtk_widget_set_visible(ph[4].rate_label, FALSE);
        }
        break;

    case INDEXER_PHASE_ARTIST_ART:
        /* Finalize fingerprint + resolve if still active */
        if (p->fingerprint_total > 0 && ph[3].state == PHASE_ACTIVE)
            finalize_phase(&ph[3], p->fingerprint_total, "albums",
                           p->phase_start_times[INDEXER_PHASE_FINGERPRINT]);
        else if (ph[3].state != PHASE_COMPLETE && ph[3].state != PHASE_SKIPPED
                 && ph[3].state != PHASE_ERROR)
            phase_skip(&ph[3]);

        if (ph[4].state == PHASE_ACTIVE)
            finalize_phase(&ph[4], p->albums_processed, "albums",
                           p->phase_start_times[INDEXER_PHASE_RESOLVE]);
        else if (ph[4].state != PHASE_COMPLETE && ph[4].state != PHASE_SKIPPED
                 && ph[4].state != PHASE_ERROR)
            phase_skip(&ph[4]);

        if (p->artist_art_total == 0) {
            /* Total not yet known — DB query still running */
            if (ph[5].state == PHASE_WAITING) {
                phase_startup(&ph[5], "Querying...");
                if (e->pulse_timer == 0)
                    e->pulse_timer = g_timeout_add(100, on_card_pulse, e);
            }
        } else {
            if (ph[5].state == PHASE_WAITING || ph[5].state == PHASE_STARTUP)
                phase_activate(&ph[5]);
            phase_update(&ph[5], p->artist_art_processed,
                         p->artist_art_total, "artists");
        }
        break;

    case INDEXER_PHASE_ARTIST_BIO:
        /* Finalize all prior phases that may have been skipped */
        if (e->pulse_timer) { g_source_remove(e->pulse_timer); e->pulse_timer = 0; }
        for (int i = 3; i <= 5; i++) {
            if (ph[i].state == PHASE_ACTIVE)
                finalize_phase(&ph[i], 0, "",
                               p->phase_start_times[INDEXER_PHASE_ARTIST_ART]);
            else if (ph[i].state != PHASE_COMPLETE && ph[i].state != PHASE_SKIPPED
                     && ph[i].state != PHASE_ERROR)
                phase_skip(&ph[i]);
        }

        if (p->artist_bio_total == 0) {
            if (ph[6].state == PHASE_WAITING) {
                phase_startup(&ph[6], "Querying...");
                if (e->pulse_timer == 0)
                    e->pulse_timer = g_timeout_add(100, on_card_pulse, e);
            }
        } else {
            if (ph[6].state == PHASE_WAITING || ph[6].state == PHASE_STARTUP)
                phase_activate(&ph[6]);
            phase_update(&ph[6], p->artist_bio_processed,
                         p->artist_bio_total, "artists");
        }
        break;

    case INDEXER_PHASE_FINALIZE:
    case INDEXER_PHASE_COMPLETE:
        /* Handled by on_indexer_done */
        break;

    case INDEXER_PHASE_COUNT:
        break;
    }
}

void on_indexer_started(IndexerController *idx, const char *library_path, gpointer data) {
    (void)idx;
    UiWindow *w = UI_WINDOW(data);
    LibEntry *e = find_lib_entry(w, library_path);
    if (!e || !e->progress_revealer) return;

    /* Cancel any pending hide timer from a previous scan */
    if (e->hide_timer) { g_source_remove(e->hide_timer); e->hide_timer = 0; }

    e->shown_initial_load_toast = FALSE;

    /* Reset all 6 phase rows, then activate phase 0 */
    for (int i = 0; i < 6; i++)
        phase_reset(&e->phases[i]);
    phase_activate(&e->phases[0]);

    gtk_revealer_set_reveal_child(GTK_REVEALER(e->progress_revealer), TRUE);

    if (e->pulse_timer == 0)
        e->pulse_timer = g_timeout_add(100, on_card_pulse, e);
}

void on_indexer_progress(IndexerController *idx, const char *library_path,
                                 indexer_progress_t *p, gpointer data) {
    (void)idx;
    UiWindow *w = UI_WINDOW(data);
    LibEntry *e = find_lib_entry(w, library_path);
    if (e && e->progress_revealer)
        update_card_phase_panel(e, p);
}

/*
 * Single point of truth for refreshing all library-dependent views.
 * Called after cache warming completes OR after artwork atlas reload.
 * Adding a new library-dependent view? Add it here.
 */
void refresh_library_views(UiWindow *w) {
    library_view_refresh(w->artists_view);
    library_view_refresh(w->albums_view);
    if (w->detail_view)
        library_unified_detail_reload(w->detail_view);
    if (strcmp(w->current_view, "search") == 0)
        do_search(w);

    /* Sync library bar toggle labels with current names from cache */
    if (w->library_cache && w->library_toggles) {
        for (int i = 0; i < w->library_toggle_count; i++) {
            int bi = GPOINTER_TO_INT(g_object_get_data(
                G_OBJECT(w->library_toggles[i]), "lib-idx"));
            const char *name = library_cache_get_library_name(w->library_cache, bi);
            gtk_button_set_label(GTK_BUTTON(w->library_toggles[i]),
                                 name ? name : "Library");
        }
    }
}

void on_cache_ready(void *data) {
    UiWindow *w = UI_WINDOW(data);
    refresh_library_views(w);
}

/* Resolve lib_idx for a library_path (returns -1 if not found). */
int find_lib_idx(UiWindow *w, const char *library_path) {
    if (!w->settings || !library_path) return -1;
    for (int i = 0; i < w->settings->library_count; i++) {
        if (strcmp(w->settings->libraries[i].path, library_path) == 0)
            return i;
    }
    return -1;
}

/* Called whenever SQLite metadata changes and the library cache must be reloaded.
 * Fired after: phases 1-3 (initial scan), phase 6 (MB enrichment). */
void on_indexer_library_updated(IndexerController *idx, const char *library_path,
                                indexer_progress_t *p, gpointer data) {
    (void)idx;
    UiWindow *w = UI_WINDOW(data);

    int lib_idx = find_lib_idx(w, library_path);
    g_message("library-updated: library=%s lib_idx=%d cache=%p",
              library_path, lib_idx, (void*)w->library_cache);

    /* COW refresh: old data stays live while new data builds in shadow arrays.
     * on_cache_ready() fires refresh_library_views() when the swap completes. */
    if (w->library_cache && lib_idx >= 0) {
        int bitmap = w->settings->libraries[lib_idx].library_index;
        library_cache_refresh_slot(w->library_cache, bitmap, NULL, 0);
        g_message("library-updated: COW refresh started for bitmap_index=%d", bitmap);
    }

    LibEntry *e = find_lib_entry(w, library_path);
    if (!e || !e->progress_revealer) return;

    /* Finalize scan + metadata phase rows (safe to call repeatedly — no-ops if already done) */
    if (e->pulse_timer) { g_source_remove(e->pulse_timer); e->pulse_timer = 0; }
    finalize_phase(&e->phases[0], p ? p->dirs_scanned : 0, "dirs",
                   p ? p->phase_start_times[INDEXER_PHASE_SCANNING] : 0);
    finalize_phase(&e->phases[1], p ? p->files_total : 0, "tracks",
                   p ? p->phase_start_times[INDEXER_PHASE_METADATA] : 0);

    libs_load_entry_stats(e, w);

    /* Defer "Library Loaded" toast until async stats arrive (Bug fix: stats are
     * zeroed by libs_load_entry_stats above; showing now would display "0 Albums"). */
    if (!e->shown_initial_load_toast) {
        e->shown_initial_load_toast = TRUE;
        e->pending_load_toast = TRUE;
    }
}

/* Called whenever an artwork atlas is written.
 * Fired after: phase 4 (album atlas), phase 7 (global artist atlas). */
void on_indexer_artwork_updated(IndexerController *idx, const char *library_path,
                                indexer_progress_t *p, gpointer data) {
    (void)idx;
    UiWindow *w = UI_WINDOW(data);

    int lib_idx = find_lib_idx(w, library_path);

    /* Reload per-library album atlas */
    if (p && p->atlas_path[0] && w->artwork_mgr && lib_idx >= 0)
        artwork_manager_reload_library_atlas(w->artwork_mgr, lib_idx, p->atlas_path);

    /* Reload global artist atlas (cheap re-mmap, safe to call every time) */
    if (w->artwork_mgr)
        artwork_manager_reload_artist_atlas(w->artwork_mgr);

    /* Refresh all library-dependent views to pick up new artwork */
    refresh_library_views(w);

    /* Per-card: finalize artwork phase row (no-op if already done) */
    LibEntry *e = find_lib_entry(w, library_path);
    if (e && e->progress_revealer) {
        finalize_phase(&e->phases[2], p ? p->albums_total : 0, "albums",
                       p ? p->phase_start_times[INDEXER_PHASE_ARTWORK] : 0);
    }
}

/* Called when a single library scan fully completes (terminal — all phases done).
 * Cache refreshes are driven by LIBRARY_UPDATED signals; this handler is
 * purely responsible for finalizing the progress panel UI. */
void on_indexer_done(IndexerController *idx, const char *library_path,
                             gboolean ok, indexer_progress_t *p, gpointer data) {
    (void)idx;
    UiWindow *w = UI_WINDOW(data);

    /* Per-card: mark resolve phase done, update final stats */
    LibEntry *e = find_lib_entry(w, library_path);
    if (e && e->progress_revealer) {
        if (e->pulse_timer) { g_source_remove(e->pulse_timer); e->pulse_timer = 0; }

        if (ok) {
            /* Finalize fingerprint phase (or show error if AcoustID unreachable) */
            if (p && p->acoustid_error)
                phase_error(&e->phases[3], "AcoustID unreachable");
            else {
                size_t fp_total = p ? p->fingerprint_total : 0;
                if (fp_total > 0)
                    finalize_phase(&e->phases[3], fp_total, "albums",
                                   p ? p->phase_start_times[INDEXER_PHASE_FINGERPRINT] : 0);
                else
                    phase_skip(&e->phases[3]);
            }
            /* Finalize resolve phase (or show error if MB PG unreachable) */
            if (p && p->mb_pg_error)
                phase_error(&e->phases[4], "Database unreachable");
            else {
                size_t resolved = p ? p->albums_processed : 0;
                finalize_phase(&e->phases[4], resolved, "albums",
                               p ? p->phase_start_times[INDEXER_PHASE_RESOLVE] : 0);
            }
            /* Finalize artist art phase */
            if (p && p->fanart_error)
                phase_error(&e->phases[5], "API key invalid");
            else {
                size_t art_total = p ? p->artist_art_total : 0;
                if (art_total > 0)
                    finalize_phase(&e->phases[5], p->artist_art_downloaded, "artists",
                                   p ? p->phase_start_times[INDEXER_PHASE_ARTIST_ART] : 0);
                else
                    phase_skip(&e->phases[5]);
            }
            /* Finalize artist bio phase */
            {
                size_t bio_total = p ? p->artist_bio_total : 0;
                if (bio_total > 0)
                    finalize_phase(&e->phases[6], p->artist_bio_fetched, "artists",
                                   p ? p->phase_start_times[INDEXER_PHASE_ARTIST_BIO] : 0);
                else
                    phase_skip(&e->phases[6]);
            }
        } else {
            phase_error(&e->phases[3], "Cancelled");
            phase_error(&e->phases[4], "Cancelled");
            phase_error(&e->phases[5], "Cancelled");
            phase_error(&e->phases[6], "Cancelled");
        }

        /* Reload stats — MB enrichment may have updated titles/artists */
        libs_load_entry_stats(e, w);

        if (e->hide_timer) g_source_remove(e->hide_timer);
        e->hide_timer = g_timeout_add(5000, on_card_hide_progress, e);
    }
}

