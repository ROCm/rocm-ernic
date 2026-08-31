# nix/oci.nix
#
# Build a reproducible OCI (Docker) image for rocm-ernic straight from its
# Nix closure — no Dockerfile. `dockerTools.streamLayeredImage` writes a small
# executable that streams content-addressed layers to `docker load` on demand,
# so nothing large is materialized in the store and layers dedupe across
# rebuilds.
#
# Two variants (the `variant` arg), mirroring the xtcp2 reference:
#   "minimal"  scratch/distroless — the server binary and its closure only.
#              Smallest; no shell, so `docker exec ... sh` won't work.
#   "debug"    minimal + bash + coreutils, for in-container troubleshooting
#              (`docker exec -it <c> bash`). Tagged `<version>-debug`.
#
# The image architecture follows the `pkgs` it is given: pass the native pkgs
# for an amd64 image, or a crossSystem pkgs set (nix/cross.nix's `crossPkgs`)
# together with that set's cross-built `rocm-ernic` for an arm64 image built
# from an x86_64 host. dockerTools routes the layer-assembly tooling to the
# build host automatically, so cross image builds "just work".
#
# Deployment note: rocm-ernic and QEMU rendezvous over a UNIX socket (the
# `vfio-user-pci` transport), so a container must share that socket path with
# whoever attaches to it — bind-mount the socket directory (default below:
# /run/rocm-ernic). The `loopback` and `tcp` backends containerize cleanly;
# the `verbs` backend needs host RDMA devices (/dev/infiniband) and elevated
# privileges, so it is not a clean container target.
#
# Usage (from flake.nix):
#   oci-image       = import ./nix/oci.nix { inherit pkgs lib rocm-ernic; };
#   oci-image-debug = import ./nix/oci.nix { inherit pkgs lib rocm-ernic; variant = "debug"; };
#
# Load and run (streamLayeredImage → the result is a loader script):
#   nix build .#oci-image && ./result | docker load
#   docker run --rm -v "$PWD/sock:/run/rocm-ernic" rocm-ernic:<tag>

{ pkgs
, lib
, rocm-ernic
, variant ? "minimal"
, tag ? null
}:

let
  isDebug = variant == "debug";

  version = rocm-ernic.version or "0.0.0";
  imageTag =
    if tag != null then tag
    else if isDebug then "${version}-debug"
    else version;

  # Directory the default command puts the vfio-user socket in. Bind-mount
  # this so QEMU (or another container) can reach the socket.
  socketDir = "/run/rocm-ernic";

  # minimal = server + closure only; debug adds a shell + coreutils.
  contents = [ rocm-ernic ]
    ++ lib.optionals isDebug [ pkgs.bashInteractive pkgs.coreutils ];
in
pkgs.dockerTools.streamLayeredImage {
  name = "rocm-ernic";
  tag = imageTag;
  inherit contents;

  # Create the writable runtime dirs the server and socket need. Paths are
  # relative to the image root inside the build sandbox.
  extraCommands = ''
    mkdir -p tmp
    mkdir -p .${socketDir}
  '';

  config = {
    Entrypoint = [ "${rocm-ernic}/bin/rocm-ernic" ];
    # Sensible container default: the self-contained loopback backend, with
    # the socket in the bind-mountable directory above. Override at runtime,
    # e.g. `docker run ... rocm-ernic:<tag> --backend tcp:manager:listen:5000`.
    Cmd = [ "--backend" "loopback" "--socket" "${socketDir}/vfio-user.sock" ];
    WorkingDir = "/";
    Labels = {
      "org.opencontainers.image.title" = "rocm-ernic";
      "org.opencontainers.image.description" =
        "Userspace emulated RDMA NIC for virtual machines";
      "org.opencontainers.image.source" = "https://github.com/ROCm/rocm-ernic";
      "org.opencontainers.image.version" = version;
      "org.opencontainers.image.variant" = variant;
    };
  };
}
