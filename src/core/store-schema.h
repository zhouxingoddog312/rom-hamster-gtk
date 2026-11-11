/*
 * store-schema.h — schema 版本常量
 *
 * 注意：DDL 文本全部放在 store.c 内部（ARCHITECTURE §1.1 硬约束 4：
 * 除 store.c 外任何源文件出现 SQL 即视为架构错误）。本头文件只声明版本号，
 * 供 CLI 打印、迁移判断使用。
 */
#pragma once

#define RH_STORE_SCHEMA_VERSION 1
