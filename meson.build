project('drm_playground', 'c',
  version: '0.0.0',
  license: 'MIT',
  default_options: [
    'c_std=c11',
    'warning_level=2',
  ],
)

add_project_arguments('-D_POSIX_C_SOURCE=200809L', language: 'c')

executable('drm_playground', 'main.c', 'drm.c', 'util.c',
  dependencies: [
    dependency('libdrm'),
    dependency('gbm'),
  ],
  install: true,
)
