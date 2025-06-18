# Dependency
opencv_dep = dependency('opencv4', version : '>=4.0.0')

# Sources used to compile this plug-in
synavideoconvertscale_sources = [
  'gstsynavideoconvertscale.cpp',
]

synap = shared_library('gstsynavideoconvertscale',
  synavideoconvertscale_sources,
  c_args : common_args,
  cpp_args: [common_args],
  include_directories: [configinc],
  dependencies : [plugin_deps, opencv_dep],
  gnu_symbol_visibility : 'hidden',
  install : true,
  install_dir : plugins_install_dir,
)

