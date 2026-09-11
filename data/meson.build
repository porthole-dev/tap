desktop_file = i18n.merge_file(
        input: app_id + '.desktop.in',
       output: app_id + '.desktop',
         type: 'desktop',
       po_dir: '../po',
      install: true,
  install_dir: get_option('datadir') / 'applications',
)

desktop_file_validate = find_program('desktop-file-validate', required: false)
if desktop_file_validate.found()
  test('validate-desktop', desktop_file_validate,
       args: [desktop_file], depends: [desktop_file])
endif

metainfo_file = i18n.merge_file(
        input: app_id + '.metainfo.xml.in',
       output: app_id + '.metainfo.xml',
       po_dir: '../po',
      install: true,
  install_dir: get_option('datadir') / 'metainfo',
)

# An app without a validating metainfo file is not a citizen of the desktop:
# no software centre lists it, nothing knows its name but the .desktop.
appstreamcli = find_program('appstreamcli', required: false)
if appstreamcli.found()
  test('validate-metainfo', appstreamcli,
       args: ['validate', '--no-net', '--explain', metainfo_file],
       depends: [metainfo_file])
endif

install_data('icons/hicolor/scalable/apps' / app_id + '.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')
install_data('icons/hicolor/symbolic/apps' / app_id + '-symbolic.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/symbolic/apps')
