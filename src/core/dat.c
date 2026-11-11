/*
 * dat.c — Logiqx DAT 解析（libxml2）
 */
#include "dat.h"

#include <libxml/parser.h>
#include <libxml/tree.h>
#include <string.h>

#include "rh-str.h"
#include "store.h"

static gchar *prop(xmlNode *n, const char *name)
{
  xmlChar *v = xmlGetProp(n, (const xmlChar *) name);

  if (!v)
    return NULL;
  gchar *out = g_strdup((const gchar *) v);

  xmlFree(v);
  return out;
}

static xmlNode *child_named(xmlNode *parent, const char *name)
{
  for (xmlNode *n = parent->children; n; n = n->next) {
    if (n->type == XML_ELEMENT_NODE &&
        xmlStrcasecmp(n->name, (const xmlChar *) name) == 0)
      return n;
  }
  return NULL;
}

static gchar *node_text(xmlNode *n)
{
  xmlChar *t = xmlNodeGetContent(n);

  if (!t)
    return NULL;
  gchar *out = rh_str_trim_dup((const char *) t);

  xmlFree(t);
  return out;
}

RhErr rh_dat_parse(const char *path, GPtrArray **entries,
                   gchar **name, gchar **version)
{
  xmlDoc *doc;
  xmlNode *root;
  GPtrArray *list;
  gchar *dat_name = NULL, *dat_version = NULL;

  g_return_val_if_fail(entries != NULL, rh_err_from(RH_ERR_INTERNAL));
  *entries = NULL;
  if (name) *name = NULL;
  if (version) *version = NULL;

  doc = xmlReadFile(path, NULL, XML_PARSE_NONET | XML_PARSE_NOBLANKS |
                                  XML_PARSE_HUGE);
  if (!doc)
    return rh_fail(RH_ERR_PARSE, "DAT 无法解析：%s", path);

  root = xmlDocGetRootElement(doc);
  if (!root) {
    xmlFreeDoc(doc);
    return rh_fail(RH_ERR_PARSE, "DAT 为空：%s", path);
  }

  /* <header><name> / <version> */
  {
    xmlNode *header = child_named(root, "header");

    if (header) {
      xmlNode *nn = child_named(header, "name");
      xmlNode *vn = child_named(header, "version");

      if (nn) dat_name = node_text(nn);
      if (vn) dat_version = node_text(vn);
    }
  }

  list = g_ptr_array_new_with_free_func((GDestroyNotify) rh_dat_entry_free);
  for (xmlNode *n = root->children; n; n = n->next) {
    const gchar *gname = NULL;

    if (n->type != XML_ELEMENT_NODE)
      continue;
    if (xmlStrcasecmp(n->name, (const xmlChar *) "game") != 0 &&
        xmlStrcasecmp(n->name, (const xmlChar *) "machine") != 0)
      continue;

    gname = NULL;
    {
      xmlNode *dn = child_named(n, "description");
      gchar *desc = dn ? node_text(dn) : NULL;

      gname = desc;
      for (xmlNode *r = n->children; r; r = r->next) {
        RhDatEntry *e;
        gchar *rom_name, *crc, *md5, *sha1, *size;

        if (r->type != XML_ELEMENT_NODE ||
            xmlStrcasecmp(r->name, (const xmlChar *) "rom") != 0)
          continue;
        rom_name = prop(r, "name");
        if (!rom_name)
          continue;
        crc = prop(r, "crc");
        md5 = prop(r, "md5");
        sha1 = prop(r, "sha1");
        size = prop(r, "size");

        e = rh_dat_entry_new();
        e->game_name = g_strdup(gname ? gname : rom_name);
        e->rom_name = rom_name;
        e->size = size ? g_ascii_strtoll(size, NULL, 10) : -1;
        e->crc32 = crc ? g_ascii_strdown(crc, -1) : NULL;
        e->md5 = md5 ? g_ascii_strdown(md5, -1) : NULL;
        e->sha1 = sha1 ? g_ascii_strdown(sha1, -1) : NULL;
        g_free(crc);
        g_free(md5);
        g_free(sha1);
        g_free(size);
        g_ptr_array_add(list, e);
      }
      g_free(desc);
    }
  }
  xmlFreeDoc(doc);

  if (list->len == 0) {
    g_ptr_array_unref(list);
    g_free(dat_name);
    g_free(dat_version);
    return rh_fail(RH_ERR_PARSE, "DAT 内没有任何 rom 条目：%s", path);
  }

  *entries = list;
  if (name) *name = dat_name; else g_free(dat_name);
  if (version) *version = dat_version; else g_free(dat_version);
  return rh_ok();
}

RhErr rh_dat_import_file(RhCtx *ctx, const char *platform, const char *path,
                         gint64 *dat_id)
{
  GPtrArray *entries = NULL;
  gchar *name = NULL, *version = NULL;
  RhErr e;
  gint64 id = 0;

  e = rh_dat_parse(path, &entries, &name, &version);
  if (!rh_err_ok(&e))
    return e;

  e = store_dat_begin(ctx, platform, path, name,
                      version, &id);
  if (!rh_err_ok(&e)) {
    g_ptr_array_unref(entries);
    g_free(name);
    g_free(version);
    return e;
  }
  e = store_dat_entries_add(ctx, id, entries);
  if (rh_err_ok(&e))
    e = store_dat_finish(ctx, id, (gint64) entries->len);

  g_ptr_array_unref(entries);
  g_free(name);
  g_free(version);
  if (dat_id)
    *dat_id = id;
  return e;
}

RhErr rh_dat_match_rom(RhCtx *ctx, const char *platform, const char *crc32,
                       const char *md5, const char *sha1, RhDatMatch **out)
{
  const gchar *algo = "crc32";
  const gchar *hash = crc32;
  GPtrArray *res = NULL;
  RhErr e;

  if (!out)
    return rh_fail(RH_ERR_CONFIG, "rh_dat_match_rom: out is NULL");
  *out = NULL;
  if (md5 && *md5) {
    algo = "md5";
    hash = md5;
  } else if (sha1 && *sha1) {
    algo = "sha1";
    hash = sha1;
  }
  if (!hash || !*hash)
    return rh_ok();

  e = store_dat_find_by_hash(ctx, algo, hash, &res);
  if (!rh_err_ok(&e))
    return e;
  if (!res)
    return rh_ok();

  for (guint i = 0; i < res->len; i++) {
    RhDatMatch *m = res->pdata[i];

    if (platform && m->platform && g_ascii_strcasecmp(m->platform, platform) != 0)
      continue;
    /* 复制一份再释放数组：数组的 free_func 会连元素一起释放 */
    *out = rh_dat_match_new();
    (*out)->dat_id = m->dat_id;
    (*out)->platform = g_strdup(m->platform);
    (*out)->dat_name = g_strdup(m->dat_name);
    (*out)->game_name = g_strdup(m->game_name);
    (*out)->rom_name = g_strdup(m->rom_name);
    (*out)->size = m->size;
    break;
  }
  g_ptr_array_unref(res);
  return rh_ok();
}