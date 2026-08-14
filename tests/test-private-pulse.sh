#!/usr/bin/env bash

set -euo pipefail

test_binary=$1
pulse_binary=$2
if [[ -n ${PULSEAUDIO_LIBRARY_PATH:-} ]]; then
  export LD_LIBRARY_PATH="${PULSEAUDIO_LIBRARY_PATH}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
fi
runtime_dir=$(mktemp -d "${TMPDIR:-/tmp}/call-ducker-pulse-XXXXXX")
socket_path=${runtime_dir}/native
ready_file=${runtime_dir}/ready
server_args=(
  --daemonize=no
  --use-pid-file=no
  --exit-idle-time=-1
  --log-target=stderr
  -n
  -L "module-native-protocol-unix socket=${socket_path} auth-anonymous=1"
  -L "module-null-sink sink_name=call_ducker_test"
)
if [[ -n ${PULSEAUDIO_DL_SEARCH_PATH:-} ]]; then
  server_args+=(--dl-search-path="${PULSEAUDIO_DL_SEARCH_PATH}")
fi
if [[ $(id -u) -eq 0 ]]; then
  chmod 0777 "${runtime_dir}"
  server_args=(--system --disallow-exit "${server_args[@]}")
fi

cleanup() {
  if [[ -n ${watcher_pid:-} ]]; then
    kill "${watcher_pid}" 2>/dev/null || true
    wait "${watcher_pid}" 2>/dev/null || true
  fi
  rm -rf -- "${runtime_dir}"
}
trap cleanup EXIT

(
  pulse_pid=
  # Invoked indirectly by the EXIT trap below.
  # shellcheck disable=SC2329
  cleanup_server() {
    if [[ -n ${pulse_pid} ]]; then
      kill "${pulse_pid}" 2>/dev/null || true
      wait "${pulse_pid}" 2>/dev/null || true
    fi
  }
  trap cleanup_server EXIT
  trap 'exit 0' INT TERM

  env HOME="${runtime_dir}" XDG_RUNTIME_DIR="${runtime_dir}" \
    DBUS_SESSION_BUS_ADDRESS="${PULSEAUDIO_DBUS_ADDRESS:-unix:path=${runtime_dir}/no-dbus}" \
    "${pulse_binary}" "${server_args[@]}" &
  pulse_pid=$!
  for _ in {1..100}; do
    [[ -S ${socket_path} ]] && break
    kill -0 "${pulse_pid}" 2>/dev/null || exit 1
    sleep 0.02
  done
  [[ -S ${socket_path} ]]
  : > "${ready_file}"

  while [[ ! -f ${runtime_dir}/restart ]]; do
    kill -0 "${pulse_pid}" 2>/dev/null || exit 1
    sleep 0.02
  done
  kill "${pulse_pid}"
  wait "${pulse_pid}" 2>/dev/null || true
  pulse_pid=
  rm -f -- "${socket_path}"
  env HOME="${runtime_dir}" XDG_RUNTIME_DIR="${runtime_dir}" \
    DBUS_SESSION_BUS_ADDRESS="${PULSEAUDIO_DBUS_ADDRESS:-unix:path=${runtime_dir}/no-dbus}" \
    "${pulse_binary}" "${server_args[@]}" &
  pulse_pid=$!
  for _ in {1..100}; do
    [[ -S ${socket_path} ]] && break
    kill -0 "${pulse_pid}" 2>/dev/null || exit 1
    sleep 0.02
  done
  [[ -S ${socket_path} ]]
  : > "${runtime_dir}/restarted"
  wait "${pulse_pid}"
  pulse_pid=
) &
watcher_pid=$!

for _ in {1..100}; do
  [[ -f ${ready_file} && -S ${socket_path} ]] && break
  kill -0 "${watcher_pid}" 2>/dev/null || {
    echo "Private PulseAudio server exited before creating its socket" >&2
    exit 1
  }
  sleep 0.02
done
[[ -f ${ready_file} && -S ${socket_path} ]]

PULSE_SERVER="unix:${socket_path}" PULSE_TEST_CONTROL_DIR="${runtime_dir}" "${test_binary}"
