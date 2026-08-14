match_test = executable('test-match', 'test-match.c', dependencies: core_dep)
engine_test = executable('test-engine', 'test-engine.c', dependencies: core_dep)
volume_test = executable('test-volume-controller',
  ['test-volume-controller.c', 'fake-audio-backend.c'], dependencies: core_dep)
state_store_test = executable('test-state-store', 'test-state-store.c', dependencies: core_dep)
contracts_test = executable('test-contracts', 'test-contracts.c', dependencies: core_dep,
  c_args: ['-DSOURCE_ROOT="' + meson.project_source_root() + '"'])
registry_test = executable('test-application-registry',
  ['test-application-registry.c', 'fake-audio-backend.c'], dependencies: core_dep)
daemon_dbus_test = executable('test-daemon-dbus',
  ['test-daemon-dbus.c', 'fake-audio-backend.c', '../src/cd-daemon.c',
   '../src/cd-dbus-service.c', daemon_dbus],
  dependencies: core_dep)
pulse_backend_test = executable('test-pulse-backend',
  ['test-pulse-backend.c', '../src/pulse-backend.c'],
  dependencies: [core_dep, pulse_glib])
client_helper = executable('client-skeleton-helper',
  ['client-skeleton-helper.c', '../src/cd-dbus-service.c', daemon_dbus],
  dependencies: gio, include_directories: inc)
test('matching', match_test)
test('engine', engine_test)
test('volume-controller', volume_test)
test('state-store', state_store_test)
test('contracts', contracts_test)
test('application-registry', registry_test,
  env: ['GSETTINGS_SCHEMA_DIR=' + meson.project_build_root() / 'data',
        'GSETTINGS_BACKEND=memory'])
test('daemon-dbus', daemon_dbus_test,
  env: ['GSETTINGS_SCHEMA_DIR=' + meson.project_build_root() / 'data',
        'GSETTINGS_BACKEND=memory'])
pulseaudio_program = find_program('pulseaudio', required: integration_tests)
if pulseaudio_program.found()
  private_pulse = find_program('test-private-pulse.sh')
  test('pulse-backend', private_pulse,
    args: [pulse_backend_test, pulseaudio_program.full_path()], timeout: 30)
endif
