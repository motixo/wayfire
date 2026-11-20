shared_module('dock-snapshot', 'dock-snapshot.cpp',
    include_directories: [wayfire_api_inc, wayfire_conf_inc, plugins_common_inc, ipc_include_dirs],
    dependencies: [wlroots, pixman, wfconfig, wayland_server, drm, json,
        dependency('threads'), plugin_pch_dep],
    install: true,
    install_dir: conf_data.get('PLUGIN_PATH'))
