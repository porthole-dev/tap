blueprints = custom_target('blueprints',
        input: files('tap-window.blp'),
       output: '.',
      command: [find_program('blueprint-compiler'), 'batch-compile',
                '@OUTPUT@', '@CURRENT_SOURCE_DIR@', '@INPUT@'],
)

# The metainfo is generated into the build tree by data/meson.build, so the
# resource compiler has to be told to look there as well as here.
resources = gnome.compile_resources('tap-resources',
  'tap.gresource.xml',
       c_name: 'tap',
  dependencies: [blueprints, metainfo_file],
   source_dir: ['.', meson.project_build_root() / 'data'],
)

executable('tap',
  ['main.c', 'tap-nfc.c', 'tap-window.c', resources],
  dependencies: [
    dependency('gtk4', version: '>= 4.14'),
    dependency('libadwaita-1', version: '>= 1.6'),
    dependency('gio-unix-2.0', version: '>= 2.76'),
  ],
  install: true,
)
