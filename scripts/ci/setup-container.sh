#!/usr/bin/env bash

# Installs the toolchain a CI container needs and performs the container-only
# preparation the test suite depends on. This is the single source of truth for
# every distribution package list; the workflows must not inline their own.
#
# Usage: scripts/ci/setup-container.sh <fedora|debian|arch> <profile>
#
# Profiles:
#   build     compile and run the test suite
#   quality   build plus clang-format, ShellCheck, and the metadata validators
#   sanitize  build plus the ASan/UBSan runtimes
#   source    build the source distribution, which runs the quality gate
#   package   build a distribution package from a source archive

set -euo pipefail

usage() {
  echo "Usage: $0 <fedora|debian|arch> <build|quality|sanitize|source|package>" >&2
}

if [[ $# -ne 2 ]]; then
  usage
  exit 2
fi

family=$1
profile=$2

case ${family} in
  fedora | debian | arch) ;;
  *)
    usage
    exit 2
    ;;
esac

case ${profile} in
  build | quality | sanitize | source | package) ;;
  *)
    usage
    exit 2
    ;;
esac

# Weak dependencies and apt recommends stay enabled on purpose: the test suite
# relies on transitively pulled tools such as dbus-run-session.
build_packages() {
  case ${family} in
    fedora)
      printf '%s\n' git gcc gettext meson ninja-build vala glib2-devel \
        pulseaudio-libs-devel json-glib-devel gtk4-devel libadwaita-devel \
        desktop-file-utils appstream dbus-daemon dbus-tools dbus-x11 \
        pulseaudio xorg-x11-server-Xvfb
      ;;
    debian)
      printf '%s\n' git build-essential gettext meson ninja-build valac \
        libglib2.0-dev libpulse-dev libjson-glib-dev libgtk-4-dev \
        libadwaita-1-dev desktop-file-utils appstream dbus-daemon pulseaudio \
        xvfb xauth
      ;;
    arch)
      printf '%s\n' git gcc gettext meson ninja vala glib2 glib2-devel \
        libpulse json-glib gtk4 libadwaita desktop-file-utils appstream dbus \
        pulseaudio shadow xorg-server-xvfb
      ;;
  esac
}

quality_packages() {
  case ${family} in
    fedora) printf '%s\n' clang-tools-extra ShellCheck ;;
    debian) printf '%s\n' clang-format shellcheck ;;
    arch) printf '%s\n' clang shellcheck ;;
  esac
}

sanitize_packages() {
  case ${family} in
    fedora) printf '%s\n' libasan libubsan ;;
    *) ;;
  esac
}

package_packages() {
  case ${family} in
    fedora) printf '%s\n' rpm-build rpmlint systemd-rpm-macros ;;
    debian) printf '%s\n' debhelper devscripts dpkg-dev fakeroot lintian xz-utils ;;
    arch) printf '%s\n' fakeroot sudo namcap pacman-contrib ;;
  esac
}

selected_packages() {
  build_packages
  case ${profile} in
    quality) quality_packages ;;
    sanitize) sanitize_packages ;;
    source)
      quality_packages
      package_packages
      ;;
    package) package_packages ;;
  esac
}

mapfile -t packages < <(selected_packages)

case ${family} in
  fedora)
    dnf install -y "${packages[@]}"
    ;;
  debian)
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y "${packages[@]}"
    ;;
  arch)
    pacman -Syu --noconfirm "${packages[@]}"
    # PulseAudio refuses to start without its system user, which the Arch
    # container image does not create.
    if ! getent passwd pulse > /dev/null; then
      useradd --system --user-group --no-create-home pulse
    fi
    ;;
esac

# dbus-run-session, used by the ui-templates test, needs a machine ID.
dbus-uuidgen --ensure=/etc/machine-id

# makepkg refuses to run as root, so the Arch package job needs an unprivileged
# builder account.
if [[ ${family} == arch && ${profile} == package ]]; then
  if ! getent passwd builder > /dev/null; then
    useradd --create-home builder
  fi
fi
