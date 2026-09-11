project('tap', 'c',
          version: '0.1.0',
          license: 'GPL-3.0-or-later',
    meson_version: '>= 1.0.0',
  default_options: ['warning_level=2', 'c_std=gnu11'],
)

i18n  = import('i18n')
gnome = import('gnome')

# One app id, used for the binary's application id, the .desktop, the
# metainfo, the icons, the GResource prefix and the Flatpak id. Everything
# that has to agree derives from this line.
app_id = 'io.github.jertlok.Tap'

config_h = configuration_data()
config_h.set_quoted('APP_ID', app_id)
config_h.set_quoted('APP_VERSION', meson.project_version())
config_h.set_quoted('GETTEXT_PACKAGE', 'tap')
config_h.set_quoted('LOCALEDIR', get_option('prefix') / get_option('localedir'))
configure_file(output: 'config.h', configuration: config_h)

# -Wunused-parameter is the only thing warning_level=2 finds in a GObject
# codebase, where a signal handler takes what the signal gives it.
add_project_arguments(
  ['-I' + meson.project_build_root(), '-Wno-unused-parameter'],
  language: 'c',
)

subdir('data')
subdir('src')
subdir('po')

gnome.post_install(
  gtk_update_icon_cache: true,
  update_desktop_database: true,
)
