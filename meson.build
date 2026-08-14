project('call-ducker', ['c', 'vala'], version: '0.1.2', license: 'GPL-3.0-or-later', default_options: ['c_std=c17', 'warning_level=2', 'werror=true'])

gnome = import('gnome')
i18n = import('i18n')
prefix = get_option('prefix')
datadir = prefix / get_option('datadir')
libexecdir = prefix / get_option('libexecdir')

glib = dependency('glib-2.0', version: '>=2.74')
gio = dependency('gio-2.0', version: '>=2.74')
pulse = dependency('libpulse')
pulse_glib = dependency('libpulse-mainloop-glib')
json = dependency('json-glib-1.0')
gtk = dependency('gtk4', version: '>=4.12')
adw = dependency('libadwaita-1', version: '>=1.5')
integration_tests = get_option('integration_tests')

# Vala 0.56 emits several warning-prone C compatibility shims. Keep warnings fatal for
# handwritten/generated D-Bus C while suppressing only known Vala C-generator diagnostics.
vala_generated_c_args = [
  '-Wno-cast-function-type',
  '-Wno-discarded-qualifiers',
  '-Wno-incompatible-pointer-types',
  '-Wno-unused-function',
  '-Wno-unused-parameter',
  '-Wno-unused-variable',
  '-Wno-unused-but-set-variable',
]

subdir('data')
subdir('src')
subdir('tests')
subdir('po')

gnome.post_install(
  glib_compile_schemas: true,
  gtk_update_icon_cache: true,
  update_desktop_database: true,
)

application_model_test = executable('test-application-model',
  ['tests/test-application-model.vala', 'src/application-model.vala',
   'src/daemon-client.vala', 'src/settings-model.vala'], dependencies: [glib, gio],
  c_args: vala_generated_c_args)
test('application-model', application_model_test)

status_view_state_test = executable('test-status-view-state',
  ['tests/test-status-view-state.vala', 'src/status-view-state.vala'], dependencies: [glib, gio],
  c_args: vala_generated_c_args + ['-DGETTEXT_PACKAGE="call-ducker"'])
test('status-view-state', status_view_state_test)

daemon_client_test = executable('test-daemon-client',
  ['tests/test-daemon-client.vala', 'src/daemon-client.vala'], dependencies: gio,
  c_args: vala_generated_c_args)
test('daemon-client', daemon_client_test, args: client_helper)

ui_test = executable('test-ui',
  ['tests/test-ui.vala', 'src/application.vala', 'src/call-ducker-window.vala',
   'src/call-ducker-main-page.vala', 'src/call-ducker-applications-page.vala',
   'src/application-model.vala', 'src/applications-view.vala', 'src/status-view-state.vala',
   'src/daemon-client.vala',
   'src/service-manager.vala', 'src/settings-model.vala', version_source,
   application_resources], dependencies: [gtk, adw, gio],
  c_args: vala_generated_c_args + ['-DGETTEXT_PACKAGE="call-ducker"'])
xvfb_run = find_program('xvfb-run', required: integration_tests)
dbus_run_session = find_program('dbus-run-session', required: integration_tests)
if xvfb_run.found() and dbus_run_session.found()
  test('ui-templates', dbus_run_session,
    args: ['--', xvfb_run.full_path(), '-a', ui_test.full_path()],
    env: ['GSETTINGS_SCHEMA_DIR=' + meson.project_build_root() / 'data',
          'GSETTINGS_BACKEND=memory',
          'XDG_CONFIG_HOME=' + meson.project_build_root() / 'test-config',
          'GTK_A11Y=none',
          'GDK_DEBUG=no-portals',
          'GSK_RENDERER=cairo',
          'GDK_DISABLE=gl,vulkan'])
endif
