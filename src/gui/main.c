/*
 * rom-hamster-gui（M0 空壳）
 *
 * M0 只验证："链接了 GTK、能起一个窗口"，数据层与界面在 M4 接入。
 */
#include <gtk/gtk.h>

#include "rh-version.h"

/* 冒烟自检：CI/无显示器环境下用它证明"窗口真的被 map 了"而不是空跑退出。
 * 只在 RH_SMOKE_TEST=1 时生效，正常运行不受影响。 */
static gboolean smoke_check(gpointer data)
{
  GtkWindow *win = GTK_WINDOW(data);

  if (gtk_widget_get_mapped(GTK_WIDGET(win)))
    g_printerr("smoke: window mapped\n");
  else {
    g_printerr("smoke: window NOT mapped\n");
    g_application_quit(G_APPLICATION(gtk_window_get_application(win)));
    return G_SOURCE_REMOVE;
  }

  g_application_quit(G_APPLICATION(gtk_window_get_application(win)));
  return G_SOURCE_REMOVE;
}

static void on_activate(GtkApplication *app, gpointer user_data)
{
  GtkWidget *win;
  GtkWidget *label;

  (void) user_data;

  win = gtk_application_window_new(app);
  gtk_window_set_title(GTK_WINDOW(win), "rom-hamster");
  gtk_window_set_default_size(GTK_WINDOW(win), 1000, 700);

  label = gtk_label_new("M0 skeleton: core store and rules arrive in M1.");
  gtk_widget_set_margin_top(label, 24);
  gtk_widget_set_margin_bottom(label, 24);
  gtk_widget_set_margin_start(label, 24);
  gtk_widget_set_margin_end(label, 24);

  gtk_window_set_child(GTK_WINDOW(win), label);
  gtk_window_present(GTK_WINDOW(win));

  if (g_getenv("RH_SMOKE_TEST"))
    g_timeout_add(800, smoke_check, win);
}

int main(int argc, char **argv)
{
  GtkApplication *app;
  int status;

#if GLIB_CHECK_VERSION(2, 74, 0)
  app = gtk_application_new("io.github.goddog312.RomHamster",
                            G_APPLICATION_DEFAULT_FLAGS);
#else
  app = gtk_application_new("io.github.goddog312.RomHamster",
                            G_APPLICATION_FLAGS_NONE);
#endif

  g_signal_connect(app, "activate", G_CALLBACK(on_activate), NULL);
  status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);
  return status;
}
