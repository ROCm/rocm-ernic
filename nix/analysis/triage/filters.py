"""Noise constants, path classifiers, filtering, and deduplication.

Adapted for rocm-ernic's C codebase. Path patterns reflect this repo's
layout:
  - src/rocm_ernic_*.{c,h}         our own userspace code
  - src/from-qemu/utils/           QEMU-ported wire-protocol parsers
                                   (DHCP, rdma-cm, TCP) — untrusted input
  - src/from-qemu/hw/rdma/         QEMU-ported RDMA backends / PVRDMA device
  - tests/                         test programs

Unlike the xdp2 original, nothing under src/ is treated as unactionable
third-party: the QEMU-ported sources are maintained here and are exactly
the security-sensitive parsers we also fuzz, so they stay in the triage
view and are marked security-sensitive to raise their priority.
"""

from finding import Finding


# Truly non-actionable third-party trees. rocm-ernic vendors nothing under
# src/ that we want to hide, so this stays empty; the is_third_party()
# fallback still drops anything outside src/ (driver/, build artifacts).
THIRD_PARTY_PATTERNS = []

# Generated files — rocm-ernic has no code-generation step, so none.
GENERATED_FILE_PATTERNS = []

EXCLUDED_CHECK_IDS = {
    # Cppcheck noise — tool limitations, not code bugs
    'missingIncludeSystem',
    'missingInclude',
    'unmatchedSuppression',
    'checkersReport',
    'syntaxError',                  # Can't parse complex macro constructs
    'preprocessorErrorDirective',   # Intentional #error guards / macro failures
    'unknownMacro',                 # Doesn't understand project/glib macros
    # Cppcheck false positives in idiomatic C
    'arithOperationsOnVoidPointer', # GNU C extension, intentional in networking code
    'subtractPointers',             # container_of style pointer arithmetic
    # Clang-tidy build errors (not real findings)
    'clang-diagnostic-error',
    'clang-diagnostic-implicit-function-declaration',  # gcc-15 C99 env issue; compiles fine
    # _FORTIFY_SOURCE warnings (build config, not code bugs)
    '-W#warnings',
    '-Wcpp',
}

EXCLUDED_MESSAGE_PATTERNS = [
    '_FORTIFY_SOURCE',
]

# Test programs live under tests/. (Leading pattern also catches any nested
# test dir just in case.)
TEST_PATH_PATTERNS = ['tests/', '/test_']

# Security-sensitive locations: the parsers that handle untrusted wire data
# (the prime fuzz targets) plus our own userspace glue.
SECURITY_PATHS = [
    'src/from-qemu/utils/',
    'src/from-qemu/hw/rdma/',
    'src/rocm_ernic',
]


def _match_generated(path: str) -> bool:
    """Check if file matches a generated file pattern (supports * glob)."""
    import fnmatch
    name = path.rsplit('/', 1)[-1] if '/' in path else path
    return any(fnmatch.fnmatch(name, pat) for pat in GENERATED_FILE_PATTERNS)


def is_generated(path: str) -> bool:
    return _match_generated(path)


def is_third_party(path: str) -> bool:
    for pat in THIRD_PARTY_PATTERNS:
        if pat in path:
            return True
    # Files not under src/ are outside the analysis scope (driver/, build).
    return not path.startswith('src/')


def is_test_code(path: str) -> bool:
    return any(pat in path for pat in TEST_PATH_PATTERNS)


def is_security_sensitive(path: str) -> bool:
    return any(pat in path for pat in SECURITY_PATHS)


def filter_findings(findings: list) -> list:
    """Remove out-of-scope code and known false positive categories."""
    return [
        f for f in findings
        if not is_third_party(f.file)
        and not is_generated(f.file)
        and f.check_id not in EXCLUDED_CHECK_IDS
        and f.line > 0
        and not any(pat in f.message for pat in EXCLUDED_MESSAGE_PATTERNS)
    ]


def deduplicate(findings: list) -> list:
    """Deduplicate findings by (file, line, check_id).

    clang-tidy reports the same header finding once per translation unit.
    Keep first occurrence only.
    """
    seen = set()
    result = []
    for f in findings:
        key = f.dedup_key()
        if key not in seen:
            seen.add(key)
            result.append(f)
    return result
