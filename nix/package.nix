{
  lib,
  stdenv,
  fetchurl,
  cmake,
  pkg-config,
  python3,
  autoPatchelfHook,
  wayland-scanner,
  binutils,
  glm,
  glfw,
  glew,
  freeglut,
  libGLU,
  libGL,
  libglvnd,
  SDL2,
  lz4,
  zlib,
  ffmpeg,
  mpv-unwrapped,
  libpulseaudio,
  freetype,
  harfbuzz,
  dbus,
  wayland,
  wayland-protocols,
  libxkbcommon,
  xorg,
  gmp,
  nodejs-slim_24,
  nss,
  nspr,
  cups,
  at-spi2-atk,
  at-spi2-core,
  libdrm,
  mesa,
  alsa-lib,
  pango,
  cairo,
  glib,
  expat,
  libva,
  vulkan-loader,
  pipewire,
  src,
  version,
  disableKdeFeatures ? false,
}:

let
  # V8 comes from Node.js's shared library like on the distributions (Arch's AUR libnode is the same build).
  # nixpkgs only has a static libv8, whose local-exec TLS can't go into our shared library
  libnode = nodejs-slim_24.overrideAttrs (old: {
    pname = "libnode";
    configureFlags = (old.configureFlags or [ ]) ++ [ "--shared" ];
    doCheck = false;
    doInstallCheck = false;
  });

  # the version CMakeLists.txt pins, CMake's DownloadCEF finds the archive already in its folder
  cefVersion = "135.0.17+gcbc1c5b+chromium-135.0.7049.52";
  cefArchive = "cef_binary_${cefVersion}_linux64_minimal.tar.bz2";
  cef = fetchurl {
    url = "https://cef-builds.spotifycdn.com/${lib.replaceStrings [ "+" ] [ "%2B" ] cefArchive}";
    hash = "sha256-JKwZgOYr57GuosM31r1Lx3DczYs35HxtuUs5fxPsTcY=";
  };
in
stdenv.mkDerivation {
  pname = "linux-wallpaperengine-kde";
  inherit version src;

  nativeBuildInputs = [
    cmake
    pkg-config
    python3
    autoPatchelfHook
    wayland-scanner
    binutils
  ];

  buildInputs = [
    glm
    glfw
    glew
    freeglut
    libGLU
    libGL
    SDL2
    lz4
    zlib
    ffmpeg
    mpv-unwrapped
    libpulseaudio
    freetype
    harfbuzz
    dbus
    wayland
    wayland-protocols
    libxkbcommon
    gmp
    libnode
    # CEF's own libraries
    nss
    nspr
    cups
    at-spi2-atk
    at-spi2-core
    libdrm
    mesa
    alsa-lib
    pango
    cairo
    glib
    expat
  ]
  ++ (with xorg; [
    libX11
    libXrandr
    libXinerama
    libXcursor
    libXi
    libXxf86vm
    libXcomposite
    libXdamage
    libXext
    libXfixes
    libxshmfence
    libxcb
  ]);

  # loaded at runtime with dlopen (GL dispatch, Vulkan for CEF, VA-API, the audio backends SDL picks)
  runtimeDependencies = map lib.getLib [
    libglvnd
    vulkan-loader
    libva
    libpulseaudio
    pipewire
    alsa-lib
    wayland
    libxkbcommon
  ];

  cmakeFlags = [
    (lib.cmakeBool "DISABLE_KDE_FEATURES" disableKdeFeatures)
    (lib.cmakeFeature "CMAKE_BUILD_TYPE" "Release")
    (lib.cmakeFeature "V8_INCLUDE_DIR" "${libnode}/include/node")
    # relative, package_release.sh installs into a prefix of its own
    (lib.cmakeFeature "CMAKE_INSTALL_LIBDIR" "lib")
  ];

  # the CEF wrapper library builds with -Werror and defines its own _FORTIFY_SOURCE
  hardeningDisable = [ "fortify" "fortify3" ];

  preConfigure = ''
    # node installs only the versioned libnode.so.<abi>
    cmakeFlagsArray+=("-DV8_LIBRARY=$(echo ${libnode}/lib/libnode.so.*)")
    mkdir -p build/cef
    ln -s ${cef} build/cef/${cefArchive}
  '';

  # the same file set as the release tarballs, the install tree also carries the vendored libraries' tools
  installPhase = ''
    runHook preInstall
    bash ../tools/package_release.sh . linux-wallpaperengine-kde "$TMPDIR/dist"
    mkdir -p $out/opt $out/bin
    tar -xzf "$TMPDIR/dist/linux-wallpaperengine-kde.tar.gz" -C $out/opt
    ln -s $out/opt/linux-wallpaperengine-kde/linux-wallpaperengine $out/bin/linux-wallpaperengine
    runHook postInstall
  '';

  dontStrip = true;

  meta = {
    description = "Wallpaper Engine live wallpapers on Linux, KDE/Wayland fork of linux-wallpaperengine";
    homepage = "https://github.com/WallpaperEngineLover/linux-wallpaperengine-kde";
    license = lib.licenses.gpl3Only;
    platforms = [ "x86_64-linux" ];
    mainProgram = "linux-wallpaperengine";
  };
}
