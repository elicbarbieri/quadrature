/**
 * Search View Internal Header
 *
 * Functions called by window.c to build and control the search view.
 */

#pragma once

#include "../internal.h"

GtkWidget *make_search_view(UiWindow *w);
void set_search_filter(UiWindow *w, int idx);
void focus_search_entry(UiWindow *w);
void clear_search_view_filters(UiWindow *w);
void do_search(UiWindow *w);
