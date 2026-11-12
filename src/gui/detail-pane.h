#pragma once

#include <gtk/gtk.h>

#include "rh-ctx.h"

GtkWidget *detail_pane_new(RhCtx *ctx);
void       detail_pane_show(GtkWidget *pane, const char *game_id);
GtkWidget *detail_pane_back_button(GtkWidget *pane);
GtkWidget *detail_pane_launch_button(GtkWidget *pane);
const char *detail_pane_launch_status(GtkWidget *pane);
guint      detail_pane_page_count(GtkWidget *pane);
guint      detail_pane_video_count(GtkWidget *pane);
guint      detail_pane_thumbs_loaded(GtkWidget *pane);
guint      detail_pane_thumbs_started(GtkWidget *pane);
guint      detail_pane_thumbs_cache_hits(GtkWidget *pane);
const char *detail_pane_title(GtkWidget *pane);
