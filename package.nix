{
  lib,
  stdenv,
  qt6,
  protobuf,
  libusb1,
  systemd,
  ffmpeg,
  pkg-config,
}:

stdenv.mkDerivation {
  pname = "tryx-panorama-manager";
  version = lib.removeSuffix "\n" (builtins.readFile ./VERSION);

  src = lib.cleanSource ./.;

  nativeBuildInputs = [
    qt6.qmake
    qt6.qttools
    qt6.wrapQtAppsHook
    pkg-config
    protobuf
  ];

  buildInputs = [
    qt6.qtbase
    qt6.qtdeclarative
    protobuf
    libusb1
    systemd
  ];

  postPatch = ''
    substituteInPlace tryx-panorama.pro \
      --replace-fail '$$system(pkg-config --variable=systemduserunitdir systemd)' /usr/lib/systemd/user \
      --replace-fail '$$system(pkg-config --variable=systemduserpresetdir systemd)' /usr/lib/systemd/user-preset
    substituteInPlace systemd/tryx-panorama.service \
      --replace-fail /usr/lib/tryx-panorama-manager/tryx-panorama-runtime \
        $out/lib/tryx-panorama-manager/tryx-panorama-runtime
  '';

  qmakeFlags = [
    "tryx-panorama-all.pro"
    "CONFIG+=c++20"
    "QT_TOOL.lrelease.binary=${lib.getBin qt6.qttools}/bin/lrelease"
  ];

  installFlags = [ "INSTALL_ROOT=$(out)" ];

  postInstall = ''
    cp -a $out/usr/. $out/
    rm -r $out/usr
  '';

  qtWrapperArgs = [ "--prefix PATH : ${lib.makeBinPath [ ffmpeg ]}" ];
  postFixup = ''
    wrapQtApp $out/lib/tryx-panorama-manager/tryx-panorama-runtime
  '';

  meta = {
    description = "Qt6 GUI and runtime for TRYX Panorama cooler displays";
    homepage = "https://github.com/DXVSI/Tryx-Linux-GUI";
    license = lib.licenses.mit;
    platforms = [ "x86_64-linux" ];
    mainProgram = "tryx-panorama-manager";
  };
}
