project('rom-hamster', 'c',
  version : '0.1.0',
  license : 'GPL-3.0-or-later',
  meson_version : '>=0.61',
  default_options : ['c_std=c11', 'warning_level=2', 'werror=false'])

# ---------------------------------------------------------------------------
# 依赖：核心库只允许这五个 .so（见 ARCHITECTURE §8.2）
# ---------------------------------------------------------------------------
glib_dep      = dependency('glib-2.0',      version : '>=2.66')
gobject_dep   = dependency('gobject-2.0',   version : '>=2.66')
sqlite_dep    = dependency('sqlite3',       version : '>=3.35')
archive_dep   = dependency('libarchive',    version : '>=3.4')
xml_dep       = dependency('libxml-2.0',    version : '>=2.9')
zlib_dep      = dependency('zlib')

core_deps = [glib_dep, gobject_dep, sqlite_dep, archive_dep, xml_dep, zlib_dep]

# ---------------------------------------------------------------------------
# 版本常量：核心只提供常量，打印由 CLI 层负责（ARCHITECTURE §1.1 硬约束 2）
# ---------------------------------------------------------------------------
ver_conf = configuration_data()
ver_conf.set('RH_VERSION', meson.project_version())
ver_conf.set('RH_APP_NAME', 'rom-hamster')
ver_conf.set('RH_MAINTAINER', 'Zhou Xin')
ver_conf.set('RH_MAINTAINER_EMAIL', 'zhouxin312@qq.com')
ver_conf.set('RH_HOMEPAGE', 'https://gitee.com/goddog312/rom-hamster-gtk')
ver_conf.set('RH_LICENSE', meson.project_license()[0])
ver_conf.set('RH_SCHEMA_VERSION', 1)

configure_file(input : 'src/core/rh-version.h.in',
  output : 'rh-version.h',
  configuration : ver_conf)

gen_inc = include_directories('.')

core_inc = include_directories('src/core')

core_sources = files(
  'src/core/rh-err.c',
  'src/core/rh-config.c',
  'src/core/rh-ctx.c',
  'src/core/rh-records.c',
  'src/core/platform.c',
  'src/core/store.c',
)

# 核心库：名字带 -1.0（沿用 1.0.0 的产物命名习惯），不链 GTK
#
# 装在 libdir/rom-hamster 这个私有目录里（ARCHITECTURE §9.1），因此两个前端
# 都要带 install_rpath 才能找到它；不污染发行版的公共库命名空间。
rh_libdir = join_paths(get_option('libdir'), 'rom-hamster')
rh_rpath  = join_paths(get_option('prefix'), rh_libdir)

libromham = shared_library('romham-1.0', core_sources,
  include_directories : [core_inc, gen_inc],
  dependencies : core_deps,
  install : true,
  install_dir : rh_libdir)

libromham_dep = declare_dependency(
  link_with : libromham,
  include_directories : [core_inc, gen_inc],
  dependencies : core_deps)

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
executable('rom-hamster',
  files('src/cli/main.c', 'src/cli/ui-cli.c'),
  dependencies : libromham_dep,
  install : true,
  install_rpath : rh_rpath)

# ---------------------------------------------------------------------------
# GUI：只在 -Dgui=true 时求值 gtk4，-Dgui=false 的机器不需要装 GTK
# ---------------------------------------------------------------------------
if get_option('gui')
  gtk_dep = dependency('gtk4', version : '>=4.10')
  executable('rom-hamster-gui',
    files('src/gui/main.c'),
    dependencies : [libromham_dep, gtk_dep],
    install : true,
    install_rpath : rh_rpath)
endif

# ---------------------------------------------------------------------------
# 数据文件（M1 起由 rom-hamster-data 包分发）
# ---------------------------------------------------------------------------
datadir_rh = join_paths(get_option('datadir'), 'rom-hamster')

install_data('data/platforms.conf', install_dir : datadir_rh)
install_data('data/rom-hamster.conf.template', install_dir : datadir_rh)
install_subdir('data/frontends', install_dir : datadir_rh)

# 系统层配置（/etc/rom-hamster，conffile）与 man page（ARCHITECTURE §9.1）
install_subdir('etc/rom-hamster', install_dir : get_option('sysconfdir'))
install_man('man/rom-hamster.1')
if get_option('gui')
  install_man('man/rom-hamster-gui.1')
endif

dirs_conf = configuration_data()
dirs_conf.set('DATADIR', join_paths(get_option('prefix'), datadir_rh))
dirs_conf.set('SYSCONFDIR', join_paths(get_option('sysconfdir'), 'rom-hamster'))
configure_file(input : 'src/core/rh-dirs.h.in',
  output : 'rh-dirs.h',
  configuration : dirs_conf)

# ---------------------------------------------------------------------------
# 测试
# ---------------------------------------------------------------------------
if get_option('tests')
  test_config = executable('test-config',
    files('tests/unit/test-config.c'),
    dependencies : [libromham_dep, dependency('gio-2.0')])
  test('config-layers', test_config)

  test_store = executable('test-store',
    files('tests/unit/test-store.c'),
    dependencies : libromham_dep)
  test('store-schema', test_store)

  test_platform = executable('test-platform',
    files('tests/unit/test-platform.c'),
    dependencies : libromham_dep)
  test('platform-rules', test_platform,
    args : [join_paths(meson.current_source_dir(), 'data', 'platforms.conf')])

  test_store_crud = executable('test-store-crud',
    files('tests/unit/test-store-crud.c'),
    dependencies : libromham_dep)
  test('store-crud', test_store_crud)
endif
