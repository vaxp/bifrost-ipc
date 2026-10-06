project('launcher', 'c',
  version : '0.1.0',
  license : 'AGPL-3.0-or-later',
  default_options : [
    'c_std=c11',
    'warning_level=2',
    'optimization=2',
  ]
)

# ── Dependencies ─────────────────────────────────────────────────────────────
gtk3_dep            = dependency('gtk+-3.0', version : '>= 3.22')
gtk_layer_shell_dep = dependency('gtk-layer-shell-0', version : '>= 0.8.0', required : false)

# ── pkg data dir define ──────────────────────────────────────────────────────
pkg_datadir = join_paths(get_option('prefix'), get_option('datadir'), meson.project_name())
add_project_arguments('-DPKGDATADIR="' + pkg_datadir + '"', language : 'c')

cc = meson.get_compiler('c')

bifrost_dep = cc.find_library('bifrost', required: true)

# ── Compiler flags ───────────────────────────────────────────────────────────
c_args = [
  '-Wall',
  '-Wextra',
  '-Wno-unused-parameter',
  '-march=native',
]

# ── Daemon Executable ────────────────────────────────────────────────────────
daemon_sources = files(
  'src/daemon/main.c',
  'src/core/app_entry.c',
  'src/core/desktop_reader.c',
  'src/core/icon_loader.c',
  'src/core/app_manager.c',
  'src/utils/string_utils.c',
)

executable('aethershell-daemon',
  daemon_sources,
  dependencies : [gtk3_dep, bifrost_dep],
  c_args       : c_args,
  link_args    : ['-lm', '-lpthread', '-lrt'],
  install      : true,
)

# ── Launcher Executable ──────────────────────────────────────────────────────
launcher_sources = files(
  'src/main.c',
  'src/core/app_entry.c',
  'src/utils/string_utils.c',
  'src/ui/client_ipc.c',
  'src/ui/app_icon.c',
  'src/ui/search_bar.c',
  'src/ui/app_grid.c',
  'src/ui/launcher_window.c',
  'src/ui/uninstall_dialog.c',
)

launcher_deps = [gtk3_dep, bifrost_dep]
launcher_c_args = c_args

if gtk_layer_shell_dep.found()
  launcher_deps += [gtk_layer_shell_dep]
  launcher_c_args += ['-DHAVE_GTK_LAYER_SHELL']
endif

executable('launcher',
  launcher_sources,
  dependencies : launcher_deps,
  c_args       : launcher_c_args,
  link_args    : ['-lm', '-lpthread', '-lrt'],
  install      : true,
)

# ── CLI Launcher Executable ──────────────────────────────────────────────────
cli_sources = files(
  'src/cli/main.c',
)

executable('cli-launcher',
  cli_sources,
  dependencies : [bifrost_dep],
  c_args       : c_args,
  link_args    : ['-lpthread', '-lrt'],
  install      : true,
)

# ── TUI Launcher Executable ──────────────────────────────────────────────────
ncurses_dep = dependency('ncurses', required: false)
if not ncurses_dep.found()
  ncurses_dep = dependency('ncursesw', required: false)
endif
if not ncurses_dep.found()
  ncurses_dep = cc.find_library('ncurses', required: false)
endif
if not ncurses_dep.found()
  ncurses_dep = cc.find_library('ncursesw', required: false)
endif

if ncurses_dep.found()
  tui_sources = files(
    'src/tui/main.c',
  )

  executable('tui-launcher',
    tui_sources,
    dependencies : [bifrost_dep, ncurses_dep],
    c_args       : c_args,
    link_args    : ['-lpthread', '-lrt'],
    install      : true,
  )
endif

# ── Data ─────────────────────────────────────────────────────────────────────
subdir('data')
