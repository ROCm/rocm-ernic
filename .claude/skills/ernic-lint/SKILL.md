---
name: ernic-lint
description: "Run rocm-ernic's non-C checks: ShellCheck, actionlint, and the embedded-shell checker; cmakelint; black, pylint, and vermin for Python; codespell; doc8 and the Sphinx docs build; and ansible-lint and ansible-test sanity. Use after editing shell scripts, GitHub workflows or actions, CMake files, Python, docs, or the Ansible collection, or when one of those CI jobs fails."
---

# Shell, CMake, Python, docs, and Ansible checks

The rules for writing these files are in [AGENTS.md](../../../AGENTS.md);
this file says how to check them. Each check is quick, so run it for any
file of its kind you touch, from the repository root. CI runs most of
them, but a workflow's `paths:` filter can miss a change, and vermin
isn't in CI at all.

## Shell

ShellCheck 0.11.0, default checks only (no `--list-optional`), over every
`*.sh`, `*.sh.in`, and extension-less executable with a shell shebang, as
[shellcheck.yml](../../../.github/workflows/shellcheck.yml) does:

```bash
git ls-files | while IFS= read -r f; do
  if [[ "$f" == *.sh || "$f" == *.sh.in ]]; then
    echo "$f"
  elif [[ "${f##*/}" != *.* && -x "$f" ]] &&
    head -n1 -- "$f" | grep -Eqs '^#!.*\b(ba)?sh\b'; then
    echo "$f"
  fi
done | xargs -d '\n' shellcheck
```

[actionlint](https://github.com/rhysd/actionlint)
(`pip install actionlint-py`; run `actionlint`) checks the workflows and
runs ShellCheck on their `run:` blocks. Its config,
[`.github/actionlint.yaml`](../../../.github/actionlint.yaml), ignores
the unsupported `$/` form of `uses:`.

[`tools/check-embedded-shell.py`](../../../tools/check-embedded-shell.py)
(needs PyYAML and ShellCheck 0.11.0) checks the shell the others miss,
reporting at the original file and line; `--list` shows what it found:

- Ansible `shell:` and `raw:` tasks, as `sh` unless `executable` is set.
- Composite actions' `run:` steps.
- Script heredocs in any of these.
- Workflow `run:` blocks GitHub runs under `sh`: a step without `shell:`
  in a job with `container:`.

AGENTS.md says how to write a heredoc so it gets checked.

## CMake

cmakelint with [`.cmakelintrc`](../../../.cmakelintrc), as
[cmakelint.yml](../../../.github/workflows/cmakelint.yml) does:

```bash
git ls-files -z -- 'CMakeLists.txt' '*/CMakeLists.txt' '*.cmake' |
  xargs -0 cmakelint
```

## Python

black, pylint, and vermin over every `*.py` and extension-less
python-shebang script, except `nix/analysis/triage/`. pylint needs
`prometheus/requirements-exporter.txt` and PyYAML installed. CI
([pylint.yml](../../../.github/workflows/pylint.yml)) runs black and
pylint under Python 3.12, and they miss most uses of newer Python than
3.10. vermin (`pip install vermin`) catches those, printing
`Target versions not met` on a violation.

```bash
git ls-files -- ':!:nix/analysis/triage/' |
  while IFS= read -r f; do
    if [[ "$f" == *.py ]]; then
      echo "$f"
    elif [[ "${f##*/}" != *.* ]] &&
      head -n1 -- "$f" | grep -Eqs '^#!.*python'; then
      echo "$f"
    fi
  done > /tmp/python-files
xargs -d '\n' black --check < /tmp/python-files
xargs -d '\n' pylint < /tmp/python-files
xargs -d '\n' vermin -t=3.10- --violations --no-tips < /tmp/python-files
```

## Spelling and documentation

- codespell with [`.codespellrc`](../../../.codespellrc), over the whole
  tree: `codespell` (CI:
  [codespell.yml](../../../.github/workflows/codespell.yml)).
- doc8: `doc8 docs/ --max-line-length 80`.
- The docs must build without a Sphinx warning. This needs Doxygen, but
  not a C compiler or the C dependencies (CI:
  [docs-check.yml](../../../.github/workflows/docs-check.yml)):

  ```bash
  cmake -B build-docs -DERNIC_DOCS_ONLY=ON -DERNIC_BUILD_DOCS=ON \
    -DERNIC_DOCS_WERROR=ON
  cmake --build build-docs --target sphinx-html
  ```

## Ansible

ansible-lint with `--strict`, so warnings fail, and
`ansible-test sanity`, both on `ansible/`. The exact commands, their
requirements, and the pinned ansible-lint version are in
[ansible-collection.yml](../../../.github/workflows/ansible-collection.yml).
Use that version: others report different problems.
