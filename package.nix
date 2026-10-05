{
  lib,
  stdenv,
  fetchFromGitHub,
  qt6,
  protobuf,
  libusb1,
  systemd,
  ffmpeg,
  dbus,
  pkg-config,
}:

stdenv.mkDerivation (finalAttrs: {
  pname = "tryx-panorama-manager";
  version = "2.5.2";

  src = fetchFromGitHub {
    owner = "DXVSI";
    repo = "Tryx-Linux-GUI";
    tag = "v${finalAttrs.version}";
    hash = "sha256-8n9GUB7yZeK5TKcaYu8xmIfoEoaAt5tPhxadNHeR+iM=";
  };

  nativeBuildInputs = [
    qt6.wrapQtAppsHook
    qt6.qmake
    qt6.qttools
    pkg-config
    protobuf
  ];

  buildInputs = [
    qt6.qtbase
    qt6.qtdeclarative
    libusb1
    systemd
    ffmpeg
    dbus
    protobuf
  ];

  configurePhase = ''
    runHook preConfigure

    qmake6 CONFIG+=c++20 tryx-panorama-all.pro

    qmake6 \
      CONFIG+=c++20 \
      -o Makefile.runtime \
      tryx-panorama.pro

    qmake6 \
      CONFIG+=c++20 \
      -o Makefile.quick \
      tryx-panorama-quick.pro

    sed -i \
      -e "s|${qt6.qtbase}/bin/lrelease|${qt6.qttools}/bin/lrelease|g" \
      -e 's/-std=gnu++1z/-std=gnu++20/g' \
      Makefile.runtime Makefile.quick

    runHook postConfigure
  '';

  buildPhase = ''
    runHook preBuild

    make -j$NIX_BUILD_CORES

    runHook postBuild
  '';

  installPhase = ''
        runHook preInstall

        make INSTALL_ROOT="$out" install

        mkdir -p "$out/bin"
        mkdir -p "$out/lib/tryx-panorama-manager"
        mkdir -p "$out/share/applications"
        mkdir -p "$out/share/icons/hicolor/512x512/apps"
        mkdir -p "$out/share/systemd/user"

        if [ -f "$out/usr/bin/tryx-panorama-manager" ]; then
          install -Dm755 \
            "$out/usr/bin/tryx-panorama-manager" \
            "$out/bin/tryx-panorama-manager"
        fi

        if [ -f "$out/usr/lib/tryx-panorama-manager/tryx-panorama-runtime" ]; then
          install -Dm755 \
            "$out/usr/lib/tryx-panorama-manager/tryx-panorama-runtime" \
            "$out/lib/tryx-panorama-manager/tryx-panorama-runtime"
        fi

        if [ -f "$out/usr/share/applications/tryx-panorama-manager.desktop" ]; then
          install -Dm644 \
            "$out/usr/share/applications/tryx-panorama-manager.desktop" \
            "$out/share/applications/tryx-panorama-manager.desktop"
        fi

        icon_file=$(
          find "$src/resources" -type f \
            \( -iname '*.png' -o -iname '*.svg' \) \
            | head -n1
        )

        if [ -n "$icon_file" ]; then
          case "$icon_file" in
            *.png)
              install -Dm644 \
                "$icon_file" \
                "$out/share/icons/hicolor/512x512/apps/tryx-panorama-manager.png"
              ;;
            *.svg)
              mkdir -p "$out/share/icons/hicolor/scalable/apps"
              install -Dm644 \
                "$icon_file" \
                "$out/share/icons/hicolor/scalable/apps/tryx-panorama-manager.svg"
              ;;
          esac
        fi

        if [ -f "$out/share/applications/tryx-panorama-manager.desktop" ]; then
          sed -i \
            -e 's|^Exec=.*|Exec=tryx-panorama-manager|' \
            -e 's|^Icon=.*|Icon=tryx-panorama-manager|' \
            "$out/share/applications/tryx-panorama-manager.desktop"
        fi

        cat > "$out/share/systemd/user/tryx-panorama.service" <<EOF
    [Unit]
    Description=TRYX Panorama SE 360 Display Runtime
    After=graphical-session.target

    [Service]
    Type=simple
    ExecStart=$out/lib/tryx-panorama-manager/tryx-panorama-runtime
    Restart=on-failure
    RestartSec=2

    [Install]
    WantedBy=default.target
    EOF

        rm -rf "$out/usr"

        runHook postInstall
  '';

  meta = with lib; {
    description = "Qt6 GUI and runtime for TRYX Panorama cooler displays";
    homepage = "https://github.com/DXVSI/Tryx-Linux-GUI";
    license = licenses.mit;
    platforms = [ "x86_64-linux" ];
    mainProgram = "tryx-panorama-manager";
  };
})
