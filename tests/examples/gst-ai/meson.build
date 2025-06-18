gst_json_dep = dependency('json-glib-1.0')

# Sources used to compile this plug-in
gst_ai_sources = [
  'main.cpp',
  'gst_ai_ic.cpp',
]

executable('gst-ai',
  gst_ai_sources,
  c_args : common_args,
  cpp_args: [common_args,'-fpermissive'],
  include_directories: [configinc],
  dependencies : [plugin_deps,gst_json_dep],
  install : true,
  install_tag : 'bin',
)
