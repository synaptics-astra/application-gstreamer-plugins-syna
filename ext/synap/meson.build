# Dependency
synap_dep = dependency('synap-framework')

# Sources used to compile this plug-in
synap_sources = [
  'gstsynap.cpp',
]

synap = shared_library('gstsynap',
  synap_sources,
  c_args : common_args,
  cpp_args: common_args,
  include_directories: [configinc],
  dependencies : [plugin_deps,synap_dep],
  gnu_symbol_visibility : 'hidden',
  install : true,
  install_dir : plugins_install_dir,
)

