/**
 * @file perf_view.h
 * @brief Performance dashboard view widget
 */

#ifndef QUADRATURE_PERF_VIEW_H
#define QUADRATURE_PERF_VIEW_H

#include <gtk/gtk.h>
#include "../../core/internal.h"

G_BEGIN_DECLS

/* Forward declarations */
typedef struct audio_cache audio_cache_t;

/**
 * Create a new performance dashboard view
 * @param dashboard Performance dashboard to display (can be NULL)
 * @param cache Audio cache for decode metrics (can be NULL)
 * @return New GtkWidget containing the dashboard view
 */
GtkWidget* perf_view_new(perf_dashboard_t* dashboard, audio_cache_t* cache);

G_END_DECLS

#endif /* QUADRATURE_PERF_VIEW_H */
