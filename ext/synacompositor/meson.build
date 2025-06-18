# Dependency
# Add SynaCompositor Library
synacomp_dep = dependency('syna-compositor')
egl_dep = dependency('egl', required: true)
gles2_dep = dependency('glesv2', required: true)
drm_dep = dependency('libdrm', required: true)
gbm_dep = dependency('gbm', required: true)
gstvideo = dependency('gstreamer-video-1.0')
gstallocators = dependency('gstreamer-allocators-1.0')
gstgl_dep = dependency('gstreamer-gl-1.0', required: true)

# Sources used to compile this plug-in
synacompositor_sources = [
  'gstsynacompositor.cpp'
]

synap = shared_library('gstsynacompositor',
  synacompositor_sources,
  c_args : common_args,
  cpp_args: [common_args, '-g3'],
  include_directories: [configinc],
  dependencies : [plugin_deps, opencv_dep, egl_dep, gles2_dep, drm_dep, gbm_dep, synacomp_dep, gstvideo, gstallocators, gstgl_dep],
  gnu_symbol_visibility : 'hidden',
  install : true,
  install_dir : plugins_install_dir,
)

