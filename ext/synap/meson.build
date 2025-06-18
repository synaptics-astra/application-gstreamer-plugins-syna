# Dependency
synap_dep = dependency('synap-framework')
cairo_dep = dependency('cairo-gobject', version : '>=1.10.0')
opencv_dep = dependency('opencv4', version : '>=4.0.0')

# Sources used to compile this plug-in
synap_sources = [
  'gstsynapplugin.cpp',
  'gstsynapinfer.cpp',
  'gstsynapoverlay.cpp',
  'gstsynapoverlayrender.cpp',
  'gstsynapimageproc.cpp',
  'gstsynapsink.cpp',
  'synapmeta.cpp',
]

synap = shared_library('gstsynap',
  synap_sources,
  c_args : common_args,
  cpp_args: [common_args],
  include_directories: [configinc],
  dependencies : [plugin_deps,synap_dep,cairo_dep,opencv_dep],
  gnu_symbol_visibility : 'hidden',
  install : true,
  install_dir : plugins_install_dir,
)

