/**
 * Libraries View / Indexer Bridge Internal Header
 *
 * Functions shared between libraries_view.c, indexer_bridge.c, and window.c.
 */

#pragma once

#include "../internal.h"

/* Libraries view */
GtkWidget *make_libraries_view(UiWindow *w);
LibEntry  *find_lib_entry(UiWindow *w, const char *path);
void       libs_load(UiWindow *w);
void       libs_rebuild(UiWindow *w);
void       libs_free(UiWindow *w);

/* Indexer bridge — stats and card updates (called from libraries_view.c) */
void libs_load_entry_stats(LibEntry *e, UiWindow *w);
void update_card_stats_labels(LibEntry *e);
void refresh_library_views(UiWindow *w);
void on_cache_ready(void *data);

/* Indexer signal handlers (connected in window.c build_ui) */
void on_indexer_started(IndexerController *idx, const char *library_path, gpointer data);
void on_indexer_progress(IndexerController *idx, const char *library_path,
                         indexer_progress_t *p, gpointer data);
void on_indexer_library_updated(IndexerController *idx, const char *library_path,
                                indexer_progress_t *p, gpointer data);
void on_indexer_artwork_updated(IndexerController *idx, const char *library_path,
                                indexer_progress_t *p, gpointer data);
void on_indexer_done(IndexerController *idx, const char *library_path,
                     gboolean ok, indexer_progress_t *p, gpointer data);
