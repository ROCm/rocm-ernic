# nix/libvfio-user.nix
#
# From-source build of nutanix/libvfio-user.
#
# libvfio-user is not packaged in nixpkgs, yet rocm-ernic's top-level
# CMakeLists.txt requires it (pkg_check_modules(VFIO_USER vfio-user), see
# CMakeLists.txt:66). Upstream's meson build installs the library and the
# headers (under include/vfio-user/) but does NOT ship a pkg-config file,
# so we synthesise a `vfio-user.pc` in postInstall. That lets rocm-ernic's
# pkg-config lookup succeed unmodified — no patching of the consumer's
# CMake, and no reliance on the /usr/local fallback (which cannot see the
# Nix store).
#
# The pinned source revision comes from the `libvfio-user` flake input
# (see flake.nix / flake.lock), passed in as `src`.

{ lib
, stdenv
, src
, meson
, ninja
, pkg-config
, json_c
, cmocka
}:

stdenv.mkDerivation {
  pname = "libvfio-user";
  # Upstream has no meaningful release tags; the exact commit is pinned by
  # the flake input. "unstable" documents that this tracks a pinned master.
  version = "unstable";

  inherit src;

  nativeBuildInputs = [ meson ninja pkg-config ];

  # json-c: required unconditionally by meson.build.
  # cmocka:  required at configure time even though we do not run the tests.
  buildInputs = [ json_c cmocka ];

  # Use the "release" buildtype so meson's `debug` option is false. Upstream
  # injects -Werror (and -DDEBUG) manually under `if opt_debug`, and their
  # bundled samples/tests do not build cleanly with -Werror on current
  # compilers. release turns that path off. We add -g back via cflags so
  # downstream sanitizer/valgrind runs still get symbols through the library.
  mesonBuildType = "release";
  mesonFlags = [
    "-Dtran-pipe=false"
  ];
  env.NIX_CFLAGS_COMPILE = "-g";

  # The test suite needs pytest/valgrind and is flaky; skip it.
  doCheck = false;

  # Emit a pkg-config file so downstream `pkg_check_modules(VFIO_USER
  # vfio-user)` resolves. Headers live in include/vfio-user/ and the code
  # includes <vfio-user/libvfio-user.h>, so includedir is the plain
  # include/ dir (the vfio-user/ prefix comes from the #include itself).
  postInstall = ''
    mkdir -p "$out/lib/pkgconfig"
    cat > "$out/lib/pkgconfig/vfio-user.pc" <<EOF
    prefix=$out
    exec_prefix=\''${prefix}
    libdir=\''${prefix}/lib
    includedir=\''${prefix}/include

    Name: vfio-user
    Description: libvfio-user (nutanix) — VFIO-over-socket device emulation
    Version: 0.0.1
    Libs: -L\''${libdir} -lvfio-user
    Cflags: -I\''${includedir}
    EOF
  '';

  meta = with lib; {
    description = "Framework for emulating VFIO devices in userspace over a socket";
    homepage = "https://github.com/nutanix/libvfio-user";
    license = licenses.bsd3;
    platforms = platforms.linux;
  };
}
