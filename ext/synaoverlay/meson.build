# Dependency
opencv_dep = dependency('opencv4', version : '>=4.0.0')

# Sources used to compile this plug-in
synaoverlay_sources = [
  'gstsynaoverlay.cpp',
]

synaoverlay = shared_library('gstsynaoverlay',
  synaoverlay_sources,
  c_args : common_args,
  cpp_args: [common_args],
  include_directories: [configinc],
  dependencies : [plugin_deps, opencv_dep],
  gnu_symbol_visibility : 'hidden',
  install : true,
  install_dir : plugins_install_dir,
)

