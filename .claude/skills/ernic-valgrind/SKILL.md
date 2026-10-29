---
name: ernic-valgrind
description: "Run rocm-ernic's unit tests and server tests under valgrind memcheck to find uninitialized reads, including uninitialized bytes sent with send() or write(). Use for a change to wire data or to anything passed to send()/write(), or when CI's GCC Debug valgrind leg fails."
---

# Valgrind

Memcheck finds uninitialized reads that the sanitizers don't, including
uninitialized bytes passed to `send()`/`write()`. CI's GCC Debug leg runs
every test this way, and then the server itself; locally, run the tests
that exercise the change.

## Build

Use **GCC**, without sanitizers, in its own build directory, with
`-DERNIC_TRIVIAL_AUTO_VAR_INIT=OFF`: by default Clang and GCC 12+ fill
uninitialized locals (`-ftrivial-auto-var-init=pattern`), which hides
the reads valgrind is looking for. Install valgrind before configuring,
so that `<valgrind/valgrind.h>` is found (`tcp-thread-stop-unit` uses it
to skip a `RLIMIT_NOFILE` row valgrind breaks).

```bash
cmake -B build-valgrind -G Ninja -DCMAKE_C_COMPILER=gcc \
  -DCMAKE_BUILD_TYPE=Debug -DERNIC_BUILD_P4=ON \
  -DERNIC_TRIVIAL_AUTO_VAR_INIT=OFF
cmake --build build-valgrind
```

## Unit tests

CI runs every test except those below, which run scripts, CMake, Python,
or a server launcher. Locally, replace the `-E` with an `-R` naming the
affected unit tests:

```bash
ctest --test-dir build-valgrind -T memcheck --output-on-failure \
  --overwrite "MemoryCheckCommand=$(command -v valgrind)" \
  --overwrite "MemoryCheckCommandOptions=--leak-check=full --track-origins=yes --num-callers=40 --error-exitcode=1" \
  -E '^(pci-config-test|find-rdma-device-tuple|find-ipv4-gid-index|server-lifecycle|ionic-server-smoke|ionic-ci|tcp-mesh-ci|nvmeof-ci|s3-ci|ionic-stats-interval-cli|ionic-eth-queues-cli|cmake-subproject|ernic-package-deb|ernic-package-deb-config-paths|ernic-vfu-client|ernic-ionic-driver|ernic-p4-driver|ernic-ionic-hw-stats|ernic-mesh-stress|ernic-mesh-flow|ernic-feature-macros)$'
```

Reports go to `build-valgrind/Testing/Temporary/MemoryChecker.*.log`,
not ctest's output.

## The server

Test scripts run the server under `ERNIC_TEST_SERVER_WRAPPER` (split on
whitespace); a new script that starts the server must too.
`test_server_lifecycle` can't read a command from the environment (that
is a CodeQL finding; see AGENTS.md's scanner patterns), so it runs under
valgrind with `--trace-children=yes`. Not every script checks the
server's exit status, so check the per-process logs. Limit the `-R` to
the server tests that exercise the change:

```bash
rm -rf build-valgrind/valgrind-server
mkdir build-valgrind/valgrind-server
opts="--log-file=$PWD/build-valgrind/valgrind-server/%p.log --leak-check=full --track-origins=yes --num-callers=40 --error-exitcode=1"
ERNIC_TEST_SERVER_WRAPPER="valgrind $opts" \
  ctest --test-dir build-valgrind --output-on-failure \
  -R '^(pci-config-test|ionic-server-smoke|ionic-ci|tcp-mesh-ci|nvmeof-ci|s3-ci|ernic-vfu-client|ernic-ionic-driver|ernic-p4-driver|ernic-ionic-hw-stats|ernic-mesh-flow|ionic-stats-interval-cli|ionic-eth-queues-cli)$'
valgrind --trace-children=yes $opts build-valgrind/tests/test_server_lifecycle
grep -L 'ERROR SUMMARY: 0 errors' build-valgrind/valgrind-server/*.log
```

`grep -L` lists the logs without a clean summary; no output means a
clean run.

Valgrind doesn't check writes into mapped guest memory. Those rely on
AGENTS.md's rule to `memset` wire data before filling it in.
