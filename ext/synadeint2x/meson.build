# Dependency

# Sources used to compile this plug-in
srcs = files('gstsynadeint2x.c')

shared_library('gstsynadeint2x',
  srcs,
  c_args : common_args,
  cpp_args: [common_args, '-g3'],
  dependencies: [plugin_deps],
  install: true,
  install_dir: plugins_install_dir,
  name_prefix: 'lib',
)

