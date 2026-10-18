lipc = dependency('lipc')

executable(
  'notebooks_helper',
  'notebooks_helper.c',
  'nb_core.c',
  c_args: ['-D_DEFAULT_SOURCE'],
  dependencies: lipc,
  install: false,
)
