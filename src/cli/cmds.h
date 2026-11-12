/*
 * cmds.h — CLI 子命令（M2 引入：init / import / dat）
 */
#pragma once

int cmd_init(int argc, char **argv);
int cmd_import(int argc, char **argv);
int cmd_dat(int argc, char **argv);
int cmd_list(int argc, char **argv);
int cmd_search(int argc, char **argv);
int cmd_show(int argc, char **argv);
int cmd_edit(int argc, char **argv);
int cmd_delete(int argc, char **argv);
int cmd_export(int argc, char **argv);
int cmd_stats(int argc, char **argv);
