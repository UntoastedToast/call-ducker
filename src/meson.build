inc = include_directories('.')
version_data = configuration_data()
version_data.set('APP_VERSION', meson.project_version())
version_source = configure_file(input: 'version.vala.in', output: 'version.vala', configuration: version_data)
application_resources = gnome.compile_resources('call-ducker-resources',
  'call-ducker.gresource.xml', source_dir: meson.current_source_dir())

daemon_dbus = gnome.gdbus_codegen('cd-dbus',
  '../data/io.github.UntoastedToast.CallDucker.Daemon1.xml',
  interface_prefix: 'io.github.UntoastedToast.CallDucker.',
  namespace: 'Cd')

core_lib = static_library('call-ducker-core', ['cd-match.c', 'cd-process.c', 'cd-engine.c', 'cd-audio-backend.c', 'cd-state-store.c', 'cd-volume-controller.c', 'cd-application-registry.c'], dependencies: [glib, gio, json, pulse], include_directories: inc)
core_dep = declare_dependency(link_with: core_lib, dependencies: [glib, gio, json, pulse], include_directories: inc)

daemon_executable = executable('call-ducker-daemon', ['daemon.c', 'cd-daemon.c', 'pulse-backend.c', 'cd-dbus-service.c', daemon_dbus], dependencies: [core_dep, pulse_glib], install: true, install_dir: libexecdir)
shared_vala = ['daemon-client.vala', 'service-manager.vala', 'settings-model.vala']
executable('call-duckerctl', ['call-duckerctl.vala', shared_vala], dependencies: [gio, json],
  c_args: vala_generated_c_args, install: true)
executable('call-ducker', ['main.vala', 'application.vala', 'call-ducker-window.vala',
  'call-ducker-main-page.vala', 'call-ducker-applications-page.vala',
  'application-model.vala', 'applications-view.vala', 'status-view-state.vala', shared_vala, version_source,
  application_resources], dependencies: [gtk, adw, gio],
  c_args: vala_generated_c_args + ['-DGETTEXT_PACKAGE="call-ducker"'], install: true)
