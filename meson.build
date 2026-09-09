project('keqingsay', 'cpp',
  version : '0.1.0',
  default_options : ['cpp_std=c++17', 'warning_level=3'])

frames_data = custom_target('frames_data',
  output : 'frames_data.hpp',
  input : files('tools/gen_frames.py'),
  command : [find_program('python3'), '@INPUT@', meson.project_source_root(), '@OUTPUT@'],
  build_by_default : true)

executable('keqingsay',
  ['src/main.cpp', frames_data],
  install : true)
