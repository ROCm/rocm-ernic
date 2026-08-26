# nix/derivation.nix
#
# CMake build of the rocm-ernic userspace server.
#
# rocm-ernic uses CMake + Ninja, so the nixpkgs cmake/ninja setup hooks do
# almost all the work: adding `ninja` to nativeBuildInputs makes the hook
# configure with -GNinja, and `install(TARGETS rocm-ernic RUNTIME ...)`
# (cmake/ErnicInstall.cmake) puts the binary in $out/bin.
#
# The optional kernel-module (ERNIC_BUILD_KMOD), rdma-core provider
# (ERNIC_RDMA_CORE_BUILD) and systemd-service (ERNIC_INSTALL_SERVICE)
# targets all default OFF, so a plain configure builds only the userspace
# binary — no kernel headers or network fetches needed in the sandbox.
#
# Parameterised so later phases (sanitizers) can reuse the exact build:
#   import ./nix/derivation.nix { inherit pkgs lib libvfio-user; }
#   import ./nix/derivation.nix { ...; enableSanitizers = true; }

{ pkgs
, lib
, libvfio-user
, src ? ../.
, buildType ? "Debug"
, enableSanitizers ? false
, enableThreadSanitizer ? false
, werror ? false
, extraCmakeFlags ? [ ]
}:

let
  packages = import ./packages.nix { inherit pkgs libvfio-user; };
  compatCflags = import ./compat-cflags.nix { };
in
pkgs.stdenv.mkDerivation {
  pname = "rocm-ernic";
  version = "0.2.0";

  inherit src;

  inherit (packages) nativeBuildInputs buildInputs;

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=${buildType}"
    "-DERNIC_WERROR=${if werror then "ON" else "OFF"}"
  ]
  # Sanitizers are wired into CMake already (cmake/ErnicSanitizers.cmake);
  # ASAN+LSAN+UBSAN and TSAN are mutually exclusive there.
  ++ lib.optional enableSanitizers "-DERNIC_USE_SANITIZERS=ON"
  ++ lib.optional enableThreadSanitizer "-DERNIC_USE_THREAD_SANITIZER=ON"
  ++ extraCmakeFlags;

  # The source copy carries no .git, so cmake/ErnicGitInfo.cmake falls back
  # to its safe default SHAs — expected and harmless.

  # Downgrade gcc-15's default-on C99 errors to warnings so the QEMU-ported
  # sources build on the modern toolchain (see nix/compat-cflags.nix).
  env.NIX_CFLAGS_COMPILE = compatCflags.string;

  doCheck = false;

  meta = with lib; {
    description = "Userspace emulated RDMA NIC for virtual machines";
    homepage = "https://github.com/ROCm/rocm-ernic";
    license = licenses.mit;
    platforms = platforms.linux;
    mainProgram = "rocm-ernic";
  };
}
