lipc = dependency('lipc')

executable(
  'notebooks_helper',
  'notebooks_helper.c',
  'nb_core.c',
  # Older glibc (kindlepw2) ignores _DEFAULT_SOURCE; ask for POSIX 2008 too.
  c_args: ['-D_DEFAULT_SOURCE', '-D_POSIX_C_SOURCE=200809L'],
  dependencies: lipc,
  install: false,
)
