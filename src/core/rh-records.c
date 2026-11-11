#include "rh-records.h"

#include <string.h>

/* --------------------------------------------------------------- 工具 */

gchar *rh_now_iso(void)
{
  GDateTime *dt = g_date_time_new_now_local();
  gchar     *s = g_date_time_format_iso8601(dt);

  g_date_time_unref(dt);
  return s;
}

gchar *rh_game_gen_id(const char *platform, const char *dir_path)
{
  const gchar *plat = (platform && *platform) ? platform : "game";
  gchar *key, *sum, *short_sum, *id;

  if (!dir_path || !*dir_path)
    return NULL;

  key = g_strdup_printf("%s\n%s", plat, dir_path);
  sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, key, -1);
  short_sum = g_strndup(sum, 12);                       /* 12 位足够避免碰撞 */
  id = g_strdup_printf("%s-%s", plat, short_sum);

  g_free(key);
  g_free(sum);
  g_free(short_sum);
  return id;
}

static void free_str(gchar **p)
{
  g_free(*p);
  *p = NULL;
}

/* --------------------------------------------------------------- RhGame */

RhGame *rh_game_new(void)
{
  RhGame *g = g_new0(RhGame, 1);

  g->year = 0;
  g->players = 0;
  g->rating = -1.0;
  g->rom_format = g_strdup("raw");
  g->rom_status = g_strdup("unknown");
  g->patch_type = g_strdup("");
  return g;
}

void rh_game_free(RhGame *g)
{
  if (!g)
    return;
  free_str(&g->id);
  free_str(&g->platform);
  free_str(&g->name);
  free_str(&g->game_main_name);
  free_str(&g->dir_path);
  free_str(&g->developer);
  free_str(&g->publisher);
  free_str(&g->genre);
  free_str(&g->region);
  free_str(&g->description);
  free_str(&g->rom_format);
  free_str(&g->rom_status);
  free_str(&g->dat_source);
  free_str(&g->patch_type);
  free_str(&g->created_at);
  free_str(&g->updated_at);
  g_free(g);
}

#define DUP(field) out->field = g_strdup(src->field)
#define DUP_IF(field) out->field = src->field ? g_strdup(src->field) : NULL

RhGame *rh_game_copy(const RhGame *src)
{
  RhGame *out;

  if (!src)
    return NULL;

  out = g_new0(RhGame, 1);
  DUP(id);
  DUP(platform);
  DUP(name);
  DUP_IF(game_main_name);
  DUP(dir_path);
  DUP_IF(developer);
  DUP_IF(publisher);
  out->year = src->year;
  DUP_IF(genre);
  out->players = src->players;
  out->rating = src->rating;
  DUP_IF(region);
  DUP_IF(description);
  out->is_multi_disc = src->is_multi_disc;
  out->is_arcade = src->is_arcade;
  DUP(rom_format);
  DUP(rom_status);
  DUP_IF(dat_source);
  DUP(patch_type);
  DUP_IF(created_at);
  DUP_IF(updated_at);
  return out;
}

/* --------------------------------------------------------------- 子表 */

RhRomFile *rh_rom_file_new(void)
{
  RhRomFile *r = g_new0(RhRomFile, 1);

  r->disc_no = 0;
  r->size = -1;
  return r;
}

void rh_rom_file_free(RhRomFile *r)
{
  if (!r)
    return;
  free_str(&r->filename);
  free_str(&r->crc32);
  free_str(&r->md5);
  free_str(&r->sha1);
  g_free(r);
}

RhMediaFile *rh_media_file_new(void)
{
  RhMediaFile *m = g_new0(RhMediaFile, 1);

  m->size = -1;
  return m;
}

void rh_media_file_free(RhMediaFile *m)
{
  if (!m)
    return;
  free_str(&m->kind);
  free_str(&m->filename);
  g_free(m);
}

RhDatMatch *rh_dat_match_new(void)
{
  RhDatMatch *m = g_new0(RhDatMatch, 1);

  m->size = -1;
  return m;
}

void rh_dat_match_free(RhDatMatch *m)
{
  if (!m)
    return;
  free_str(&m->platform);
  free_str(&m->dat_name);
  free_str(&m->game_name);
  free_str(&m->rom_name);
  g_free(m);
}

RhDatEntry *rh_dat_entry_new(void)
{
  RhDatEntry *e = g_new0(RhDatEntry, 1);

  e->size = -1;
  return e;
}

void rh_dat_entry_free(RhDatEntry *e)
{
  if (!e)
    return;
  free_str(&e->game_name);
  free_str(&e->rom_name);
  free_str(&e->crc32);
  free_str(&e->md5);
  free_str(&e->sha1);
  g_free(e);
}

RhJobItem *rh_job_item_new(void)
{
  RhJobItem *it = g_new0(RhJobItem, 1);

  it->state = g_strdup("pending");
  return it;
}

void rh_job_item_free(RhJobItem *it)
{
  if (!it)
    return;
  free_str(&it->src);
  free_str(&it->action);
  free_str(&it->platform);
  free_str(&it->target_id);
  free_str(&it->state);
  free_str(&it->message);
  g_free(it);
}

/* --------------------------------------------------------------- 筛选 */
RhFilter *rh_filter_new(void)
{
  RhFilter *f = g_new0(RhFilter, 1);

  f->multi = -1;
  f->has_patch = -1;
  f->sort = "name";
  return f;
}

void rh_filter_free(RhFilter *f)
{
  /* 条件字段一律"借用"调用方内存，这里只释放结构体本身 */
  g_free(f);
}

/* --------------------------------------------------------------- 统计 */

RhPlatformStat *rh_platform_stat_new(void)
{
  return g_new0(RhPlatformStat, 1);
}

void rh_platform_stat_free(RhPlatformStat *s)
{
  if (!s)
    return;
  g_free(s->platform);
  g_free(s);
}

/* --------------------------------------------------------------- 作业 */

RhJob *rh_job_new(void)
{
  return g_new0(RhJob, 1);
}

void rh_job_free(RhJob *j)
{
  if (!j)
    return;
  free_str(&j->kind);
  free_str(&j->state);
  free_str(&j->created_at);
  free_str(&j->finished_at);
  free_str(&j->error);
  g_free(j);
}
