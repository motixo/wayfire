json_c = dependency('json-c')
librsvg = dependency('librsvg-2.0', required: false)
panel_decor_deps = [wlroots, pixman, wfconfig, cairo, pango, pangocairo,
    wayland_server, json_c, libinotify, plugin_pch_dep]
tabler_dir = join_paths(get_option('prefix'), get_option('datadir'), 'wayfire', 'tabler')
panel_decor_args = ['-DPANEL_DECOR_TABLER_DIR="@0@"'.format(tabler_dir)]

if librsvg.found()
  panel_decor_deps += librsvg
  panel_decor_args += '-DHAVE_LIBRSVG'
endif

shared_module('panel-decor',
    ['decoration.cpp', 'deco-subsurface.cpp', 'deco-button.cpp',
     'deco-layout.cpp', 'deco-theme.cpp'],
    include_directories: [wayfire_api_inc, wayfire_conf_inc, plugins_common_inc],
    dependencies: panel_decor_deps,
    cpp_args: panel_decor_args,
    install: true,
    install_dir: conf_data.get('PLUGIN_PATH'))

shared_module('rounded-windows', 'rounded-windows.cpp',
    include_directories: [wayfire_api_inc, wayfire_conf_inc],
    dependencies: [wlroots, pixman, wfconfig, wayland_server, glesv2, json_c,
        libinotify, plugin_pch_dep],
    install: true,
    install_dir: conf_data.get('PLUGIN_PATH'))

install_data('assets/plugin-panel-decor.svg',
    install_dir: join_paths(get_option('datadir'), 'wcm', 'icons'))
install_subdir('assets/tabler', install_dir: join_paths(get_option('datadir'), 'wayfire'))
