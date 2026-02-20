#!/usr/bin/env python3
#
# setup-vm-rocm-ernic.py
#
# Run inside a new VM to set up for rocm-ernic testing. Does: mount hostfs
# (9p) to ~/Projects with uid/gid remap; install build-essential, rdma-core,
# linux-headers; build and load the driver; merge pci.ids fragment; install
# sysctl and udev rules for dmesg and interface naming; add netplan for
# roc0s0 DHCP IPv4. All steps are idempotent (safe to run multiple times).
#
# Usage: sudo python3 scripts/setup-vm-rocm-ernic.py [OPTIONS]
#   --no-mount      skip mounting hostfs to ~/Projects
#   --no-packages   skip apt install
#   --no-driver     skip driver build and load
#   --no-pciids     skip pci.ids merge
#   --no-dmesg      skip sysctl for unprivileged dmesg
#   --no-udev       skip udev rule for roc0s0
#   --no-netplan    skip netplan for roc0s0 dhcp4
#
# Prerequisites: VM started with virtfs/hostfs (e.g. run-vm-vfio-user.sh
# with FILESYSTEM=/path/on/host). Ubuntu 24.04 assumed for packages.
#

import argparse
import os
import re
import shutil
import subprocess
import sys


def run(cmd, **kwargs):
    """Run command; on failure print and exit unless check=False."""
    check = kwargs.pop("check", True)
    try:
        subprocess.run(cmd, check=check, **kwargs)
    except subprocess.CalledProcessError as e:
        if check:
            print(f"Error: {cmd} failed with {e.returncode}", file=sys.stderr)
            sys.exit(1)
        raise


def module_loaded(name):
    """Return True if kernel module name is loaded."""
    path = "/proc/modules"
    if not os.path.isfile(path):
        return False
    with open(path) as f:
        for line in f:
            if line.split()[0] == name:
                return True
    return False


def main():
    parser = argparse.ArgumentParser(
        description="Set up VM for rocm-ernic testing (mount, packages, "
        "driver, pci.ids, dmesg, udev, netplan)."
    )
    parser.add_argument("--no-mount", action="store_true", help="skip hostfs mount")
    parser.add_argument("--no-packages", action="store_true", help="skip apt install")
    parser.add_argument("--no-driver", action="store_true", help="skip driver build/load")
    parser.add_argument("--no-pciids", action="store_true", help="skip pci.ids merge")
    parser.add_argument("--no-dmesg", action="store_true", help="skip dmesg sysctl")
    parser.add_argument("--no-udev", action="store_true", help="skip udev rule")
    parser.add_argument("--no-netplan", action="store_true", help="skip netplan")
    args = parser.parse_args()

    if os.geteuid() != 0:
        print("Run with sudo (required for mount, apt, insmod, udev, netplan).",
              file=sys.stderr)
        sys.exit(1)

    script_dir = os.path.dirname(os.path.abspath(__file__))
    project_root = os.path.dirname(script_dir)
    driver_dir = os.path.join(project_root, "driver")
    home = os.environ.get("SUDO_HOME") or os.environ.get("HOME") or "/root"
    projects = os.path.join(home, "Projects")
    uid = os.environ.get("SUDO_UID") or str(os.getuid())
    gid = os.environ.get("SUDO_GID") or str(os.getgid())

    # 1. Mount hostfs to ~/Projects (uid/gid remap)
    if not args.no_mount:
        print("=== Mount hostfs to ~/Projects ===")
        os.makedirs(projects, exist_ok=True)
        try:
            run(["mountpoint", "-q", projects], check=False)
            already = True
        except subprocess.CalledProcessError:
            already = False
        if already:
            print("  Already mounted.")
        else:
            opts = (
                f"trans=virtio,version=9p2000.L,uid={uid},gid={gid}"
            )
            run(["mount", "-t", "9p", "-o", opts, "hostfs", projects])
            print("  Mounted hostfs at", projects)

    # 2. Install packages (Ubuntu 24.04)
    if not args.no_packages:
        print("=== Install packages ===")
        run(["apt-get", "update", "-qq"])
        kver = os.uname().release
        run([
            "apt-get", "install", "-y", "-qq",
            "build-essential", "rdma-core", f"linux-headers-{kver}",
        ])
        print("  Installed build-essential, rdma-core, linux-headers.")

    # 3. Build and load driver
    if not args.no_driver:
        print("=== Build and load driver ===")
        if not os.path.isdir(driver_dir):
            print("  Driver dir not found:", driver_dir, file=sys.stderr)
            sys.exit(1)
        run(["make", "clean"], cwd=driver_dir)
        run(["make"], cwd=driver_dir)
        run(["modprobe", "ib_core"], check=False)
        run(["modprobe", "ib_uverbs"], check=False)
        eth_ko = os.path.join(driver_dir, "rocm_ernic_eth.ko")
        rdma_ko = os.path.join(driver_dir, "rocm_ernic_rdma.ko")
        if os.path.isfile(eth_ko) and not module_loaded("rocm_ernic_eth"):
            run(["insmod", eth_ko])
        if os.path.isfile(rdma_ko) and not module_loaded("rocm_ernic_rdma"):
            run(["insmod", rdma_ko])
        print("  Driver built and loaded.")

    # 4. Merge pci.ids fragment
    if not args.no_pciids:
        print("=== Merge pci.ids fragment ===")
        pci_path = None
        for path in ["/usr/share/hwdata/pci.ids", "/usr/share/misc/pci.ids"]:
            if os.path.isfile(path):
                pci_path = path
                break
        if not pci_path:
            print("  No system pci.ids found; skipping.")
        else:
            fragment = os.path.join(script_dir, "pci.ids.rocm-ernic")
            want_line = None
            if os.path.isfile(fragment):
                with open(fragment) as f:
                    for line in f:
                        if line.startswith("\t8000 "):
                            want_line = line.rstrip("\n")
                            break
            with open(pci_path) as f:
                content = f.read()
            if want_line and want_line not in content:
                # Match vendor 1022 line (system may have "1022  Advanced
                # Micro Devices, Inc. [AMD]" not "1022  AMD")
                pattern = r"(^1022\s+.+$)\n"
                if re.search(pattern, content, re.MULTILINE):
                    content = re.sub(
                        pattern,
                        r"\1\n" + want_line + "\n",
                        content,
                        count=1,
                        flags=re.MULTILINE,
                    )
                    with open(pci_path, "w") as f:
                        f.write(content)
                    print("  Merged device 8000 into", pci_path)
                else:
                    print("  Vendor 1022 not found in pci.ids; skipping.")
            else:
                print("  pci.ids already has ROCm ERNIC device entry.")

    # 5. Unprivileged dmesg (copy same content is idempotent; sysctl -p safe to repeat)
    if not args.no_dmesg:
        print("=== Unprivileged dmesg (sysctl) ===")
        src = os.path.join(script_dir, "99-rocm-ernic-dmesg.conf")
        dst = "/etc/sysctl.d/99-rocm-ernic-dmesg.conf"
        if os.path.isfile(src):
            with open(src) as f:
                new_content = f.read()
            if os.path.isfile(dst):
                with open(dst) as f:
                    if f.read() == new_content:
                        print("  Already configured.", dst)
                    else:
                        with open(dst, "w") as f:
                            f.write(new_content)
                        run(["sysctl", "-p", dst])
                        print("  Installed and applied.", dst)
            else:
                shutil.copy2(src, dst)
                run(["sysctl", "-p", dst])
                print("  Installed and applied.", dst)
        else:
            print("  Snippet not found:", src)

    # 6. Udev rule for roc0s0 (skip copy/reload if content unchanged)
    if not args.no_udev:
        print("=== Udev rule (roc0s0) ===")
        src = os.path.join(script_dir, "85-rocm-ernic-net.rules")
        dst = "/etc/udev/rules.d/85-rocm-ernic-net.rules"
        if os.path.isfile(src):
            with open(src) as f:
                new_content = f.read()
            if os.path.isfile(dst):
                with open(dst) as f:
                    if f.read() == new_content:
                        print("  Already installed.", dst)
                    else:
                        with open(dst, "w") as f:
                            f.write(new_content)
                        run(["udevadm", "control", "--reload-rules"])
                        print("  Installed and reloaded udev.")
            else:
                shutil.copy2(src, dst)
                run(["udevadm", "control", "--reload-rules"])
                print("  Installed and reloaded udev.")
        else:
            print("  Rule file not found:", src)

    # 7. Netplan for roc0s0 DHCP (only write and apply if content changed)
    if not args.no_netplan:
        print("=== Netplan (roc0s0 dhcp4) ===")
        netplan = """network:
  version: 2
  ethernets:
    roc0s0:
      dhcp4: true
"""
        path = "/etc/netplan/01-rocm-ernic.yaml"
        if os.path.isfile(path):
            with open(path) as f:
                if f.read() == netplan:
                    print("  Already configured.", path)
                else:
                    with open(path, "w") as f:
                        f.write(netplan)
                    run(["netplan", "apply"])
                    print("  Wrote", path, "and applied.")
        else:
            with open(path, "w") as f:
                f.write(netplan)
            run(["netplan", "apply"])
            print("  Wrote", path, "and applied.")

    print("Done.")


if __name__ == "__main__":
    main()
