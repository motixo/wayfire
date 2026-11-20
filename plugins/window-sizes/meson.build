shared_module('window-sizes', 'window-sizes.cpp',
    include_directories: [wayfire_api_inc, wayfire_conf_inc],
    dependencies: [wlroots, pixman, wfconfig, wayland_server, dependency('json-c'),
        plugin_pch_dep],
    install: true,
    install_dir: conf_data.get('PLUGIN_PATH'))
