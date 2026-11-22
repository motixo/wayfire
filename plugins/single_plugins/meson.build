plugins = [
  'move', 'resize', 'command', 'autostart', 'vswipe', 'wrot', 'expo',
  'switcher', 'fast-switcher', 'oswitch', 'place', 'invert',
  'zoom', 'alpha', 'idle', 'extra-gestures', 'preserve-output',
  'wsets', 'xkb-bindings',
]

all_include_dirs = [wayfire_api_inc, wayfire_conf_inc, plugins_common_inc, vswitch_inc, wobbly_inc, grid_inc, ipc_include_dirs]
if wlroots_features['vulkan_renderer']
  plugins += 'vk-color-management'
endif

all_deps = [wlroots, pixman, wfconfig, wftouch, cairo, pango, pangocairo, json, plugin_pch_dep]
switcher_gtk = dependency('gtk+-3.0', required: false)
switcher_gio = dependency('gio-unix-2.0', required: false)

extra_libs = {
    'move': [move_drag_interface],
    'expo': [workspace_wall, move_drag_interface],
    'vswipe': [workspace_wall],
}

foreach plugin : plugins
  deps = all_deps
  args = []
  if plugin == 'switcher' and switcher_gtk.found() and switcher_gio.found()
    deps += [switcher_gtk, switcher_gio]
    args += '-DWF_SWITCHER_ICONS'
  endif
  plugin_module = shared_module(plugin, plugin + '.cpp',
      include_directories: all_include_dirs,
      dependencies: deps,
      cpp_args: args,
      link_with: extra_libs.get(plugin, []),
      install: true,
      install_dir: conf_data.get('PLUGIN_PATH'))

  if plugin == 'command'
    command_plugin = plugin_module
  endif
  if plugin == 'resize'
    resize_plugin = plugin_module
  endif
endforeach
