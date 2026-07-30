Name:           tryx-panorama-manager
Version:        2.0.1
Release:        1%{?dist}
Summary:        Linux manager for supported TRYX Panorama cooler displays

License:        MIT AND BSD-2-Clause
URL:            https://github.com/DXVSI/Tryx-Linux-GUI
Source0:        %{url}/releases/download/v%{version}/%{name}-%{version}.tar.xz

# The first native package release is intentionally limited to the architecture
# covered by the package CI and hardware release gate.
ExclusiveArch:  x86_64

BuildRequires:  gcc-c++
BuildRequires:  make
BuildRequires:  dbus-daemon
BuildRequires:  ffmpeg-free
BuildRequires:  qt6-rpm-macros
BuildRequires:  qt6-linguist
BuildRequires:  qt6-qtdeclarative-devel
BuildRequires:  pkgconfig(Qt6Core)
BuildRequires:  pkgconfig(Qt6DBus)
BuildRequires:  pkgconfig(Qt6Gui)
BuildRequires:  pkgconfig(Qt6Qml)
BuildRequires:  pkgconfig(Qt6Quick)
BuildRequires:  pkgconfig(Qt6QuickControls2)
BuildRequires:  protobuf-compiler
BuildRequires:  pkgconfig(protobuf)
BuildRequires:  pkgconfig(libudev)
BuildRequires:  pkgconfig(libusb-1.0)
BuildRequires:  pkgconfig(systemd)
BuildRequires:  systemd-rpm-macros
BuildRequires:  systemd-udev
BuildRequires:  desktop-file-utils
BuildRequires:  appstream

# Shared-library dependencies are generated automatically from the installed
# ELF binary. These explicit dependencies describe non-ELF runtime contracts.
Requires:       dbus
Requires:       systemd
Requires:       systemd-udev
Requires:       hicolor-icon-theme
Requires:       qt6-qtdeclarative%{?_isa}
# Media preparation uses the libx264 encoder. Fedora's ffmpeg-free may provide
# /usr/bin/ffmpeg without that encoder, so require RPM Fusion's full package.
Requires:       ffmpeg

# Native Wayland support and the legacy/firmware discovery helpers are useful
# but are not required for the PASE printer-class control path.
Recommends:     qt6-qtwayland%{?_isa}
Suggests:       /usr/bin/adb
Suggests:       /usr/bin/unzip
Suggests:       /usr/sbin/debugfs
Suggests:       /usr/bin/glxinfo
Suggests:       /usr/bin/lspci

%{?systemd_requires}

%description
TRYX Panorama Manager is a Qt 6 application for controlling supported TRYX
Panorama and Panorama SE cooler displays on Linux. It manages custom media,
display settings, live system metrics, and devices using the PASE
printer-class USB protocol.

The package contains open-source project components and host integration. It
does not contain vendor media, firmware archives, or the proprietary Rockchip
upgrade_tool backend.

%prep
%autosetup

test "$(tr -d '\r\n' < VERSION)" = "%{version}"

%build
%qmake_qt6 tryx-panorama-all.pro
%make_build

%install
%make_install INSTALL_ROOT=%{buildroot}

%check
dbus-run-session -- %make_build package-check

packaging/scripts/verify-package-contents.sh %{buildroot}
desktop-file-validate \
    %{buildroot}%{_datadir}/applications/tryx-panorama-manager.desktop
appstreamcli validate --no-net --strict \
    %{buildroot}%{_metainfodir}/io.github.dxvsi.tryx_panorama_manager.metainfo.xml
udevadm verify --resolve-names=never \
    %{buildroot}%{_udevrulesdir}/70-tryx-pase-access.rules \
    %{buildroot}%{_udevrulesdir}/99-tryx-pase-printer.rules

# Fedora's systemd transaction file triggers reload user-unit and udev
# configuration. Do not enable or restart a user's service from a root package
# transaction: the GUI starts it on demand and upgrades may race active USB I/O.
%post
%systemd_user_post tryx-panorama.service

%preun
%systemd_user_preun tryx-panorama.service

%postun
%systemd_user_postun tryx-panorama.service

%files
%license %{_licensedir}/%{name}/LICENSE
%license %{_licensedir}/%{name}/picojson-BSD-2-Clause.txt
%doc README.md
%{_bindir}/tryx-panorama-manager
%{_prefix}/lib/tryx-panorama-manager/tryx-panorama-runtime
%{_userunitdir}/tryx-panorama.service
%{_userpresetdir}/90-tryx-panorama.preset
%{_udevrulesdir}/70-tryx-pase-access.rules
%{_udevrulesdir}/99-tryx-pase-printer.rules
%{_datadir}/applications/tryx-panorama-manager.desktop
%{_iconsdir}/hicolor/256x256/apps/tryx-panorama.png
%{_metainfodir}/io.github.dxvsi.tryx_panorama_manager.metainfo.xml
%{_mandir}/man1/tryx-panorama-manager.1*

%changelog
* Mon Jul 27 2026 DXVSI <DXVSI@users.noreply.github.com> - 2.0.1-1
- Fix checksum generation for native GitHub Release assets

* Mon Jul 27 2026 DXVSI <DXVSI@users.noreply.github.com> - 2.0.0-1
- Add the first native Fedora package for the 2.0 release line
