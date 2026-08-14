conf = configuration_data()
conf.set('libexecdir', libexecdir)
service = configure_file(input: 'call-ducker.service.in', output: 'call-ducker.service', configuration: conf)
systemd_dir = get_option('systemd_user_unit_dir')
if systemd_dir == ''
  systemd_dir = prefix / 'lib/systemd/user'
endif
install_data(service, install_dir: systemd_dir)
install_data('io.github.UntoastedToast.CallDucker.desktop', install_dir: datadir / 'applications')
install_data('io.github.UntoastedToast.CallDucker.metainfo.xml', install_dir: datadir / 'metainfo')
install_data('io.github.UntoastedToast.CallDucker.svg', install_dir: datadir / 'icons/hicolor/scalable/apps')
install_data('io.github.UntoastedToast.CallDucker-symbolic.svg', install_dir: datadir / 'icons/hicolor/symbolic/apps')
install_man('call-ducker.1', 'call-duckerctl.1')
gnome.compile_schemas(build_by_default: true, depend_files: files('io.github.UntoastedToast.CallDucker.gschema.xml'))
install_data('io.github.UntoastedToast.CallDucker.gschema.xml', install_dir: datadir / 'glib-2.0/schemas')
install_data('io.github.UntoastedToast.CallDucker.Daemon1.xml', install_dir: datadir / 'dbus-1/interfaces')
