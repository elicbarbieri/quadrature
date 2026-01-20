/**
 * @file perf_view.h
 * @brief Performance dashboard view widget
 */

#ifndef QUADRATURE_PERF_VIEW_H
#define QUADRATURE_PERF_VIEW_H

#include <gtk/gtk.h>
#include "quadrature/core/perf_dashboard.h"

G_BEGIN_DECLS

/**
 * Create a new performance dashboard view
 * @param dashboard Performance dashboard to display (can be NULL)
 * @return New GtkWidget containing the dashboard view
 */
GtkWidget* perf_view_new(perf_dashboard_t* dashboard);

G_END_DECLS

#endif /* QUADRATURE_PERF_VIEW_H */
