# CI/CD Workflows

This directory contains GitHub Actions workflows for automated testing and
quality assurance.

## Workflows

### Build and Test (`build-and-test.yml`)

Builds the vfu_pvrdma server and runs loopback backend tests on every push
and pull request.

**What it tests:**
- Build with meson + ninja
- Address Sanitizer (ASan) and Undefined Behavior Sanitizer (UBSan)
- Loopback backend functionality
- RDMA operations (PD, CQ, MR, QP creation)
- QP state transitions (INIT → RTR → RTS)
- Send/Recv operations with varying sizes
- Multiple iterations for stability

**Test approach:**
- Uses standalone test client (`test_loopback_standalone.c`)
- No QEMU required (fast, CI-friendly)
- Tests backend operations directly
- Runs in ~2-3 minutes

### Spell Check (`spellcheck.yml`)

Checks markdown files for spelling errors using `pyspelling`.

Configuration: `.spellcheck.yml` with project-specific technical terms.

### Linting (`lint.yml`)

Multiple linters for code quality:

1. **clang-format**: C code style checking
2. **cppcheck**: Static analysis for C code
3. **shellcheck**: Shell script checking

## Running Locally

### Build and Test
```bash
# Install dependencies
sudo apt-get install meson ninja-build libibverbs-dev librdmacm-dev

# Install libvfio-user
git clone https://github.com/nutanix/libvfio-user.git
cd libvfio-user && mkdir build && cd build
cmake .. && make && sudo make install && sudo ldconfig

# Build
cd /home/stebates/Projects/vfu-rdma
meson setup build -Db_sanitize=address,undefined
meson compile -C build

# Run tests (requires RDMA device or loopback backend)
cd tests
gcc -o test_loopback_ci test_loopback_standalone.c -libverbs -lpthread
sudo ./test_loopback_ci
```

### Spell Check
```bash
pip install pyspelling
pyspelling -c .spellcheck.yml
```

### Linting
```bash
# Format check
find src tests -name '*.c' -o -name '*.h' | \
  xargs clang-format --dry-run --Werror

# Static analysis
cppcheck --enable=all src/vfu_*.c

# Shell scripts
find . -name '*.sh' | xargs shellcheck
```

## Test Coverage

The CI currently tests:
- ✅ Loopback backend (full RDMA operations)
- ✅ Memory safety (ASan/UBSan)
- ✅ Code style (clang-format)
- ✅ Static analysis (cppcheck)
- ✅ Shell script quality (shellcheck)
- ✅ Documentation spelling

**Not yet tested in CI:**
- ❌ Full integration with QEMU/VM (requires nested virt)
- ❌ Driver loading and kernel module tests
- ❌ Performance benchmarks
- ❌ Real hardware (verbs backend)

These can be added as optional/on-demand workflows or tested manually.

## Adding New Tests

To add a new test to CI:

1. Create test program in `tests/`
2. Update `build-and-test.yml` to build and run it
3. Ensure it returns 0 on success, non-zero on failure
4. Keep tests fast (<5 minutes) for good CI experience

## Troubleshooting

If CI fails:

1. Check the workflow run logs on GitHub
2. Download artifacts (server logs) if available
3. Reproduce locally using the commands above
4. Fix the issue and push again

Common issues:
- Missing dependencies: Update workflow `apt-get install`
- Test timeout: Increase `timeout` value or optimize test
- Flaky tests: Add retries or fix race conditions

