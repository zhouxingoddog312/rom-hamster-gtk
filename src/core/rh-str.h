/*
 * rh-str.h — 字符串工具（ARCHITECTURE §5：util.sh 的 rh-str 部分）
 *
 * 纯函数，不读盘、不打印、不碰数据库。返回的字符串一律由调用方 g_free。
 */
#pragma once

#include <glib.h>

/* 去首尾空白（UTF-8 安全）；无内容时返回空串 */
gchar *rh_str_trim_dup(const char *s);
gchar *rh_str_lower_dup(const char *s);

/* 最后一个扩展名，小写、含点；无扩展名返回 "" */
gchar *rh_ext_of(const char *path);

/* 去掉最后一个扩展名的 basename */
gchar *rh_base_noext(const char *path);

/* 去掉完整扩展名（调用方给出 ext，如 ".p8.png"）；ext 为 NULL 时等价 rh_base_noext */
gchar *rh_rom_base_name(const char *path, const char *ext);

/*
 * 目录名消毒：替换 / \ : * ? " < > | 为 '_'，删控制字符，折叠空白，
 * 截断到 180 字符。用于 dir_path（与 1.0.0 布局兼容，ARCHITECTURE §5）。
 */
gchar *rh_sanitize_dirname(const char *name);

/*
 * 从主名识别碟号：disc/disk/cd/dvd/side + 数字或字母。
 * 返回碟号（0 = 无标记）；*out_main 为去掉标记后的主名（新分配）。
 */
int rh_parse_disc_no(const char *stem, gchar **out_main);

/* 名称相似度 0–100（LCS 比例，1.0.0 的 name_sim 同算法） */
int rh_name_sim(const char *a, const char *b);
