/*
 * emulator.h — emulators.conf 解析与启动命令（ARCHITECTURE §7.1）
 *
 * 载体是数据文件，不在核心写死模拟器路径：
 *   <平台ID>: <命令模板>
 * 占位符：%ROM%（选中 ROM 的绝对路径）、%DIR%（游戏目录）。
 * 兼容旧配置键 emulator_launch 作为全局兜底模板，以及 {file}/{dir} 写法。
 */
#pragma once

#include <glib.h>

#include "detail.h"
#include "rh-ctx.h"
#include "rh-err.h"

typedef struct RhEmuRules RhEmuRules;

RhEmuRules *rh_emu_rules_new(void);
void        rh_emu_rules_free(RhEmuRules *r);

/* 读一份 emulators.conf；文件不存在不是错误。 */
RhErr rh_emu_rules_load_file(RhEmuRules *r, const char *path);

/* factory → /etc → ~/.config，后者覆盖前者。 */
RhErr rh_emu_rules_load_standard(RhEmuRules *r);

const gchar *rh_emu_rules_lookup(RhEmuRules *r, const char *platform);

/*
 * 把模板展开成 argv。
 * 成功时 *argv_out 由 g_strfreev 释放；*command_out 可为 NULL。
 * 模板没有 %ROM%/{file} 时返回 RH_ERR_CONFIG，不猜测追加参数。
 */
RhErr rh_emu_command_build(RhCtx *ctx, RhEmuRules *rules, const char *platform,
                           const char *rom_path, const char *game_dir,
                           gchar ***argv_out, gchar **command_out);

/*
 * 选择 ROM（多碟优先 .m3u，否则第一个存在的 ROM），展开并 spawn。
 * *rom_out 返回实际使用的路径（可为 NULL）。spawn 只负责启动，不等退出。
 */
RhErr rh_emu_launch(RhCtx *ctx, const RhGameDetail *d, gchar **rom_out);

/* 从详情页取出要启动的 ROM；没有可用 ROM 返回 NULL。调用方 g_free。 */
gchar *rh_emu_pick_rom(const RhGameDetail *d);
