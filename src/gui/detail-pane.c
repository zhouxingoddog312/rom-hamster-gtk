#include "detail-pane.h"

#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <gmodule.h>

#include "detail.h"

typedef struct {
  RhCtx *ctx;
  GtkStack *media;
  GtkBox *meta;
  GtkLabel *title;
  GtkWidget *root;
  guint thumbs_started;
  guint thumbs_loaded;
  guint thumbs_cache_hits;
  guint thumb_generation;
  guint videos;
  guint video_errors;
  guint video_ready;
  guint video_playing;
  GtkWidget *first_playable_video;
} DetailPane;

typedef struct {
  gchar *path;
  gchar *cache_path;
} ThumbJob;

typedef struct {
  GdkPixbuf *pb;
  gboolean cache_hit;
  gint64   load_us;
} ThumbResult;

typedef struct {
  GtkWidget *pic;       /* 持有引用，回调结束时释放 */
  GWeakRef   pane;      /* 详情页销毁后不再碰 dp */
  guint       generation;
} ThumbDone;

static void thumb_job_free(gpointer data)
{
  ThumbJob *j = data;

  g_free(j->path);
  g_free(j->cache_path);
  g_free(j);
}

static void thumb_result_free(gpointer data)
{
  ThumbResult *r = data;

  if (r->pb)
    g_object_unref(r->pb);
  g_free(r);
}

/* 缓存键 = 路径 + 源文件 size + mtime（秒级），避免改图后命中旧缩略图。 */
static gchar *thumb_cache_path(DetailPane *dp, const char *path)
{
  GStatBuf st;
  gchar *key, *hash, *dir, *file;

  if (!dp->ctx->cache_dir || !path || g_stat(path, &st) != 0)
    return NULL;
  key = g_strdup_printf("400x300|%s|%" G_GINT64_FORMAT "|%" G_GINT64_FORMAT,
                        path, (gint64) st.st_size,
                        (gint64) st.st_mtime);
  hash = g_compute_checksum_for_string(G_CHECKSUM_SHA1, key, -1);
  dir = g_build_filename(dp->ctx->cache_dir, "thumbs", NULL);
  g_mkdir_with_parents(dir, 0755);
  file = g_build_filename(dir, hash, NULL);
  {
    gchar *with_ext = g_strconcat(file, ".png", NULL);

    g_free(file);
    file = with_ext;
  }
  g_free(key);
  g_free(hash);
  g_free(dir);
  return file;
}

static gboolean thumb_cache_write(const char *cache_path, GdkPixbuf *pb)
{
  gchar *dir = g_path_get_dirname(cache_path);
  gchar *tmp = g_build_filename(dir, ".thumb.XXXXXX", NULL);
  GError *err = NULL;
  int fd;
  gboolean ok = FALSE;

  g_mkdir_with_parents(dir, 0755);
  fd = g_mkstemp(tmp);
  if (fd < 0)
    goto done;
  close(fd);
  if (gdk_pixbuf_save(pb, tmp, "png", &err, NULL)) {
    if (g_rename(tmp, cache_path) == 0)
      ok = TRUE;
  }
  g_clear_error(&err);
  if (!ok)
    g_unlink(tmp);

done:
  g_free(tmp);
  g_free(dir);
  return ok;
}

static void thumb_thread(GTask *task, gpointer source, gpointer data,
                         GCancellable *cancellable)
{
  ThumbJob *j = data;
  GdkPixbuf *pb = NULL;
  GError *err = NULL;
  gboolean cache_hit = FALSE;
  gint64 t0 = g_get_monotonic_time();

  (void) source;
  (void) cancellable;
  if (j->cache_path && g_file_test(j->cache_path, G_FILE_TEST_IS_REGULAR)) {
    pb = gdk_pixbuf_new_from_file_at_scale(j->cache_path, 400, 300, TRUE,
                                           &err);
    if (pb)
      cache_hit = TRUE;
    else
      g_clear_error(&err);
  }
  if (!pb) {
    pb = gdk_pixbuf_new_from_file_at_scale(j->path, 400, 300, TRUE, &err);
    if (pb && j->cache_path)
      (void) thumb_cache_write(j->cache_path, pb);
  }
  if (pb) {
    ThumbResult *r = g_new0(ThumbResult, 1);

    r->pb = pb;
    r->cache_hit = cache_hit;
    r->load_us = g_get_monotonic_time() - t0;
    g_task_return_pointer(task, r, thumb_result_free);
  } else {
    g_task_return_error(task, err);
  }
}

static gint64 PERF_DETAIL_T0;
static gboolean PERF_FIRST_THUMB;

static void thumb_done(GObject *source, GAsyncResult *res, gpointer data)
{
  ThumbDone *done = data;
  GtkWidget *pic = done->pic;
  GtkWidget *pane = g_weak_ref_get(&done->pane);
  DetailPane *dp = pane ? g_object_get_data(G_OBJECT(pane), "dp") : NULL;
  ThumbResult *r;
  GError *err = NULL;

  (void) source;
  r = g_task_propagate_pointer(G_TASK(res), &err);
  if (r && r->pb) {
    if (dp && done->generation == dp->thumb_generation) {
      GdkTexture *tex = gdk_texture_new_for_pixbuf(r->pb);

      gtk_picture_set_paintable(GTK_PICTURE(pic), GDK_PAINTABLE(tex));
      dp->thumbs_loaded++;
      if (r->cache_hit)
        dp->thumbs_cache_hits++;
      if (PERF_DETAIL_T0 && !PERF_FIRST_THUMB) {
        PERF_FIRST_THUMB = TRUE;
        g_printerr("perf: first_thumb_ms=%.1f thumb_load_ms=%.1f "
                   "cache_hit=%d\n",
                   (g_get_monotonic_time() - PERF_DETAIL_T0) / 1000.0,
                   r->load_us / 1000.0, r->cache_hit ? 1 : 0);
      }
      g_object_unref(tex);
    }
  } else {
    gtk_picture_set_paintable(GTK_PICTURE(pic), NULL);
    g_clear_error(&err);
  }
  if (r)
    thumb_result_free(r);
  g_clear_object(&pane);
  g_object_unref(pic);
  g_weak_ref_clear(&done->pane);
  g_free(done);
}

static void start_thumb(DetailPane *dp, GtkWidget *pic, const char *path)
{
  GTask *task;
  ThumbJob *job = g_new0(ThumbJob, 1);
  ThumbDone *done = g_new0(ThumbDone, 1);

  job->path = g_strdup(path);
  job->cache_path = thumb_cache_path(dp, path);
  done->pic = g_object_ref(pic);
  done->generation = dp->thumb_generation;
  g_weak_ref_init(&done->pane, dp->root);
  dp->thumbs_started++;
  task = g_task_new(NULL, NULL, thumb_done, done);
  g_task_set_task_data(task, job, thumb_job_free);
  g_task_run_in_thread(task, thumb_thread);
  g_object_unref(task);
}

/* --------------------------------------------------------------- 小部件 */

static void clear_box(GtkBox *box)
{
  GtkWidget *c;

  while ((c = gtk_widget_get_first_child(GTK_WIDGET(box))) != NULL)
    gtk_box_remove(box, c);
}

static void add_meta(DetailPane *dp, const char *key, const char *value)
{
  GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *k = gtk_label_new(key);
  GtkWidget *v = gtk_label_new(value && *value ? value : "—");

  (void) dp;
  gtk_label_set_xalign(GTK_LABEL(k), 0.0);
  gtk_widget_set_size_request(k, 110, -1);
  gtk_label_set_xalign(GTK_LABEL(v), 0.0);
  gtk_label_set_wrap(GTK_LABEL(v), TRUE);
  gtk_label_set_selectable(GTK_LABEL(v), TRUE);
  gtk_label_set_ellipsize(GTK_LABEL(v), PANGO_ELLIPSIZE_END);
  gtk_widget_set_hexpand(v, TRUE);
  gtk_box_append(GTK_BOX(row), k);
  gtk_box_append(GTK_BOX(row), v);
  gtk_box_append(dp->meta, row);
}

static void add_section_label(DetailPane *dp, const char *text)
{
  GtkWidget *l = gtk_label_new(text);

  gtk_label_set_xalign(GTK_LABEL(l), 0.0);
  gtk_widget_set_margin_top(l, 10);
  {
    PangoAttrList *attrs = pango_attr_list_new();
    pango_attr_list_insert(attrs, pango_attr_weight_new(PANGO_WEIGHT_BOLD));
    gtk_label_set_attributes(GTK_LABEL(l), attrs);
    pango_attr_list_unref(attrs);
  }
  gtk_box_append(dp->meta, l);
}

static void add_description(DetailPane *dp, const char *text)
{
  GtkWidget *sc = gtk_scrolled_window_new();
  GtkWidget *tv = gtk_text_view_new();

  add_section_label(dp, "Description");
  gtk_widget_set_size_request(sc, -1, 140);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sc),
                                 GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  gtk_text_view_set_editable(GTK_TEXT_VIEW(tv), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(tv), FALSE);
  gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(tv), GTK_WRAP_WORD_CHAR);
  gtk_text_view_set_left_margin(GTK_TEXT_VIEW(tv), 8);
  gtk_text_view_set_right_margin(GTK_TEXT_VIEW(tv), 8);
  gtk_text_view_set_top_margin(GTK_TEXT_VIEW(tv), 6);
  gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(tv), 6);
  gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(tv)),
                           text && *text ? text : "No description.", -1);
  gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sc), tv);
  gtk_box_append(dp->meta, sc);
}

static GtkWidget *image_page(DetailPane *dp, const RhDetailGroup *group)
{
  GtkWidget *flow = gtk_flow_box_new();
  guint n = 0;

  gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 3);
  gtk_flow_box_set_min_children_per_line(GTK_FLOW_BOX(flow), 1);
  gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
  gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 8);
  gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 8);

  for (guint i = 0; i < group->files->len; i++) {
    RhDetailFile *f = g_ptr_array_index(group->files, i);
    GtkWidget *item = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *pic = gtk_picture_new();
    GtkWidget *name = gtk_label_new(NULL);

    gtk_widget_set_size_request(pic, 220, 160);
    gtk_widget_set_size_request(item, 220, -1);
    gtk_picture_set_content_fit(GTK_PICTURE(pic), GTK_CONTENT_FIT_CONTAIN);
    gtk_picture_set_can_shrink(GTK_PICTURE(pic), TRUE);
    gtk_label_set_text(GTK_LABEL(name), f->filename);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_max_width_chars(GTK_LABEL(name), 24);

    gtk_box_append(GTK_BOX(item), pic);
    gtk_box_append(GTK_BOX(item), name);
    gtk_flow_box_insert(GTK_FLOW_BOX(flow), item, -1);
    n++;

    if (f->exists)
      start_thumb(dp, pic, f->path);
    else {
      gchar *missing = g_strdup_printf("%s (missing)", f->filename);
      gtk_label_set_text(GTK_LABEL(name), missing);
      g_free(missing);
    }
  }

  if (n == 0) {
    GtkWidget *l = gtk_label_new("No files in this group.");
    gtk_widget_set_margin_top(l, 16);
    return l;
  }
  return flow;
}

static void on_open_external(GtkButton *btn, gpointer data);

typedef struct {
  GtkWidget *video;
  GtkLabel  *hint;
  GtkWidget *root;
  gboolean   degraded;
  gboolean   ready;
  gboolean   played;
} VideoProbe;

/*
 * GtkVideo 在 GTK 的 GStreamer 后端缺少 playbin3 时会直接 g_error() 终止进程，
 * 不会给 GtkMediaStream 设置 error。这里用 GModule 动态查询 playbin3，缺失时
 * 根本不创建媒体流，改为外部打开。GUI 不因此增加 GStreamer 编译依赖。
 */
static gboolean inline_media_backend(void)
{
  static gint state = -1;
  GModule *mod;
  gpointer init_sym = NULL;
  gpointer find_sym = NULL;
  gpointer unref_sym = NULL;
  gboolean ok = FALSE;

  if (state >= 0)
    return state;
  state = 0;
  mod = g_module_open("libgstreamer-1.0.so.0", G_MODULE_BIND_LAZY);
  if (!mod)
    return FALSE;
  if (!g_module_symbol(mod, "gst_init_check", &init_sym) ||
      !g_module_symbol(mod, "gst_element_factory_find", &find_sym))
    return FALSE;
  g_module_symbol(mod, "gst_object_unref", &unref_sym);
  {
    typedef gboolean (*InitCheck)(int *, char **, GError **);
    typedef gpointer (*FactoryFind)(const char *);
    typedef void (*ObjectUnref)(gpointer);
    GError *err = NULL;

    if (((InitCheck) init_sym)(NULL, NULL, &err)) {
      gpointer factory = ((FactoryFind) find_sym)("playbin3");

      if (factory) {
        ok = TRUE;
        if (unref_sym)
          ((ObjectUnref) unref_sym)(factory);
      }
    }
    g_clear_error(&err);
  }
  state = ok ? 1 : 0;
  return ok;
}

static void degrade_video(VideoProbe *vp, const GError *err)
{
  DetailPane *dp;

  if (!vp || !vp->video || vp->degraded)
    return;
  vp->degraded = TRUE;
  gtk_widget_set_visible(vp->video, FALSE);
  if (vp->hint) {
    gchar *msg = g_strdup_printf(
      "Inline playback unavailable: %s Use \"Open externally\" instead.",
      err && err->message ? err->message : "media backend error.");
    gtk_label_set_text(vp->hint, msg);
    g_free(msg);
  }
  if (vp->root) {
    dp = g_object_get_data(G_OBJECT(vp->root), "dp");
    if (dp)
      dp->video_errors++;
  }
}

static void on_video_notify(GObject *obj, GParamSpec *pspec, gpointer data)
{
  VideoProbe *vp = data;
  GtkMediaStream *stream = GTK_MEDIA_STREAM(obj);
  const GError *err = gtk_media_stream_get_error(stream);
  DetailPane *dp;

  (void) pspec;
  if (err) {
    degrade_video(vp, err);
    return;
  }
  if (!vp || vp->ready || !gtk_media_stream_is_prepared(stream))
    return;
  vp->ready = TRUE;
  if (vp->root) {
    dp = g_object_get_data(G_OBJECT(vp->root), "dp");
    if (dp)
      dp->video_ready++;
  }
  if (vp->hint)
    gtk_label_set_text(vp->hint, "Inline playback ready.");
}

static void on_video_playing(GObject *obj, GParamSpec *pspec, gpointer data)
{
  VideoProbe *vp = data;
  DetailPane *dp;

  (void) pspec;
  if (!vp || vp->played || !gtk_media_stream_get_playing(GTK_MEDIA_STREAM(obj)))
    return;
  vp->played = TRUE;
  if (vp->root) {
    dp = g_object_get_data(G_OBJECT(vp->root), "dp");
    if (dp)
      dp->video_playing++;
  }
}

static GtkWidget *video_page(DetailPane *dp, const RhDetailGroup *group)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gboolean backend;

  if (group->files->len == 0) {
    GtkWidget *l = gtk_label_new("No videos.");
    gtk_widget_set_margin_top(l, 16);
    return l;
  }
  backend = inline_media_backend();

  for (guint i = 0; i < group->files->len; i++) {
    RhDetailFile *f = g_ptr_array_index(group->files, i);
    GtkWidget *video = gtk_video_new();
    GtkWidget *name = gtk_label_new(f->filename);
    GtkWidget *open = gtk_button_new_with_label("Open externally");
    GtkWidget *hint = gtk_label_new(
      "Inline playback needs the system media backend; use Open externally if it fails.");
    GtkMediaStream *stream = NULL;
    VideoProbe *probe = NULL;

    gtk_label_set_xalign(GTK_LABEL(name), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(name), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_xalign(GTK_LABEL(hint), 0.0);
    gtk_label_set_wrap(GTK_LABEL(hint), TRUE);
    gtk_video_set_autoplay(GTK_VIDEO(video), FALSE);
    gtk_widget_set_size_request(video, 480, 270);
    if (!backend) {
      gtk_widget_set_visible(video, FALSE);
      gtk_label_set_text(GTK_LABEL(hint),
                         "No inline media backend; use Open externally.");
      dp->video_errors++;
    } else if (f->exists) {
      GFile *file = g_file_new_for_path(f->path);

      gtk_video_set_file(GTK_VIDEO(video), file);
      g_object_unref(file);
      stream = gtk_video_get_media_stream(GTK_VIDEO(video));
    } else {
      gtk_widget_set_visible(video, FALSE);
      gtk_label_set_text(GTK_LABEL(hint), "File is missing; use Open externally.");
    }
    if (stream) {
      probe = g_new0(VideoProbe, 1);
      probe->video = video;
      probe->hint = GTK_LABEL(hint);
      probe->root = dp->root;
      g_object_set_data_full(G_OBJECT(stream), "video-probe", probe, g_free);
      g_signal_connect(stream, "notify::error", G_CALLBACK(on_video_notify),
                       probe);
      g_signal_connect(stream, "notify::prepared", G_CALLBACK(on_video_notify),
                       probe);
      g_signal_connect(stream, "notify::playing", G_CALLBACK(on_video_playing),
                       probe);
      if (!dp->first_playable_video && f->size > 0)
        dp->first_playable_video = video;
      /* 后端可能在 set_file 时已同步设置 error/prepared，补一次当前状态检查。 */
      on_video_notify(G_OBJECT(stream), NULL, probe);
    }
    gtk_box_append(GTK_BOX(box), name);
    gtk_box_append(GTK_BOX(box), video);
    gtk_box_append(GTK_BOX(box), open);
    gtk_box_append(GTK_BOX(box), hint);
    dp->videos++;
    g_object_set_data_full(G_OBJECT(open), "path", g_strdup(f->path), g_free);
    g_signal_connect(open, "clicked", G_CALLBACK(on_open_external), NULL);
  }
  return box;
}

static void on_open_external(GtkButton *btn, gpointer data)
{
  const char *path = g_object_get_data(G_OBJECT(btn), "path");
  GtkRoot *root = gtk_widget_get_root(GTK_WIDGET(btn));
  GtkUriLauncher *launcher;
  gchar *uri;

  (void) data;
  if (!path)
    return;
  uri = g_filename_to_uri(path, NULL, NULL);
  if (!uri)
    return;
  launcher = gtk_uri_launcher_new(uri);
  if (root && GTK_IS_WINDOW(root))
    gtk_uri_launcher_launch(launcher, GTK_WINDOW(root), NULL, NULL, NULL);
  else
    gtk_uri_launcher_launch(launcher, NULL, NULL, NULL, NULL);
  g_object_unref(launcher);
  g_free(uri);
}

static GtkWidget *file_page(const RhDetailGroup *group)
{
  GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

  if (group->files->len == 0) {
    GtkWidget *l = gtk_label_new("No files in this group.");
    gtk_widget_set_margin_top(l, 16);
    return l;
  }
  for (guint i = 0; i < group->files->len; i++) {
    RhDetailFile *f = g_ptr_array_index(group->files, i);
    gchar *size = f->size >= 0 ? g_format_size(f->size) : g_strdup("?");
    gchar *text = g_strdup_printf("%s%s    %s",
                                  f->filename,
                                  f->exists ? "" : "  (missing)", size);
    GtkWidget *l = gtk_label_new(text);

    gtk_label_set_xalign(GTK_LABEL(l), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
    gtk_label_set_selectable(GTK_LABEL(l), TRUE);
    gtk_box_append(GTK_BOX(box), l);
    g_free(size);
    g_free(text);
  }
  return box;
}

static gboolean is_image_group(const char *kind)
{
  return g_strcmp0(kind, "covers") == 0 ||
         g_strcmp0(kind, "screenshots") == 0 ||
         g_strcmp0(kind, "marquees") == 0 ||
         g_strcmp0(kind, "miximages") == 0;
}

static const char *group_label(const char *kind)
{
  if (g_strcmp0(kind, "rom") == 0) return "ROM";
  if (g_strcmp0(kind, "covers") == 0) return "Covers";
  if (g_strcmp0(kind, "screenshots") == 0) return "Screenshots";
  if (g_strcmp0(kind, "videos") == 0) return "Videos";
  if (g_strcmp0(kind, "marquees") == 0) return "Marquees";
  if (g_strcmp0(kind, "miximages") == 0) return "Miximages";
  if (g_strcmp0(kind, "intro") == 0) return "Manuals";
  if (g_strcmp0(kind, "patcher") == 0) return "Patches";
  return kind;
}

static void clear_stack(GtkStack *stack)
{
  GtkWidget *c;

  while ((c = gtk_widget_get_first_child(GTK_WIDGET(stack))) != NULL)
    gtk_stack_remove(stack, c);
}

/* --------------------------------------------------------------- 构建 */

GtkWidget *detail_pane_new(RhCtx *ctx)
{
  const char *path = "/io/github/goddog312/RomHamster/ui/detail-pane.ui";
  GError *err = NULL;
  GBytes *bytes = g_resources_lookup_data(path, 0, &err);
  GtkBuilder *b;
  GtkWidget *root;

  if (!bytes) {
    g_warning("cannot load detail UI: %s", err ? err->message : "unknown");
    g_clear_error(&err);
    return NULL;
  }
  b = gtk_builder_new_from_string(g_bytes_get_data(bytes, NULL),
                                  g_bytes_get_size(bytes));
  g_bytes_unref(bytes);
  root = GTK_WIDGET(gtk_builder_get_object(b, "detail_root"));
  if (!root) {
    g_warning("detail UI has no detail_root");
    g_object_unref(b);
    return NULL;
  }
  DetailPane *dp = g_new0(DetailPane, 1);

  dp->ctx = ctx;
  dp->root = root;
  dp->media = GTK_STACK(gtk_builder_get_object(b, "media_stack"));
  dp->meta = GTK_BOX(gtk_builder_get_object(b, "detail_meta_box"));
  dp->title = GTK_LABEL(gtk_builder_get_object(b, "detail_title"));
  gtk_stack_switcher_set_stack(
    GTK_STACK_SWITCHER(gtk_builder_get_object(b, "media_switcher")), dp->media);
  g_object_set_data_full(G_OBJECT(root), "dp", dp, g_free);
  g_object_set_data(G_OBJECT(root), "back",
                    gtk_builder_get_object(b, "btn_back"));
  g_object_ref(root);   /* builder 释放后 root 仍归调用方 */
  g_object_unref(b);
  return root;
}

GtkWidget *detail_pane_back_button(GtkWidget *pane)
{
  return GTK_WIDGET(g_object_get_data(G_OBJECT(pane), "back"));
}

guint detail_pane_video_count(GtkWidget *pane)
{
  DetailPane *dp = g_object_get_data(G_OBJECT(pane), "dp");

  return dp ? dp->videos : 0;
}

guint detail_pane_video_errors(GtkWidget *pane)
{
  DetailPane *dp = g_object_get_data(G_OBJECT(pane), "dp");

  return dp ? dp->video_errors : 0;
}

guint detail_pane_video_ready(GtkWidget *pane)
{
  DetailPane *dp = g_object_get_data(G_OBJECT(pane), "dp");

  return dp ? dp->video_ready : 0;
}

guint detail_pane_video_playing(GtkWidget *pane)
{
  DetailPane *dp = g_object_get_data(G_OBJECT(pane), "dp");

  return dp ? dp->video_playing : 0;
}

gboolean detail_pane_play_first_video(GtkWidget *pane)
{
  DetailPane *dp = g_object_get_data(G_OBJECT(pane), "dp");
  GtkMediaStream *stream;

  if (!dp || !dp->first_playable_video)
    return FALSE;
  stream = gtk_video_get_media_stream(GTK_VIDEO(dp->first_playable_video));
  if (!stream)
    return FALSE;
  gtk_media_stream_play(stream);
  return TRUE;
}

gboolean detail_pane_select_page(GtkWidget *pane, const char *name)
{
  DetailPane *dp = g_object_get_data(G_OBJECT(pane), "dp");

  if (!dp || !dp->media || !gtk_stack_get_child_by_name(dp->media, name))
    return FALSE;
  gtk_stack_set_visible_child_name(dp->media, name);
  return TRUE;
}

guint detail_pane_thumbs_loaded(GtkWidget *pane)
{
  DetailPane *dp = g_object_get_data(G_OBJECT(pane), "dp");

  return dp ? dp->thumbs_loaded : 0;
}

guint detail_pane_thumbs_started(GtkWidget *pane)
{
  DetailPane *dp = g_object_get_data(G_OBJECT(pane), "dp");

  return dp ? dp->thumbs_started : 0;
}

guint detail_pane_thumbs_cache_hits(GtkWidget *pane)
{
  DetailPane *dp = g_object_get_data(G_OBJECT(pane), "dp");

  return dp ? dp->thumbs_cache_hits : 0;
}

guint detail_pane_page_count(GtkWidget *pane)
{
  DetailPane *dp = g_object_get_data(G_OBJECT(pane), "dp");
  GtkWidget *c;
  guint n = 0;

  if (!dp || !dp->media)
    return 0;
  for (c = gtk_widget_get_first_child(GTK_WIDGET(dp->media)); c;
       c = gtk_widget_get_next_sibling(c))
    n++;
  return n;
}

const char *detail_pane_title(GtkWidget *pane)
{
  DetailPane *dp = g_object_get_data(G_OBJECT(pane), "dp");

  return dp && dp->title ? gtk_label_get_text(dp->title) : "";
}

void detail_pane_show(GtkWidget *pane, const char *game_id)
{
  DetailPane *dp = g_object_get_data(G_OBJECT(pane), "dp");
  RhGameDetail *d = NULL;
  RhErr e;
  gchar *title;
  gint64 perf_t0 = g_getenv("RH_PERF") ? g_get_monotonic_time() : 0;

  PERF_DETAIL_T0 = perf_t0;
  PERF_FIRST_THUMB = FALSE;
  clear_box(dp->meta);
  clear_stack(dp->media);
  dp->thumb_generation++;
  dp->thumbs_started = 0;
  dp->thumbs_loaded = 0;
  dp->thumbs_cache_hits = 0;
  dp->videos = 0;
  dp->video_errors = 0;
  dp->video_ready = 0;
  dp->video_playing = 0;
  dp->first_playable_video = NULL;

  e = rh_detail_load(dp->ctx, game_id, &d);
  if (!rh_err_ok(&e)) {
    gtk_label_set_text(dp->title, "Detail unavailable");
    add_meta(dp, "Error", e.msg);
    return;
  }

  if (d->game->year > 0)
    title = g_strdup_printf("%s · %s · %d",
                            d->game->name ? d->game->name : game_id,
                            d->game->platform ? d->game->platform : "",
                            d->game->year);
  else
    title = g_strdup_printf("%s · %s",
                            d->game->name ? d->game->name : game_id,
                            d->game->platform ? d->game->platform : "");
  gtk_label_set_text(dp->title, title);
  g_free(title);

  add_meta(dp, "Platform", d->game->platform);
  {
    char buf[32];
    snprintf(buf, sizeof buf, "%d", d->game->year);
    add_meta(dp, "Year", d->game->year > 0 ? buf : "");
  }
  add_meta(dp, "Genre", d->game->genre);
  add_meta(dp, "Developer", d->game->developer);
  add_meta(dp, "Publisher", d->game->publisher);
  add_meta(dp, "Region", d->game->region);
  {
    char buf[32];
    snprintf(buf, sizeof buf, "%d", d->game->players);
    add_meta(dp, "Players", d->game->players > 0 ? buf : "");
  }
  {
    char buf[32];
    snprintf(buf, sizeof buf, "%.2f", d->game->rating);
    add_meta(dp, "Rating", d->game->rating >= 0 ? buf : "");
  }
  add_meta(dp, "Status", d->game->rom_status);
  add_meta(dp, "Format", d->game->rom_format);
  add_meta(dp, "Multi-disc", d->game->is_multi_disc ? "yes" : "no");
  add_meta(dp, "Patch", d->game->patch_type);
  add_meta(dp, "DAT", d->game->dat_source);
  add_meta(dp, "Directory", d->game->dir_path);
  add_description(dp, d->game->description);

  for (int i = 0; i < RH_DETAIL_GROUP_COUNT; i++) {
    RhDetailGroup *grp = &d->groups[i];
    GtkWidget *page;

    if (is_image_group(grp->kind))
      page = image_page(dp, grp);
    else if (g_strcmp0(grp->kind, "videos") == 0)
      page = video_page(dp, grp);
    else
      page = file_page(grp);

    gtk_stack_add_titled(dp->media, page, grp->kind, group_label(grp->kind));
  }
  gtk_stack_set_visible_child_name(dp->media, "rom");
  if (perf_t0)
    g_printerr("perf: detail_ms=%.1f\n",
               (g_get_monotonic_time() - perf_t0) / 1000.0);
  rh_detail_free(d);
}
