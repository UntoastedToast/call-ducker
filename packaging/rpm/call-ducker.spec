Name:           call-ducker
Version:        0.1.1
Release:        1%{?dist}
Summary:        Adjust game audio automatically during calls
License:        GPL-3.0-or-later
URL:            https://github.com/UntoastedToast/call-ducker
Source0:        %{url}/releases/download/v%{version}/%{name}-%{version}.tar.xz
BuildRequires:  gcc meson ninja-build vala glib2-devel pulseaudio-libs-devel pulseaudio json-glib-devel gtk4-devel libadwaita-devel
BuildRequires:  desktop-file-utils appstream dbus-daemon dbus-x11 xorg-x11-server-Xvfb
BuildRequires:  systemd-rpm-macros
Requires:       pulseaudio-daemon

%description
CallDucker is a native Fedora/GNOME utility that sets games to a target volume
while a selected voice-call application is recording. It restores the original
per-channel volumes afterward.

%prep
%autosetup

%build
%meson -Dintegration_tests=enabled
%meson_build

%install
%meson_install

%check
%meson_test
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.UntoastedToast.CallDucker.desktop
appstreamcli validate --no-net %{buildroot}%{_metainfodir}/io.github.UntoastedToast.CallDucker.metainfo.xml

%post
%systemd_user_post call-ducker.service

%preun
%systemd_user_preun call-ducker.service

%postun
%systemd_user_postun_with_restart call-ducker.service

%files
%license LICENSE
%doc README.md docs/ARCHITECTURE.md .github/ISSUE_TEMPLATE/release.md
%{_bindir}/call-ducker
%{_bindir}/call-duckerctl
%{_libexecdir}/call-ducker-daemon
%{_userunitdir}/call-ducker.service
%{_datadir}/applications/io.github.UntoastedToast.CallDucker.desktop
%{_metainfodir}/io.github.UntoastedToast.CallDucker.metainfo.xml
%{_datadir}/icons/hicolor/*/apps/io.github.UntoastedToast.CallDucker*.svg
%{_datadir}/glib-2.0/schemas/io.github.UntoastedToast.CallDucker.gschema.xml
%{_datadir}/dbus-1/interfaces/io.github.UntoastedToast.CallDucker.Daemon1.xml
%{_mandir}/man1/call-ducker.1*
%{_mandir}/man1/call-duckerctl.1*

%changelog
* Fri Aug 14 2026 UntoastedToast <45534729+UntoastedToast@users.noreply.github.com> - 0.1.1-1
- First release built for Debian, Ubuntu, and Arch alongside Fedora

* Fri Aug 14 2026 UntoastedToast <45534729+UntoastedToast@users.noreply.github.com> - 0.1.0-1
- Initial release
