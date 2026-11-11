/*
 * rh-err.h — 错误码与错误对象
 *
 * 核心库不打印任何文字（ARCHITECTURE §1.1 硬约束 2）：这里只返回错误码和
 * 消息字符串，怎么显示由 CLI / GUI 决定。
 */
#pragma once

#include <glib.h>

typedef enum {
  RH_OK = 0,
  RH_ERR_IO,          /* 读写文件失败 */
  RH_ERR_PARSE,       /* 配置文件 / DAT / 清单解析失败 */
  RH_ERR_DB,          /* SQLite 返回错误 */
  RH_ERR_DB_BUSY,     /* 另一实例正在写，超时退出 */
  RH_ERR_SCHEMA,      /* schema 版本不兼容 */
  RH_ERR_CONFIG,      /* 配置项缺失或非法 */
  RH_ERR_NOT_FOUND,
  RH_ERR_EXISTS,      /* UNIQUE 冲突 */
  RH_ERR_CONFLICT,
  RH_ERR_UNSUPPORTED,
  RH_ERR_CANCELLED,
  RH_ERR_INTERNAL,
} RhErrCode;

#define RH_ERR_MSG_MAX 512

typedef struct {
  RhErrCode code;
  char      msg[RH_ERR_MSG_MAX];
} RhErr;

#define RH_ERR_INIT { RH_OK, { 0 } }

/* 成功 */
RhErr rh_ok(void);

/* 失败：code + printf 风格消息 */
RhErr rh_fail(RhErrCode code, const char *fmt, ...) G_GNUC_PRINTF(2, 3);

/* 只有错误码，消息用默认文案 */
RhErr rh_err_from(RhErrCode code);

gboolean    rh_err_ok(const RhErr *e);
const char *rh_err_code_name(RhErrCode code);

/* 供上层判断"能不能重试" */
gboolean rh_err_is_transient(RhErrCode code);

/* 就地写入错误（M2 引入：避免"丢弃返回值"导致错误被无声吞掉）。
 * e 可为 NULL（只丢弃）。rh_err_set_errno 会在末尾追加 strerror(errno)。 */
void rh_err_clear(RhErr *e);
void rh_err_set(RhErr *e, RhErrCode code, const char *fmt, ...) G_GNUC_PRINTF(3, 4);
void rh_err_set_errno(RhErr *e, RhErrCode code, const char *fmt, ...) G_GNUC_PRINTF(3, 4);
