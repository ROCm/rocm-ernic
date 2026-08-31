# nix/microvms/lib.nix
#
# Host-side lifecycle driver generator for the rocm-ernic microvms.
#
# mkRunner builds a `run-<arch>-tests` command (writeShellApplication, so it
# is shellcheck-checked and strict) that: launches the VM's microvm-run in
# its own session, waits for the serial console port, watches it for the
# self-test verdict via scripts/vm-verify.exp, then tears the VM down and
# exits with the verdict's status.
#
# The VM runner (`vm`) is referenced by store path, so the driver needs no
# `nix build` at runtime. It is built from the *host* pkgs (it runs on the
# build host to drive QEMU), independent of the guest arch.

{ pkgs, lib }:

let
  verifyScript = ./scripts/vm-verify.exp;
in
{
  mkRunner = { arch, vm, ports, timeouts, hostname, description ? "" }:
    pkgs.writeShellApplication {
      name = "run-${arch}-tests";
      runtimeInputs = [ pkgs.coreutils pkgs.expect pkgs.procps ];
      text = ''
        echo "=================================================="
        echo "  rocm-ernic microvm run: ${arch}"
        ${lib.optionalString (description != "") ''echo "  ${description}"''}
        echo "=================================================="

        serial=${toString ports.serial}
        ready_timeout=${toString timeouts.serialReady}
        verdict_timeout=${toString timeouts.serviceReady}

        # Launch the VM. microvm-run execs qemu, which we tag with
        # -name process=${hostname} (see mkVm.nix) so teardown can target it
        # precisely regardless of how many processes sit in between.
        "${vm}/bin/microvm-run" > /tmp/ernic-vm-${arch}.log 2>&1 &
        vmpid=$!
        echo "[run] microvm-run pid=$vmpid (log: /tmp/ernic-vm-${arch}.log)"

        # Invoked via `trap cleanup EXIT`, which shellcheck does not see.
        # shellcheck disable=SC2329
        cleanup() {
          echo "[run] tearing down VM"
          pkill -f "process=${hostname}" 2>/dev/null || true
          kill "$vmpid" 2>/dev/null || true
          wait "$vmpid" 2>/dev/null || true
        }
        trap cleanup EXIT

        # Wait for QEMU's serial TCP server to accept connections.
        echo "[run] waiting up to ''${ready_timeout}s for serial port $serial ..."
        ok=0
        for _ in $(seq 1 "$ready_timeout"); do
          if (exec 3<>"/dev/tcp/127.0.0.1/$serial") 2>/dev/null; then
            exec 3>&- 3<&- || true
            ok=1
            break
          fi
          # Bail early if the VM process died.
          kill -0 "$vmpid" 2>/dev/null || { echo "[run] VM exited before serial came up"; break; }
          sleep 1
        done
        if [ "$ok" != 1 ]; then
          echo "[run] serial port never came up — see /tmp/ernic-vm-${arch}.log"
          exit 2
        fi

        # Watch the console for the self-test verdict.
        echo "[run] watching console for verdict (up to ''${verdict_timeout}s) ..."
        set +e
        expect "${verifyScript}" 127.0.0.1 "$serial" "$verdict_timeout"
        rc=$?
        set -e

        case "$rc" in
          0) echo "[run] RESULT: PASS (${arch})" ;;
          1) echo "[run] RESULT: FAIL (${arch}) — self-test reported failure" ;;
          *) echo "[run] RESULT: ERROR (${arch}) — verify rc=$rc" ;;
        esac
        exit "$rc"
      '';
    };
}
