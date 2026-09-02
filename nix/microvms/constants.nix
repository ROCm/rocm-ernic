# nix/microvms/constants.nix
#
# Per-architecture configuration for the rocm-ernic microvm run layer.
#
# Each target architecture gets a NixOS microvm (nix/microvms/mkVm.nix) that
# boots under QEMU and runs the cross-built server against the PCI-config
# test client on the loopback backend — the emulated-run counterpart to the
# native loopback exercise in nix/analysis/. All non-x86 targets run under
# TCG (full software emulation), so they are slow; x86_64 uses KVM and exists
# mainly as a fast baseline for validating the harness itself.
#
# Modeled on ~/Downloads/xdp2/nix/microvms/constants.nix, trimmed to what a
# userspace socket server needs (no eBPF/BTF/XDP machinery).

rec {
  # Base TCP port for the per-VM consoles; each arch gets a block of 10.
  #   +0 = serial console (boot messages)
  #   +1 = virtio console (interactive / command channel)
  portBase = 23600;
  archPortOffset = {
    x86_64  = 0;
    aarch64 = 10;
    riscv64 = 20;
    armv7l  = 30;
    ppc64le = 40;
  };

  architectures = {
    x86_64 = {
      nixSystem = "x86_64-linux";
      qemuMachine = "pc";
      qemuCpu = "host";
      useKvm = true;
      consoleDevice = "ttyS0";
      # No cross build needed; the flake's cross layer skips x86_64. This VM
      # is a KVM baseline to shake out the harness before the slow TCG runs.
      crossArch = null;
      biosArgs = [ ];
      runMode = "vm";
      description = "x86_64 (KVM baseline for the harness)";
    };

    aarch64 = {
      nixSystem = "aarch64-linux";
      qemuMachine = "virt";
      qemuCpu = "cortex-a72";
      useKvm = false;
      consoleDevice = "ttyAMA0";
      crossArch = "aarch64";
      biosArgs = [ ];
      runMode = "vm";
      description = "aarch64 (ARM64, QEMU TCG)";
    };

    riscv64 = {
      nixSystem = "riscv64-linux";
      qemuMachine = "virt";
      qemuCpu = "rv64";
      useKvm = false;
      consoleDevice = "ttyS0";
      crossArch = "riscv64";
      biosArgs = [ "-bios" "default" ];  # OpenSBI firmware
      runMode = "vm";
      description = "riscv64 (RISC-V 64-bit, QEMU TCG)";
    };

    # armv7l and ppc64le: full-VM boot is blocked by an upstream microvm.nix
    # limitation — its QEMU runner derives the system-emulator binary name
    # from the nix-system prefix (`qemu-system-armv7l`,
    # `qemu-system-powerpc64le`), but QEMU ships those as `qemu-system-arm` /
    # `qemu-system-ppc64`, so microvm-run fails with "No such file or
    # directory" before the guest ever starts. aarch64/riscv64/x86_64 happen
    # to match QEMU's names, so they boot. Rather than force a full QEMU
    # rebuild per-arch (the closure-minimizing `.override` path needs a real
    # qemu, not a symlink wrapper) or disable microvm's `optimize.enable`
    # (which perturbs the delicate 9p-store boot proven for the other arches),
    # these two land as a qemu-user smoke test (runMode = "smoke"): the
    # cross-built server is executed under user-mode emulation
    # (`qemu-arm` / `qemu-ppc64le`), exercising the target-arch dynamic loader,
    # every shared library in its closure, and the program's own startup /
    # argument parsing, and must exit 0. See nix/microvms/smoke.nix.
    armv7l = {
      nixSystem = "armv7l-linux";
      qemuMachine = "virt";
      qemuCpu = "cortex-a7";
      useKvm = false;
      consoleDevice = "ttyAMA0";
      crossArch = "armv7l";
      biosArgs = [ ];
      runMode = "smoke";
      qemuUserBin = "qemu-arm";
      description = "armv7l (32-bit ARM, qemu-user smoke)";
    };

    ppc64le = {
      nixSystem = "powerpc64le-linux";
      # sPAPR para-virtual machine (not bare-metal powernv) — the machine
      # NixOS/QEMU actually boots for ppc64le. Console is the hypervisor
      # console hvc0.
      qemuMachine = "pseries";
      qemuCpu = "power9";
      useKvm = false;
      consoleDevice = "hvc0";
      crossArch = "ppc64le";
      biosArgs = [ ];
      runMode = "smoke";
      qemuUserBin = "qemu-ppc64le";
      description = "ppc64le (POWER9 little-endian, qemu-user smoke)";
    };
  };

  getPorts = arch:
    let base = portBase + archPortOffset.${arch};
    in { serial = base; virtio = base + 1; };

  # Hostnames must not contain underscores.
  archHostname = {
    x86_64 = "x86-64";
    aarch64 = "aarch64";
    riscv64 = "riscv64";
    armv7l = "armv7l";
    ppc64le = "ppc64le";
  };
  getHostname = arch: "ernic-vm-${archHostname.${arch}}";

  # Emulated boots are slow; give systemd headroom. Avoid exactly 2048 MiB —
  # microvm.nix warns QEMU hangs at exactly 2 GB
  # (https://github.com/microvm-nix/microvm.nix/issues/171).
  getMem = arch: if architectures.${arch}.useKvm then 1024 else 1536;
  vcpu = 2;

  # Per-phase timeouts (seconds). Emulated arches (especially riscv64/ppc64le)
  # boot much slower than KVM.
  timeouts = {
    kvm  = { build = 900;  serialReady = 30;  serviceReady = 90;  shutdown = 30;  command = 10; };
    tcg  = { build = 2400; serialReady = 90;  serviceReady = 300; shutdown = 60;  command = 20; };
    slow = { build = 3600; serialReady = 180; serviceReady = 600; shutdown = 90;  command = 30; };
  };
  getTimeouts = arch:
    if architectures.${arch}.useKvm then timeouts.kvm
    else if arch == "riscv64" || arch == "ppc64le" then timeouts.slow
    else timeouts.tcg;
}
