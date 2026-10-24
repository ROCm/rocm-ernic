"""Starting, watching, and stopping rocm-ernic server processes.

A test that drives servers rather than linking the device code has to be
careful about three things, and this is where that care lives:

  * A server must come up before a client connects, which means waiting
    for its socket, not for a sleep to expire.
  * A server must exit promptly on SIGTERM, and with status 0. A status
    other than 0 is how a sanitizer report from a server process reaches
    the test; a server still running after the deadline has hung in
    teardown, which the TCP backend is known to do.
  * The descriptors a server holds are evidence. A count that climbs
    across a churn loop is the socket leak; sockets left in CLOSE_WAIT
    name it exactly.

Copyright (C) Advanced Micro Devices, Inc.

SPDX-License-Identifier: GPL-2.0-or-later
"""

import os
import shlex
import signal
import subprocess
import tempfile
import time

from . import vfu

# How long a server gets to exit after SIGTERM before it counts as hung.
# Generous because a sanitizer or valgrind build is slow to tear down.
DEFAULT_STOP_TIMEOUT = 20.0


def run_vfu_suite(suite, binary, tests, fixture, extra_args=None):
    """Run @tests against a loopback server in a throwaway directory.

    The server, its socket and its log live under a temporary directory
    that goes away with the run, and the server is always stopped and
    checked, so a sanitizer report in it fails the suite even when a
    test raised.

    @fixture is called with the started server and the run directory,
    and returns the object the tests are handed; anything a suite needs
    to set up per run belongs there. @extra_args, if given, is called
    with the run directory and returns extra server arguments, for a
    suite that needs the server pointed at a path inside it.

    Shared by the test programs that drive a real server over vfio-user,
    which otherwise differ only in what they ask it for.
    """
    with tempfile.TemporaryDirectory(prefix="ernic-vfu-") as run_dir:
        server = Server(
            binary,
            os.path.join(run_dir, "ionic.sock"),
            log_path=os.path.join(run_dir, "server.log"),
            extra_args=() if extra_args is None else extra_args(run_dir),
        )
        server.start(timeout=60)
        fix = None
        try:
            fix = fixture(server, run_dir)
            for name, func in tests:
                suite.run(name, func, fix)
        finally:
            if fix is not None:
                fix.close()
            stop_and_check(suite, server)
    suite.finish()


def stop_and_check(suite, server, label="server"):
    """Stop @server and record whether it exited cleanly and on time.

    A non-zero status is how a sanitizer report from a server process
    reaches a test: the sanitizers run with halt_on_error, so a report
    takes the process down rather than printing into a log nothing
    reads.
    """
    try:
        server.stop()
        suite.check(True, f"{label} exited cleanly on SIGTERM")
    except Exception as exc:  # pylint: disable=broad-except
        suite.check(False, f"{label} shutdown: {exc}")


class ServerHung(Exception):
    """A server did not exit within its deadline after SIGTERM."""


class ServerFailed(Exception):
    """A server exited with a non-zero status."""


def server_wrapper():
    """The command to run the server under, from ERNIC_TEST_SERVER_WRAPPER.

    The test scripts all honour this so CI can run the server under
    valgrind; a new test that starts a server has to as well.
    """
    return shlex.split(os.environ.get("ERNIC_TEST_SERVER_WRAPPER", ""))


class Server:
    """One rocm-ernic process."""

    # The knobs are the interface, and a mesh test sets most of them.
    # pylint: disable=too-many-instance-attributes,too-many-arguments
    # pylint: disable=too-many-positional-arguments
    def __init__(
        self,
        binary,
        socket_path,
        backend="loopback",
        log_path=None,
        env=None,
        extra_args=(),
        log_level="warn",
    ):
        self.binary = binary
        self.socket_path = socket_path
        self.backend = backend
        self.log_path = log_path
        self.extra_args = list(extra_args)
        self.log_level = log_level
        self.env = dict(os.environ)
        if env:
            self.env.update(env)
        self.proc = None
        self._log_fh = None

    @property
    def pid(self):
        """The process id, or None if it isn't running."""
        return self.proc.pid if self.proc is not None else None

    def start(self, timeout=20.0, wait=True):
        """Start the server, and by default wait for its socket."""
        argv = server_wrapper() + [
            self.binary,
            "--socket",
            self.socket_path,
            "--backend",
            self.backend,
            "--log-level",
            self.log_level,
        ]
        argv += self.extra_args
        if self.log_path:
            # pylint: disable-next=consider-using-with  # closed in stop()
            self._log_fh = open(self.log_path, "wb")
            stdout = self._log_fh
        else:
            stdout = subprocess.DEVNULL
        # pylint: disable-next=consider-using-with  # the process outlives this call
        self.proc = subprocess.Popen(
            argv, stdout=stdout, stderr=subprocess.STDOUT, env=self.env
        )
        if wait and not self.wait_ready(timeout):
            self.stop()
            raise ServerFailed(f"server did not come up: {self.tail_log()}")
        return self

    def wait_ready(self, timeout=20.0):
        """Wait until the vfio-user socket accepts a connection."""
        return vfu.wait_for_socket(self.socket_path, timeout, self.proc)

    def connect(self, **kwargs):
        """Attach a vfio-user client to this server."""
        return vfu.VfioUserClient(self.socket_path).connect(**kwargs)

    def is_running(self):
        """Whether the process is still alive."""
        return self.proc is not None and self.proc.poll() is None

    def kill(self):
        """SIGKILL, for simulating a node that dies without cleaning up.

        Raises ServerFailed if the process was already gone, and if what
        it died of was not the signal sent here. A test that kills a
        server on purpose cannot otherwise tell that outcome from a
        server that had already aborted -- which is exactly what a
        sanitizer report looks like, since the sanitizers run with
        halt_on_error. Accepting a corpse would let that report vanish.
        """
        if self.proc is None:
            raise ServerFailed(f"server {self.socket_path} was never started")
        if not self.is_running():
            status = self.proc.returncode
            self._close_log()
            raise ServerFailed(
                f"server {self.socket_path} had already exited with status "
                f"{status} before it was killed: {self.tail_log()}"
            )
        self.proc.send_signal(signal.SIGKILL)
        self.proc.wait()
        status = self.proc.returncode
        self._close_log()
        if status != -signal.SIGKILL:
            raise ServerFailed(
                f"server {self.socket_path} exited with status {status}, "
                f"not the SIGKILL it was sent: {self.tail_log()}"
            )

    def pause(self):
        """SIGSTOP, for a node that stops answering without going away.

        Its sockets stay open, and the kernel goes on accepting and
        acknowledging for it, so its peers see a connection that is up
        but silent, as a partition or a hung host would leave it.
        """
        self.proc.send_signal(signal.SIGSTOP)

    def resume(self):
        """SIGCONT, to let a paused server run again."""
        self.proc.send_signal(signal.SIGCONT)

    def stop(self, timeout=DEFAULT_STOP_TIMEOUT, check=True):
        """SIGTERM the server and wait for it, holding it to the deadline.

        Raises ServerHung if it outlives @timeout and, with @check,
        ServerFailed if it exits non-zero. It is SIGKILLed either way, so
        a hung server is never left behind.
        """
        if self.proc is None:
            return 0
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
            # A paused server would hold the signal until it ran again
            self.proc.send_signal(signal.SIGCONT)
        hung = False
        try:
            self.proc.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            hung = True
            self.proc.send_signal(signal.SIGKILL)
            self.proc.wait()
        status = self.proc.returncode
        self._close_log()
        if hung:
            raise ServerHung(
                f"server {self.socket_path} did not exit within "
                f"{timeout:.1f}s of SIGTERM"
            )
        if check and status != 0:
            raise ServerFailed(
                f"server {self.socket_path} exited with status {status}: "
                f"{self.tail_log()}"
            )
        return status

    def _close_log(self):
        if self._log_fh is not None:
            self._log_fh.close()
            self._log_fh = None

    def tail_log(self, lines=40):
        """The last few lines of the server's log, for a failure message."""
        if not self.log_path or not os.path.exists(self.log_path):
            return "(no log)"
        with open(self.log_path, "rb") as fh:
            text = fh.read().decode("utf-8", "replace")
        return "\n".join(text.splitlines()[-lines:])

    def log_text(self):
        """The whole log, for a test that greps it."""
        if not self.log_path or not os.path.exists(self.log_path):
            return ""
        with open(self.log_path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")

    def wait_for_log(self, needle, timeout=10.0):
        """Wait for @needle to appear in the log. True if it did."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if needle in self.log_text():
                return True
            if not self.is_running():
                return needle in self.log_text()
            time.sleep(0.05)
        return False

    def wait_for_count(self, needle, wanted, timeout=10.0):
        """Wait for @needle to appear at least @wanted times. True if it did.

        A log is cumulative, so waiting for a line to appear at all says
        nothing about whether it appeared *this* time round; a test that
        repeats something has to count against a mark it took first.
        """
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.log_text().count(needle) >= wanted:
                return True
            time.sleep(0.05)
        return self.log_text().count(needle) >= wanted

    def fd_count(self):
        """How many descriptors the process holds, or -1 if it is gone."""
        if not self.is_running():
            return -1
        try:
            return len(os.listdir(f"/proc/{self.proc.pid}/fd"))
        except OSError:
            return -1

    def socket_inodes(self):
        """The inode numbers of the process's sockets."""
        inodes = set()
        try:
            entries = os.listdir(f"/proc/{self.proc.pid}/fd")
        except OSError:
            return inodes
        for entry in entries:
            try:
                target = os.readlink(f"/proc/{self.proc.pid}/fd/{entry}")
            except OSError:
                continue
            if target.startswith("socket:["):
                inodes.add(target[len("socket:[") : -1])
        return inodes

    def close_wait_count(self):
        """How many of the process's TCP sockets are in CLOSE_WAIT.

        A connection whose peer hung up and that nothing closed sits here
        forever; a count that grows across a churn loop is the leak.
        """
        mine = self.socket_inodes()
        if not mine:
            return 0
        count = 0
        for path in ("/proc/net/tcp", "/proc/net/tcp6"):
            try:
                with open(path, encoding="ascii") as fh:
                    next(fh, None)
                    for line in fh:
                        fields = line.split()
                        if len(fields) > 9 and fields[3] == "08" and fields[9] in mine:
                            count += 1
            except OSError:
                continue
        return count
