/*
 * rom-hamster-gui — M4 主窗口
 *
 * 首次运行：启动即选 ROM_ROOT，建库后直接进主窗口，不插欢迎页。
 * 主窗口：GtkColumnView 绑定 RhGameListModel，搜索走 rh_query_list（与 CLI 同一条查询）。
 */
#include <gtk/gtk.h>
#include <locale.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

#include "detail-pane.h"
#include "import.h"
#include "job.h"
#include "platform.h"
#include "game-model.h"
#include "query.h"
#include "rh-config.h"
#include "rh-ctx.h"
#include "rh-fs.h"
#include "rh-records.h"
#include "rh-version.h"
#include "store.h"

/* 应用 ID 的唯一出处：同时作为 GtkApplication 的 application_id、AppStream 组件
 * ID、.desktop 文件名与 X11 的 WM_CLASS（见 data/io.github.goddog312.romhamster.*）。
 * AppStream 要求组件 ID 只含小写，别改成 CamelCase。 */
#define RH_GUI_APP_ID "io.github.goddog312.romhamster"

typedef struct {
  gboolean running;
  gboolean cancel_requested;
  gint64   started_at;
  gint64   last_progress_at;
  gint64   last_sched_at;
  gint64   last_ui_at;
  gint64   cancel_at;
  gint64   cancel_ms;
  gint64   first_ui_ms;
  guint    enable_cancel_id;
  gint64   max_progress_gap_us;
  gint64   max_ui_gap_us;
} ImportState;

typedef struct {
  GtkApplication *app;
  RhCtx *ctx;
  GtkWindow *win;
  RhGameListModel *model;
  GtkColumnView *list;
  GtkWidget *list_scroll;
  GtkWidget *empty;
  GtkWidget *detail;
  GtkSingleSelection *selection;
  GtkSearchEntry *search;
  GtkStack *stack;
  GtkLabel *status;
  GtkProgressBar *progress;
  GtkButton *btn_import;
  GtkButton *btn_cancel;
  GtkButton *btn_rescan;
  guint search_timeout;
  gboolean store_open;
  gboolean smoke_started;
  ImportState imp;
} Gui;

typedef struct {
  gboolean on;
  gboolean scroll_mode;
  gboolean detail_mode;
  gboolean search_mode;
  gint64   t_start;
  gint64   t_map;
  gint64   t_first_paint;
  gint64   t_search_input;
  gint64   t_search_query;
  gint64   scroll_start;
  gint64   last_frame;
  GArray  *frame_gaps;
  guint    scroll_pos;
  guint    scroll_timer;
  long     rss_before;
  gboolean scroll_active;
  gboolean after_paint_connected;
} Perf;

static Perf P;
static RhUi GUI_UI;

static long rss_kb(void)
{
  FILE *f = fopen("/proc/self/status", "r");
  char line[256];
  long kb = -1;

  if (!f)
    return -1;
  while (fgets(line, sizeof line, f)) {
    if (sscanf(line, "VmRSS: %ld kB", &kb) == 1)
      break;
  }
  fclose(f);
  return kb;
}

static void perf_print(const char *fmt, ...)
{
  va_list ap;
  gchar *msg;

  if (!P.on)
    return;
  va_start(ap, fmt);
  msg = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  g_printerr("perf: %s\n", msg);
  g_free(msg);
}

static Gui *gui_new(GtkApplication *app)
{
  Gui *g = g_new0(Gui, 1);   /* root object，随进程结束；窗口关闭即退出 */
  g->app = app;
  return g;
}

static void set_status(Gui *g, const char *fmt, ...)
{
  va_list ap;
  gchar *msg;

  if (!g->status)
    return;
  va_start(ap, fmt);
  msg = g_strdup_vprintf(fmt, ap);
  va_end(ap);
  gtk_label_set_text(g->status, msg);
  g_free(msg);
}

/* --------------------------------------------------------------- 列表列 */

static void label_setup(GtkSignalListItemFactory *factory, GtkListItem *item,
                        gpointer data)
{
  GtkWidget *label = gtk_label_new(NULL);
  (void) factory;
  (void) data;
  gtk_label_set_xalign(GTK_LABEL(label), 0.0);
  gtk_label_set_ellipsize(GTK_LABEL(label), PANGO_ELLIPSIZE_END);
  gtk_widget_set_margin_start(label, 4);
  gtk_widget_set_margin_end(label, 4);
  gtk_list_item_set_child(item, label);
}

typedef const char *(*RowField)(RhGameRow *);

static void label_bind(GtkSignalListItemFactory *factory, GtkListItem *item,
                       gpointer data)
{
  GtkWidget *label = gtk_list_item_get_child(item);
  RhGameRow *row = gtk_list_item_get_item(item);
  RowField field = (RowField) data;
  (void) factory;

  gtk_label_set_text(GTK_LABEL(label), row ? field(row) : "");
}

static void append_column(Gui *g, const char *title, RowField field,
                          gboolean expand, int fixed_width)
{
  GtkListItemFactory *factory = gtk_signal_list_item_factory_new();
  GtkColumnViewColumn *col;

  g_signal_connect(factory, "setup", G_CALLBACK(label_setup), NULL);
  g_signal_connect(factory, "bind", G_CALLBACK(label_bind), field);
  col = gtk_column_view_column_new(title, factory);
  gtk_column_view_column_set_expand(col, expand);
  if (fixed_width > 0)
    gtk_column_view_column_set_fixed_width(col, fixed_width);
  gtk_column_view_append_column(g->list, col);
  g_object_unref(col);
}

static void build_columns(Gui *g)
{
  append_column(g, "Name", rh_game_row_name, TRUE, 0);
  append_column(g, "Platform", rh_game_row_platform, FALSE, 110);
  append_column(g, "Year", rh_game_row_year, FALSE, 70);
  append_column(g, "Genre", rh_game_row_genre, FALSE, 150);
  append_column(g, "Rating", rh_game_row_rating, FALSE, 80);
}

/* --------------------------------------------------------------- 查询 */

static void refresh(Gui *g)
{
  RhFilter *f;
  GPtrArray *games = NULL;
  RhErr e;
  const char *kw;
  guint shown;
  gint64 t0 = g_get_monotonic_time();

  if (!g->store_open)
    return;

  kw = gtk_editable_get_text(GTK_EDITABLE(g->search));
  f = rh_filter_new();
  f->kw = kw && *kw ? kw : NULL;
  e = rh_query_list(g->ctx, f, &games);
  rh_filter_free(f);
  if (!rh_err_ok(&e)) {
    set_status(g, "Query failed: %s", e.msg);
    return;
  }

  rh_game_list_model_set_games(g->model, games);
  shown = rh_game_list_model_get_count(g->model);
  gtk_stack_set_visible_child_name(g->stack, shown ? "list" : "empty");
  set_status(g, "%u game(s)", shown);
  if (P.on) {
    gint64 now = g_get_monotonic_time();
    perf_print("refresh_ms=%.1f rows=%u",
               (now - t0) / 1000.0, shown);
    if (P.t_search_query) {
      perf_print("search_total_ms=%.1f search_query_ms=%.1f",
                 (now - P.t_search_input) / 1000.0,
                 (now - P.t_search_query) / 1000.0);
      P.t_search_query = 0;
      P.t_search_input = 0;
    }
  }
}

static gboolean search_timeout(gpointer data)
{
  Gui *g = data;
  g->search_timeout = 0;
  if (P.on && P.search_mode && P.t_search_input)
    P.t_search_query = g_get_monotonic_time();
  refresh(g);
  return G_SOURCE_REMOVE;
}

static void on_search_changed(GtkEditable *editable, gpointer data)
{
  Gui *g = data;
  (void) editable;

  if (P.on && P.search_mode)
    P.t_search_input = g_get_monotonic_time();
  if (g->search_timeout)
    g_source_remove(g->search_timeout);
  g->search_timeout = g_timeout_add(100, search_timeout, g);
}

static void on_refresh(GtkButton *btn, gpointer data)
{
  (void) btn;
  refresh(data);
}

static void on_selection_changed(GtkSelectionModel *sel, guint pos,
                                 guint n_items, gpointer data)
{
  Gui *g = data;
  RhGameRow *row = gtk_single_selection_get_selected_item(g->selection);
  (void) sel;
  (void) pos;
  (void) n_items;

  if (row)
    set_status(g, "%s · %s · %s", rh_game_row_name(row),
               rh_game_row_platform(row), rh_game_row_year(row));
  else if (g->model)
    set_status(g, "%u game(s)", rh_game_list_model_get_count(g->model));
}

static void show_detail(Gui *g, guint pos)
{
  RhGameRow *row;
  GListModel *model = gtk_single_selection_get_model(g->selection);

  row = g_list_model_get_item(model, pos);
  if (!row)
    return;
  detail_pane_show(g->detail, rh_game_row_id(row));
  gtk_stack_set_visible_child_name(g->stack, "detail");
  set_status(g, "Showing %s", rh_game_row_name(row));
  g_object_unref(row);
}

static void on_row_activated(GtkColumnView *list, guint pos, gpointer data)
{
  Gui *g = data;
  (void) list;
  show_detail(g, pos);
}

static void on_detail_back(GtkButton *btn, gpointer data)
{
  Gui *g = data;
  guint n = rh_game_list_model_get_count(g->model);

  (void) btn;
  gtk_stack_set_visible_child_name(g->stack, n ? "list" : "empty");
  set_status(g, "%u game(s)", n);
}

/* --------------------------------------------------------------- 打开库 */

static gboolean open_library(Gui *g, const char *root)
{
  RhErr e;
  gchar *db_path;
  gchar *root_copy = g_strdup(root);

  g_mkdir_with_parents(root_copy, 0755);
  /* 先落盘再覆盖运行时配置：rh_config_set 会释放同键旧值，root 可能指向它。 */
  e = rh_config_save_user_key("ROM_ROOT", root_copy);
  rh_config_set(g->ctx->config, "ROM_ROOT", root_copy, RH_LAYER_CLI);
  if (!rh_err_ok(&e)) {
    set_status(g, "Cannot save configuration: %s", e.msg);
    g_free(root_copy);
    return FALSE;
  }

  db_path = rh_ctx_resolve_db_path(g->ctx, root_copy);
  g_free(root_copy);
  if (!db_path) {
    set_status(g, "Cannot resolve database path");
    return FALSE;
  }
  e = store_open(g->ctx, db_path, STORE_OPEN_CREATE);
  g_free(db_path);
  if (!rh_err_ok(&e)) {
    set_status(g, "Cannot open library: %s", e.msg);
    return FALSE;
  }
  g->store_open = TRUE;
  refresh(g);
  return TRUE;
}

/* --------------------------------------------------------------- 导入 */

typedef struct {
  RhErr err;
  RhImportReport *rep;
  gboolean cancelled;
} ImportResult;

typedef struct {
  Gui *g;
  GPtrArray *paths;
} ImportArgs;

static void import_args_free(gpointer data)
{
  ImportArgs *a = data;

  g_ptr_array_unref(a->paths);
  g_free(a);
}

typedef struct {
  GWeakRef win;
  double pct;
  gchar *label;
} ProgressMsg;

static void progress_msg_free(gpointer data)
{
  ProgressMsg *m = data;

  g_weak_ref_clear(&m->win);
  g_free(m->label);
  g_free(m);
}

static gboolean update_progress_ui(gpointer data)
{
  ProgressMsg *m = data;
  GtkWindow *win = g_weak_ref_get(&m->win);

  if (win) {
    Gui *g = g_object_get_data(G_OBJECT(win), "gui");

    if (g && g->imp.running) {
      gint64 now = g_get_monotonic_time();

      if (g->imp.last_ui_at) {
        if (now - g->imp.last_ui_at > g->imp.max_ui_gap_us)
          g->imp.max_ui_gap_us = now - g->imp.last_ui_at;
      } else {
        /* 首次更新取决于首帧绘制，单独记为 first_ui_ms，不计入刷新间隔。 */
        g->imp.first_ui_ms = (now - g->imp.started_at) / 1000;
      }
      g->imp.last_ui_at = now;
      gtk_widget_set_visible(GTK_WIDGET(g->progress), TRUE);
      if (m->pct < 0)
        gtk_progress_bar_pulse(g->progress);
      else
        gtk_progress_bar_set_fraction(g->progress,
                                      CLAMP(m->pct, 0.0, 1.0));
      gtk_progress_bar_set_text(g->progress, m->label && *m->label ? m->label : "");
      set_status(g, "Importing: %s", m->label && *m->label ? m->label : "…");
    }
    g_object_unref(win);
  }
  return G_SOURCE_REMOVE;
}

/* 工作线程只通过这个回调发进度，不直接碰 GTK。最多每 100 ms 投递一次。 */
static void gui_import_progress(void *ud, double pct, const char *label)
{
  Gui *g = ud;
  gint64 now;

  if (!g || !g->imp.running)
    return;
  now = g_get_monotonic_time();
  if (g->imp.last_progress_at &&
      now - g->imp.last_progress_at > g->imp.max_progress_gap_us)
    g->imp.max_progress_gap_us = now - g->imp.last_progress_at;
  g->imp.last_progress_at = now;
  if (now - g->imp.last_sched_at < 100000 && pct < 1.0)
    return;
  g->imp.last_sched_at = now;

  ProgressMsg *m = g_new0(ProgressMsg, 1);

  g_weak_ref_init(&m->win, g->win);
  m->pct = pct;
  m->label = g_strdup(label ? label : "");
  g_idle_add_full(G_PRIORITY_DEFAULT, update_progress_ui, m, progress_msg_free);
}

static void import_result_free(gpointer data)
{
  ImportResult *r = data;

  if (r->rep)
    g_free(r->rep);
  g_free(r);
}

static void import_thread(GTask *task, gpointer source, gpointer data,
                          GCancellable *cancellable)
{
  ImportArgs *a = data;
  Gui *g = a->g;
  RhPlatformRules *rules = platform_rules_new();
  RhErr e = platform_rules_load_standard(rules);
  RhImportReport *rep = NULL;
  ImportResult *r = g_new0(ImportResult, 1);

  (void) source;
  (void) cancellable;
  {
    const char *pf = g_getenv("RH_PLATFORMS");

    if (pf && *pf)
      e = platform_rules_load(rules, pf);
  }
  if (!rh_err_ok(&e) && platform_count(rules) == 0) {
    r->err = e;
  } else if (platform_count(rules) == 0) {
    r->err = rh_fail(RH_ERR_CONFIG,
                     "平台规则为空：请安装 rom-hamster-data，或设置 RH_PLATFORMS");
  } else {
    GPtrArray *inputs = g_ptr_array_new_with_free_func(g_free);
    RhImportOpts opts = { 0 };

    if (!rh_err_ok(&e))
      rh_err_clear(&e);
    for (guint i = 0; i < a->paths->len; i++)
      g_ptr_array_add(inputs, g_canonicalize_filename(a->paths->pdata[i], NULL));
    opts.non_interactive = TRUE;
    r->err = rh_import_run(g->ctx, rules, inputs, &opts, &rep);
    r->rep = rep;
    r->cancelled = rh_job_cancelled(g->ctx);
    g_ptr_array_unref(inputs);
  }
  platform_rules_free(rules);
  g_task_return_pointer(task, r, import_result_free);
}

static void request_import_cancel(Gui *g)
{
  if (!g->imp.running || g->imp.cancel_requested)
    return;
  g->imp.cancel_requested = TRUE;
  g->imp.cancel_at = g_get_monotonic_time();
  gtk_widget_set_sensitive(GTK_WIDGET(g->btn_cancel), FALSE);
  rh_job_cancel(g->ctx);
  set_status(g, "Cancelling import…");
}

static void on_cancel_clicked(GtkButton *btn, gpointer data)
{
  (void) btn;
  request_import_cancel(data);
}

static gboolean smoke_import_cancel(gpointer data)
{
  request_import_cancel(data);
  return G_SOURCE_REMOVE;
}

static gboolean enable_cancel(gpointer data)
{
  Gui *g = data;

  g->imp.enable_cancel_id = 0;
  if (g->imp.running)
    gtk_widget_set_sensitive(GTK_WIDGET(g->btn_cancel), TRUE);
  return G_SOURCE_REMOVE;
}

static void on_import_done(GObject *source, GAsyncResult *res, gpointer data)
{
  Gui *g = data;

  (void) source;
  ImportResult *r = g_task_propagate_pointer(G_TASK(res), NULL);
  gint64 now = g_get_monotonic_time();

  g->imp.running = FALSE;
  if (g->imp.enable_cancel_id) {
    g_source_remove(g->imp.enable_cancel_id);
    g->imp.enable_cancel_id = 0;
  }
  if (g->imp.cancel_at)
    g->imp.cancel_ms = (now - g->imp.cancel_at) / 1000;

  gtk_widget_set_visible(GTK_WIDGET(g->progress), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->btn_cancel), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(g->btn_cancel), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(g->btn_import), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(g->btn_rescan), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(g->search), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(g->list), TRUE);

  refresh(g);
  if (!r) {
    set_status(g, "Import failed: no result");
  } else if ((r->err.code == RH_ERR_CANCELLED) || r->cancelled) {
    set_status(g, "Import cancelled%s",
               g->imp.cancel_ms ? "" : " (before start)");
  } else if (!rh_err_ok(&r->err)) {
    set_status(g, "Import failed: %s", r->err.msg);
  } else if (r->rep) {
    set_status(g, "Imported %d ROM + %d media, skipped %d, failed %d",
               r->rep->imported, r->rep->media,
               r->rep->skipped, r->rep->failed);
  } else {
    set_status(g, "Import finished");
  }

  if (g_getenv("RH_SMOKE_TEST")) {
    g_printerr("smoke: import cancelled=%d planned=%d imported=%d media=%d "
               "skipped=%d failed=%d\n",
               r ? (r->cancelled || r->err.code == RH_ERR_CANCELLED) : 0,
               r && r->rep ? r->rep->planned : -1,
               r && r->rep ? r->rep->imported : -1,
               r && r->rep ? r->rep->media : -1,
               r && r->rep ? r->rep->skipped : -1,
               r && r->rep ? r->rep->failed : -1);
    g_printerr("smoke: import progress_max_gap_ms=%.1f ui_max_gap_ms=%.1f "
               "first_ui_ms=%ld cancel_ms=%.1f rows=%u\n",
               g->imp.max_progress_gap_us / 1000.0,
               g->imp.max_ui_gap_us / 1000.0,
               (long) g->imp.first_ui_ms,
               (double) g->imp.cancel_ms,
               rh_game_list_model_get_count(g->model));
    g_application_quit(G_APPLICATION(g->app));
  }
  if (r)
    import_result_free(r);
}

static void start_import_paths(Gui *g, GPtrArray *paths)
{
  GTask *task;
  const char *first;

  if (!g || !g->store_open || g->imp.running || !paths || paths->len == 0)
    return;
  first = paths->pdata[0];
  if (!first || !*first)
    return;
  g->imp.running = TRUE;
  g->imp.cancel_requested = FALSE;
  g->imp.started_at = g_get_monotonic_time();
  g->imp.last_progress_at = g->imp.started_at;
  g->imp.last_sched_at = 0;
  g->imp.last_ui_at = 0;
  g->imp.cancel_at = 0;
  g->imp.cancel_ms = 0;
  g->imp.max_progress_gap_us = 0;
  g->imp.max_ui_gap_us = 0;
  g->imp.first_ui_ms = -1;
  GUI_UI.userdata = g;
  GUI_UI.progress = gui_import_progress;
  GUI_UI.log = NULL;
  rh_ctx_set_ui(g->ctx, &GUI_UI);
  g_atomic_int_set(&g->ctx->cancel_flag, 0);

  gtk_widget_set_sensitive(GTK_WIDGET(g->btn_import), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(g->btn_rescan), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(g->search), FALSE);
  gtk_widget_set_sensitive(GTK_WIDGET(g->list), FALSE);
  gtk_widget_set_visible(GTK_WIDGET(g->progress), TRUE);
  gtk_progress_bar_set_fraction(g->progress, 0.0);
  gtk_progress_bar_set_text(g->progress, "Scanning…");
  gtk_widget_set_visible(GTK_WIDGET(g->btn_cancel), TRUE);
  gtk_widget_set_sensitive(GTK_WIDGET(g->btn_cancel), FALSE);
  g->imp.enable_cancel_id = g_timeout_add(100, enable_cancel, g);
  if (paths->len == 1)
    set_status(g, "Scanning %s", first);
  else
    set_status(g, "Scanning %u items", paths->len);

  task = g_task_new(NULL, NULL, on_import_done, g);
  {
    ImportArgs *a = g_new0(ImportArgs, 1);

    a->g = g;
    a->paths = g_ptr_array_ref(paths);
    g_task_set_task_data(task, a, import_args_free);
  }
  g_task_run_in_thread(task, import_thread);
  g_object_unref(task);
}

static void start_import_path(Gui *g, const char *path)
{
  GPtrArray *paths;

  if (!path || !*path)
    return;
  paths = g_ptr_array_new_with_free_func(g_free);
  g_ptr_array_add(paths, g_strdup(path));
  start_import_paths(g, paths);
  g_ptr_array_unref(paths);
}

static gboolean on_drop(GtkDropTarget *target, const GValue *value,
                         double x, double y, gpointer data)
{
  Gui *g = data;
  GdkFileList *file_list;
  GSList *files;
  GPtrArray *paths;

  (void) target;
  (void) x;
  (void) y;
  if (!g || !g->store_open || g->imp.running)
    return FALSE;
  if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST))
    return FALSE;
  file_list = g_value_get_boxed(value);
  if (!file_list)
    return FALSE;
  files = gdk_file_list_get_files(file_list);
  paths = g_ptr_array_new_with_free_func(g_free);
  for (GSList *l = files; l; l = l->next) {
    gchar *path = g_file_get_path(G_FILE(l->data));

    if (path && *path)
      g_ptr_array_add(paths, path);
    else
      g_free(path);
  }
  if (paths->len == 0) {
    set_status(g, "No local files to import");
    g_ptr_array_unref(paths);
    return FALSE;
  }
  start_import_paths(g, paths);
  g_ptr_array_unref(paths);
  return TRUE;
}

static void install_drop_target(Gui *g)
{
  GtkDropTarget *target = gtk_drop_target_new(GDK_TYPE_FILE_LIST,
                                              GDK_ACTION_COPY);

  g_signal_connect(target, "drop", G_CALLBACK(on_drop), g);
  gtk_widget_add_controller(GTK_WIDGET(g->win),
                           GTK_EVENT_CONTROLLER(target));
}

static GtkWidget *DND_SOURCE;
static GtkWindow *DND_SOURCE_WIN;

/* 真实指针拖拽验收时创建独立源窗口，正常运行时界面不变。 */
static void install_dnd_source(Gui *g)
{
  const char *joined = g_getenv("RH_SMOKE_DND_PATHS");
  gchar **parts;
  GPtrArray *files;
  GdkFileList *list;
  GdkContentProvider *provider;
  GtkDragSource *source;
  GtkWidget *label;

  (void) g;
  if (!joined || !*joined)
    return;
  parts = g_strsplit(joined, G_SEARCHPATH_SEPARATOR_S, 0);
  files = g_ptr_array_new_with_free_func(g_object_unref);
  for (guint i = 0; parts[i]; i++) {
    if (*parts[i])
      g_ptr_array_add(files, g_file_new_for_path(parts[i]));
  }
  if (files->len == 0) {
    g_ptr_array_unref(files);
    g_strfreev(parts);
    return;
  }
  list = gdk_file_list_new_from_array((GFile **) files->pdata, files->len);
  provider = gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, list);
  source = gtk_drag_source_new();
  gtk_drag_source_set_content(source, provider);
  gtk_drag_source_set_actions(source, GDK_ACTION_COPY);
  DND_SOURCE_WIN = GTK_WINDOW(gtk_window_new());
  gtk_window_set_title(DND_SOURCE_WIN, "rh-dnd-source");
  gtk_window_set_default_size(DND_SOURCE_WIN, 200, 100);
  gtk_window_set_decorated(DND_SOURCE_WIN, FALSE);
  label = gtk_label_new("DRAG");
  gtk_widget_set_size_request(label, 200, 100);
  gtk_widget_set_hexpand(label, TRUE);
  gtk_widget_set_vexpand(label, TRUE);
  gtk_window_set_child(DND_SOURCE_WIN, label);
  gtk_widget_add_controller(label, GTK_EVENT_CONTROLLER(source));
  DND_SOURCE = label;
  gtk_window_present(DND_SOURCE_WIN);
  g_object_unref(provider);
  g_boxed_free(GDK_TYPE_FILE_LIST, list);
  g_ptr_array_unref(files);
  g_strfreev(parts);
}

static void on_import_folder_chosen(GObject *source, GAsyncResult *res,
                                    gpointer data)
{
  Gui *g = data;
  GtkFileDialog *dlg = GTK_FILE_DIALOG(source);
  GFile *folder;
  GError *err = NULL;
  gchar *path;

  folder = gtk_file_dialog_select_folder_finish(dlg, res, &err);
  if (!folder) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      GtkAlertDialog *ad = gtk_alert_dialog_new("Cannot choose folder: %s",
                                                err->message);
      gtk_alert_dialog_show(ad, g->win);
      g_object_unref(ad);
    }
    g_clear_error(&err);
    return;
  }
  path = g_file_get_path(folder);
  g_object_unref(folder);
  start_import_path(g, path);
  g_free(path);
}

static void on_import_clicked(GtkButton *btn, gpointer data)
{
  Gui *g = data;
  GtkFileDialog *dlg = gtk_file_dialog_new();

  (void) btn;
  gtk_file_dialog_set_title(dlg, "Import games or a folder");
  gtk_file_dialog_select_folder(dlg, g->win, NULL,
                                on_import_folder_chosen, g);
  g_object_unref(dlg);
}

/* --------------------------------------------------------------- 首启 */

static void on_root_chosen(GObject *source, GAsyncResult *res, gpointer data)
{
  Gui *g = data;
  GtkFileDialog *dlg = GTK_FILE_DIALOG(source);
  GFile *folder;
  GError *err = NULL;
  gchar *path;

  folder = gtk_file_dialog_select_folder_finish(dlg, res, &err);
  if (!folder) {
    if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
      GtkAlertDialog *ad = gtk_alert_dialog_new("Cannot choose folder: %s",
                                                err->message);
      gtk_alert_dialog_show(ad, g->win);
      g_object_unref(ad);
    }
    g_clear_error(&err);
    g_application_quit(G_APPLICATION(g->app));
    return;
  }

  path = g_file_get_path(folder);
  if (!open_library(g, path)) {
    GtkAlertDialog *ad =
      gtk_alert_dialog_new("Library could not be opened. See the status bar.");
    gtk_alert_dialog_show(ad, g->win);
    g_object_unref(ad);
  }
  g_free(path);
  g_object_unref(folder);
}

static void choose_root(Gui *g)
{
  const char *test_root = g_getenv("RH_SMOKE_ROOT");

  if (test_root && *test_root) {
    open_library(g, test_root);
    return;
  }

  GtkFileDialog *dlg = gtk_file_dialog_new();
  gtk_file_dialog_set_title(dlg, "Choose ROM library folder");
  gtk_file_dialog_select_folder(dlg, g->win, NULL, on_root_chosen, g);
  g_object_unref(dlg);
}

/* --------------------------------------------------------------- 窗口 */

static gboolean smoke_after_search(gpointer data)
{
  Gui *g = data;
  const char *expect = g_getenv("RH_SMOKE_EXPECT_SEARCH");
  guint n = rh_game_list_model_get_count(g->model);

  g_printerr("smoke: search rows=%u\n", n);
  if (expect && n != (guint) atoi(expect))
    g_printerr("smoke: expected search rows=%s\n", expect);
  g_application_quit(G_APPLICATION(g->app));
  return G_SOURCE_REMOVE;
}

static gboolean smoke_detail_done(gpointer data)
{
  Gui *g = data;

  g_printerr("smoke: detail pages=%u title=%s thumbs=%u/%u cache_hits=%u videos=%u video_ready=%u video_playing=%u video_errors=%u\n",
             detail_pane_page_count(g->detail), detail_pane_title(g->detail),
             detail_pane_thumbs_loaded(g->detail),
             detail_pane_thumbs_started(g->detail),
             detail_pane_thumbs_cache_hits(g->detail),
             detail_pane_video_count(g->detail),
             detail_pane_video_ready(g->detail),
             detail_pane_video_playing(g->detail),
             detail_pane_video_errors(g->detail));
  if (g_getenv("RH_SMOKE_BACK")) {
    g_signal_emit_by_name(detail_pane_back_button(g->detail), "clicked");
    g_printerr("smoke: back stack=%s\n",
               gtk_stack_get_visible_child_name(g->stack));
  }
  g_application_quit(G_APPLICATION(g->app));
  return G_SOURCE_REMOVE;
}

static gboolean perf_quit(gpointer data)
{
  g_application_quit(G_APPLICATION(((Gui *) data)->app));
  return G_SOURCE_REMOVE;
}

/* 冒烟脚本要抓窗口截图，需要窗口保持存活一段时间；RH_SMOKE_HOLD_MS 只推迟退出。 */
static gboolean smoke_hold_done(gpointer data)
{
  g_printerr("smoke: hold finished\n");
  return perf_quit(data);
}

static void perf_scroll_stop(Gui *g)
{
  guint n = P.frame_gaps ? P.frame_gaps->len : 0;
  long rss_after = rss_kb();
  gint64 p95 = 0;
  gint64 worst = 0;

  if (P.scroll_timer) {
    g_source_remove(P.scroll_timer);
    P.scroll_timer = 0;
  }
  P.scroll_active = FALSE;
  for (guint i = 0; i < n; i++) {
    for (guint j = i + 1; j < n; j++) {
      gint64 *a = &g_array_index(P.frame_gaps, gint64, i);
      gint64 *b = &g_array_index(P.frame_gaps, gint64, j);
      if (*a > *b) {
        gint64 t = *a;
        *a = *b;
        *b = t;
      }
    }
  }
  if (n) {
    p95 = g_array_index(P.frame_gaps, gint64, (n * 95) / 100);
    worst = g_array_index(P.frame_gaps, gint64, n - 1);
  }
  perf_print("scroll_frames=%u p95_ms=%.1f max_ms=%.1f rss_delta_kb=%ld",
             n, p95 / 1000.0, worst / 1000.0,
             rss_after >= 0 ? rss_after - P.rss_before : -1);
  g_application_quit(G_APPLICATION(g->app));
}

static gboolean perf_scroll_step(gpointer data)
{
  Gui *g = data;
  guint count = rh_game_list_model_get_count(g->model);

  if (g_get_monotonic_time() - P.scroll_start >= 10000 * 1000 ||
      !count || !P.scroll_active) {
    P.scroll_timer = 0;
    perf_scroll_stop(g);
    return G_SOURCE_REMOVE;
  }
  P.scroll_pos = (P.scroll_pos + 250) % count;
#if GTK_CHECK_VERSION(4, 12, 0)
  gtk_column_view_scroll_to(g->list, P.scroll_pos, NULL,
                            GTK_LIST_SCROLL_NONE, NULL);
#else
  /* GTK 4.10 基线没有该入口；性能夹具在基线版本上显式跳过滚动项。 */
  perf_print("scroll_skip=gtk_column_view_scroll_to_unavailable");
  P.scroll_timer = 0;
  P.scroll_active = FALSE;
  g_application_quit(G_APPLICATION(g->app));
  return G_SOURCE_REMOVE;
#endif
  return G_SOURCE_CONTINUE;
}

static void perf_scroll_start(Gui *g)
{
  P.scroll_active = TRUE;
  P.scroll_start = g_get_monotonic_time();
  P.last_frame = 0;
  P.rss_before = rss_kb();
  P.frame_gaps = g_array_new(FALSE, FALSE, sizeof(gint64));
  P.scroll_timer = g_timeout_add(16, perf_scroll_step, g);
  perf_print("scroll_start rows=%u", rh_game_list_model_get_count(g->model));
}

static void perf_after_paint(GdkFrameClock *clock, gpointer data)
{
  Gui *g = data;
  gint64 now = g_get_monotonic_time();

  (void) clock;
  if (!P.t_first_paint) {
    P.t_first_paint = now;
    perf_print("first_screen_ms=%.1f rss_kb=%ld",
               (now - P.t_map) / 1000.0, rss_kb());
    if (P.scroll_mode) {
      perf_scroll_start(g);
    } else if (P.detail_mode) {
      show_detail(g, 0);
      g_timeout_add(2500, perf_quit, g);
    } else if (P.search_mode) {
      const char *kw = g_getenv("RH_PERF_SEARCH");
      if (kw && *kw)
        gtk_editable_set_text(GTK_EDITABLE(g->search), kw);
      g_timeout_add(1500, perf_quit, g);
    } else if (!g_getenv("RH_SMOKE_TEST")) {
      g_timeout_add(100, perf_quit, g);
    }
  }
  if (P.scroll_active) {
    if (P.last_frame)
      g_array_append_val(P.frame_gaps, (gint64){now - P.last_frame});
    P.last_frame = now;
  }
}

static void smoke_drop_paths(Gui *g, const char *joined)
{
  gchar **parts = g_strsplit(joined, G_SEARCHPATH_SEPARATOR_S, 0);
  GPtrArray *files = g_ptr_array_new_with_free_func(g_object_unref);
  GdkFileList *list;
  GValue value = G_VALUE_INIT;

  for (guint i = 0; parts[i]; i++) {
    if (*parts[i])
      g_ptr_array_add(files, g_file_new_for_path(parts[i]));
  }
  if (files->len == 0) {
    g_printerr("smoke: no drop paths\n");
    g_ptr_array_unref(files);
    g_strfreev(parts);
    return;
  }
  list = gdk_file_list_new_from_array((GFile **) files->pdata, files->len);
  g_value_init(&value, GDK_TYPE_FILE_LIST);
  g_value_set_boxed(&value, list);
  on_drop(NULL, &value, 0, 0, g);
  g_value_unset(&value);
  g_boxed_free(GDK_TYPE_FILE_LIST, list);
  g_ptr_array_unref(files);
  g_strfreev(parts);
}

static gboolean smoke_dnd_coords(gpointer data)
{
  Gui *g = data;
  int ww, wh;

  if (!DND_SOURCE)
    return G_SOURCE_REMOVE;
  ww = gtk_widget_get_width(GTK_WIDGET(g->win));
  wh = gtk_widget_get_height(GTK_WIDGET(g->win));
  g_printerr("smoke: dnd source x=%d y=%d target x=%d y=%d\n",
             100, 50, ww / 2, wh / 2);
  return G_SOURCE_REMOVE;
}

static void on_window_map(GtkWidget *widget, gpointer data)
{
  Gui *g = data;
  const char *expect = g_getenv("RH_SMOKE_EXPECT_ROWS");
  const char *kw = g_getenv("RH_SMOKE_SEARCH");

  (void) widget;
  if (P.on && !P.t_map) {
    P.t_map = g_get_monotonic_time();
    perf_print("startup_ms=%.1f rss_kb=%ld",
               (P.t_map - P.t_start) / 1000.0, rss_kb());
    if (!P.after_paint_connected) {
      GdkFrameClock *clock = gtk_widget_get_frame_clock(widget);
      if (clock) {
        P.after_paint_connected = TRUE;
        g_signal_connect(clock, "after-paint", G_CALLBACK(perf_after_paint), g);
      }
    }
  }
  if (!g_getenv("RH_SMOKE_TEST"))
    return;
  if (g->smoke_started)
    return;
  g->smoke_started = TRUE;

  g_printerr("smoke: window mapped, %u row(s) in %s\n",
             g->model ? rh_game_list_model_get_count(g->model) : 0,
             rh_config_get(g->ctx->config, "ROM_ROOT"));
  if (expect && g->model &&
      rh_game_list_model_get_count(g->model) != (guint) atoi(expect))
    g_printerr("smoke: expected %s row(s)\n", expect);
  if (g_getenv("RH_SMOKE_DND_PATHS") && DND_SOURCE) {
    g_timeout_add(100, smoke_dnd_coords, g);
    return;
  }
  if (g_getenv("RH_SMOKE_DROP_PATHS")) {
    smoke_drop_paths(g, g_getenv("RH_SMOKE_DROP_PATHS"));
    return;
  }
  if (g_getenv("RH_SMOKE_IMPORT_DIR")) {
    const char *cancel_ms = g_getenv("RH_SMOKE_IMPORT_CANCEL_MS");
    GSource *src = NULL;

    start_import_path(g, g_getenv("RH_SMOKE_IMPORT_DIR"));
    if (cancel_ms && *cancel_ms)
      src = g_timeout_source_new(atoi(cancel_ms));
    if (src) {
      g_source_set_callback(src, smoke_import_cancel, g, NULL);
      g_source_attach(src, NULL);
      g_source_unref(src);
    }
    return;
  }
  if (kw && *kw) {
    gtk_editable_set_text(GTK_EDITABLE(g->search), kw);
    g_timeout_add(500, smoke_after_search, g);
    return;
  }
  if (g_getenv("RH_SMOKE_DETAIL") && g->model &&
      rh_game_list_model_get_count(g->model) > 0) {
    show_detail(g, 0);
    if (g_getenv("RH_SMOKE_VIDEO_TAB"))
      detail_pane_select_page(g->detail, "videos");
    if (g_getenv("RH_SMOKE_VIDEO_PLAY"))
      detail_pane_play_first_video(g->detail);
    g_timeout_add(2500, smoke_detail_done, g);
    return;
  }
  if (g_getenv("RH_SMOKE_HOLD_MS")) {
    g_timeout_add((guint) atoi(g_getenv("RH_SMOKE_HOLD_MS")), smoke_hold_done, g);
    return;
  }
  g_application_quit(G_APPLICATION(g->app));
}

static gboolean on_close_request(GtkWindow *win, gpointer data)
{
  Gui *g = data;

  (void) win;
  if (g->imp.running) {
    set_status(g, "Cancel the running import before closing");
    return TRUE;
  }
  return FALSE;
}

static void create_window(Gui *g)
{
  GtkBuilder *b = gtk_builder_new_from_resource(
    "/io/github/goddog312/romhamster/ui/main-window.ui");
  GtkButton *btn_rescan;

  g->win = GTK_WINDOW(gtk_builder_get_object(b, "win"));
  g->list = GTK_COLUMN_VIEW(gtk_builder_get_object(b, "list"));
  g->list_scroll = GTK_WIDGET(gtk_builder_get_object(b, "list_scroll"));
  g->empty = GTK_WIDGET(gtk_builder_get_object(b, "empty"));
  g->search = GTK_SEARCH_ENTRY(gtk_builder_get_object(b, "search"));
  g->stack = GTK_STACK(gtk_builder_get_object(b, "stack"));
  g->status = GTK_LABEL(gtk_builder_get_object(b, "status"));
  g->progress = GTK_PROGRESS_BAR(gtk_builder_get_object(b, "progress"));
  g->btn_import = GTK_BUTTON(gtk_builder_get_object(b, "btn_import"));
  g->btn_cancel = GTK_BUTTON(gtk_builder_get_object(b, "btn_cancel"));
  g->btn_rescan = btn_rescan = GTK_BUTTON(gtk_builder_get_object(b, "btn_rescan"));

  gtk_window_set_application(g->win, g->app);
  g->model = rh_game_list_model_new();
  gtk_stack_page_set_name(gtk_stack_get_page(g->stack, g->list_scroll), "list");
  gtk_stack_page_set_name(gtk_stack_get_page(g->stack, g->empty), "empty");
  g->selection = gtk_single_selection_new(G_LIST_MODEL(g->model));
  gtk_column_view_set_model(g->list, GTK_SELECTION_MODEL(g->selection));
  build_columns(g);

  g_signal_connect(g->search, "changed",
                   G_CALLBACK(on_search_changed), g);
  g_object_set_data(G_OBJECT(g->win), "gui", g);
  gtk_progress_bar_set_show_text(g->progress, TRUE);
  g_signal_connect(btn_rescan, "clicked", G_CALLBACK(on_refresh), g);
  g_signal_connect(g->btn_import, "clicked", G_CALLBACK(on_import_clicked), g);
  g_signal_connect(g->btn_cancel, "clicked", G_CALLBACK(on_cancel_clicked), g);
  g_signal_connect(g->selection, "selection-changed",
                   G_CALLBACK(on_selection_changed), g);
  g->detail = detail_pane_new(g->ctx);
  gtk_stack_add_titled(g->stack, g->detail, "detail", "Detail");
  g_signal_connect(detail_pane_back_button(g->detail), "clicked",
                   G_CALLBACK(on_detail_back), g);
  g_signal_connect(g->list, "activate", G_CALLBACK(on_row_activated), g);
  g_signal_connect(g->win, "map", G_CALLBACK(on_window_map), g);
  g_signal_connect(g->win, "close-request", G_CALLBACK(on_close_request), g);
  install_drop_target(g);
  install_dnd_source(g);
  g_object_unref(g->detail);   /* stack 已持有；g->detail 仍指向同一个对象 */
  g_object_unref(b);
}

static void on_activate(GtkApplication *app, gpointer data)
{
  Gui *g = data;
  const char *root;

  if (g->win) {
    gtk_window_present(g->win);
    return;
  }

  (void) app;
  g->ctx = rh_ctx_new();
  GUI_UI.userdata = g;
  GUI_UI.progress = gui_import_progress;
  GUI_UI.log = NULL;
  rh_ctx_set_ui(g->ctx, &GUI_UI);
  create_window(g);

  root = rh_config_get(g->ctx->config, "ROM_ROOT");
  if (root && *root)
    open_library(g, root);
  else
    choose_root(g);
  gtk_window_present(g->win);
}

int main(int argc, char **argv)
{
  GtkApplication *app;
  P.t_start = g_get_monotonic_time();
  P.on = g_getenv("RH_PERF") != NULL;
  P.scroll_mode = g_getenv("RH_PERF_SCROLL") != NULL;
  P.detail_mode = g_getenv("RH_PERF_DETAIL") != NULL;
  P.search_mode = g_getenv("RH_PERF_SEARCH") != NULL;
  Gui *g;
  int status;

  setlocale(LC_ALL, "");

  /* g_get_prgname 默认取 argv[0]（rom-hamster-gui），X11 的 WM_CLASS 由它决定。
   * 显式设成应用 ID，.desktop 的 StartupWMClass 才能对上（Wayland 下 GTK 本来就用
   * application_id 作为 app_id，这里只是把两条路径对齐）。 */
  g_set_prgname(RH_GUI_APP_ID);

#if GLIB_CHECK_VERSION(2, 74, 0)
  app = gtk_application_new(RH_GUI_APP_ID,
                            G_APPLICATION_DEFAULT_FLAGS);
#else
  app = gtk_application_new(RH_GUI_APP_ID,
                            G_APPLICATION_FLAGS_NONE);
#endif

  g = gui_new(app);
  g_signal_connect(app, "activate", G_CALLBACK(on_activate), g);
  status = g_application_run(G_APPLICATION(app), argc, argv);

  if (g->store_open)
    store_close(g->ctx);
  if (g->ctx)
    rh_ctx_free(g->ctx);
  g_free(g);
  g_object_unref(app);
  return status;
}
