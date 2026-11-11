#include "game-model.h"

struct _RhGameRow {
  GObject parent_instance;
  gchar *id;
  gchar *name;
  gchar *platform;
  gchar *year;
  gchar *genre;
  gchar *rating;
};

G_DEFINE_TYPE(RhGameRow, rh_game_row, G_TYPE_OBJECT)

static void row_finalize(GObject *obj)
{
  RhGameRow *r = RH_GAME_ROW(obj);
  g_free(r->id);
  g_free(r->name);
  g_free(r->platform);
  g_free(r->year);
  g_free(r->genre);
  g_free(r->rating);
  G_OBJECT_CLASS(rh_game_row_parent_class)->finalize(obj);
}

static void rh_game_row_class_init(RhGameRowClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = row_finalize;
}

static void rh_game_row_init(RhGameRow *r)
{
  (void) r;
}

static RhGameRow *row_new(const RhGame *g)
{
  RhGameRow *r = g_object_new(RH_TYPE_GAME_ROW, NULL);

  r->id = g_strdup(g->id ? g->id : "");
  r->name = g_strdup(g->name ? g->name : "");
  r->platform = g_strdup(g->platform ? g->platform : "");
  r->year = g->year > 0 ? g_strdup_printf("%d", g->year) : g_strdup("");
  r->genre = g_strdup(g->genre ? g->genre : "");
  r->rating = g->rating >= 0 ? g_strdup_printf("%.2f", g->rating)
                             : g_strdup("");
  return r;
}

const char *rh_game_row_id(RhGameRow *r) { return r->id; }
const char *rh_game_row_name(RhGameRow *r) { return r->name; }
const char *rh_game_row_platform(RhGameRow *r) { return r->platform; }
const char *rh_game_row_year(RhGameRow *r) { return r->year; }
const char *rh_game_row_genre(RhGameRow *r) { return r->genre; }
const char *rh_game_row_rating(RhGameRow *r) { return r->rating; }

struct _RhGameListModel {
  GObject parent_instance;
  GPtrArray *rows;   /* RhGameRow* */
};

static void list_model_iface_init(GListModelInterface *iface);

G_DEFINE_TYPE_WITH_CODE(RhGameListModel, rh_game_list_model, G_TYPE_OBJECT,
                        G_IMPLEMENT_INTERFACE(G_TYPE_LIST_MODEL,
                                              list_model_iface_init))

static GType list_get_item_type(GListModel *model)
{
  (void) model;
  return RH_TYPE_GAME_ROW;
}

static guint list_get_n_items(GListModel *model)
{
  return RH_GAME_LIST_MODEL(model)->rows->len;
}

static gpointer list_get_item(GListModel *model, guint pos)
{
  RhGameListModel *m = RH_GAME_LIST_MODEL(model);
  if (pos >= m->rows->len)
    return NULL;
  return g_object_ref(g_ptr_array_index(m->rows, pos));
}

static void list_model_iface_init(GListModelInterface *iface)
{
  iface->get_item_type = list_get_item_type;
  iface->get_n_items = list_get_n_items;
  iface->get_item = list_get_item;
}

static void list_finalize(GObject *obj)
{
  RhGameListModel *m = RH_GAME_LIST_MODEL(obj);
  g_ptr_array_unref(m->rows);
  G_OBJECT_CLASS(rh_game_list_model_parent_class)->finalize(obj);
}

static void rh_game_list_model_class_init(RhGameListModelClass *klass)
{
  G_OBJECT_CLASS(klass)->finalize = list_finalize;
}

static void rh_game_list_model_init(RhGameListModel *m)
{
  m->rows = g_ptr_array_new_with_free_func(g_object_unref);
}

RhGameListModel *rh_game_list_model_new(void)
{
  return g_object_new(RH_TYPE_GAME_LIST_MODEL, NULL);
}

void rh_game_list_model_set_games(RhGameListModel *m, GPtrArray *games)
{
  guint old_n = m->rows->len;
  guint new_n = games ? games->len : 0;

  g_ptr_array_set_size(m->rows, 0);
  for (guint i = 0; i < new_n; i++)
    g_ptr_array_add(m->rows, row_new(g_ptr_array_index(games, i)));

  if (games)
    g_ptr_array_unref(games);

  g_list_model_items_changed(G_LIST_MODEL(m), 0, old_n, new_n);
}

guint rh_game_list_model_get_count(RhGameListModel *m)
{
  return m->rows->len;
}
