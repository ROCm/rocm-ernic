#!/usr/bin/env python3
# Copyright (c) Advanced Micro Devices, Inc. All rights reserved.
#
# SPDX-License-Identifier: MIT

# The hyphenated script name is not a valid module name. Enabling it again
# at once still covers the module's name, but no name inside it.
# pylint: disable=invalid-name
# pylint: enable=invalid-name

"""Run ShellCheck on shell that lives inside other files.

The ShellCheck workflow checks the shell scripts and actionlint checks the
workflows' run: blocks, but neither sees:

  * the shell: and raw: tasks in the Ansible collection,
  * the run: steps of the composite actions in .github/actions/,
  * heredocs whose body is itself a script, such as the ones the system
    tests feed to a guest with ``ssh ... 'bash -s' <<'EOF'``, wherever they
    appear: in a script, a workflow, a composite action, or an Ansible task,
  * the run: blocks of steps that GitHub runs with sh rather than bash (a
    step with no shell: in a job with a container:), which actionlint
    checks as bash.

This pulls each of those out, checks it with ShellCheck in the dialect it
runs under, and reports findings against the file and line they came from.

Run from the repository root:

    tools/check-embedded-shell.py           # check everything
    tools/check-embedded-shell.py --list    # list what it found

Exits 0 when clean, 1 when ShellCheck reports anything, and 2 when it
couldn't check something (ShellCheck missing, a YAML file it can't parse,
or a heredoc with no end).
"""

from __future__ import annotations

import argparse
import bisect
import dataclasses
import json
import os
import pathlib
import re
import shlex

# Bandit flags the import itself. Each call below is marked with the reason
# it is safe.
import subprocess  # nosec B404
import sys
import tempfile
from typing import Callable

import yaml

# Checks that can't apply to an extracted snippet, as actionlint also turns
# off for run: blocks:
#   SC1091  a sourced file it wasn't given
#   SC2154  a variable set outside the snippet: by env:, environment:, the
#           parent script, or an assignment in front of ``bash -s``
#   SC2050, SC2157, SC2194
#           a comparison that looks constant because a ${{ }} or {{ }}
#           expression was replaced by a placeholder
EXCLUDED_CHECKS = "SC1091,SC2050,SC2154,SC2157,SC2194"

# Maps a (line, column) in a snippet, both 1-based, to its file.
Locator = Callable[[int, int], "tuple[int, int]"]


class CheckError(Exception):
    """Something couldn't be checked; the message says what."""


@dataclasses.dataclass
class Snippet:
    """A piece of shell, and how to map its positions back to the file."""

    path: str
    what: str
    shell: str
    text: str
    locate: Locator


# --------------------------------------------------------------------------
# Templates
# --------------------------------------------------------------------------

GITHUB_EXPR = re.compile(r"\$\{\{.*?\}\}", re.S)
JINJA = re.compile(r"\{\{.*?\}\}|\{%.*?%\}|\{#.*?#\}", re.S)


def blank_out(text: str, pattern: re.Pattern[str]) -> str:
    """Replace each match with underscores, keeping lines and columns."""
    return pattern.sub(lambda m: re.sub(r"[^\n]", "_", m.group(0)), text)


# --------------------------------------------------------------------------
# YAML
# --------------------------------------------------------------------------


def read_yaml(path: str) -> tuple[yaml.Node | None, list[str]]:
    """Parse a YAML file into nodes, which keep their source positions."""
    text = pathlib.Path(path).read_text(encoding="utf-8")
    try:
        return yaml.compose(text, Loader=yaml.SafeLoader), text.splitlines()
    except yaml.YAMLError as err:
        raise CheckError(f"{path}: can't parse YAML: {err}") from err


def get(node: yaml.Node | None, key: str) -> yaml.Node | None:
    """Return a mapping node's value for key, or None."""
    if isinstance(node, yaml.MappingNode):
        for k, v in node.value:
            if isinstance(k, yaml.ScalarNode) and k.value == key:
                return v
    return None


def get_str(node: yaml.Node | None, key: str) -> str | None:
    """Return a mapping node's scalar value for key, or None."""
    value = get(node, key)
    return value.value if isinstance(value, yaml.ScalarNode) else None


def items(node: yaml.Node | None) -> list[yaml.Node]:
    """Return a sequence node's items, or nothing."""
    return list(node.value) if isinstance(node, yaml.SequenceNode) else []


def indent_of(line: str) -> int:
    """The number of leading spaces."""
    return len(line) - len(line.lstrip(" "))


def piece_locator(pieces: list[tuple[int, int, int, int]], default: int) -> Locator:
    """Map a position to the piece of the value it falls in.

    Each piece is the value line and column where a source line's text
    starts, and the file line and column it came from.
    """

    def locate(line: int, col: int) -> tuple[int, int]:
        best = (default, 1)
        for vline, vcol, fline, fcol in pieces:
            if vline == line and vcol <= col:
                best = (fline, fcol + col - vcol)
        return best

    return locate


def folded_pieces(
    lines: list[str], begin: int, end: int
) -> list[tuple[int, int, int, int]]:
    """Where each line of a folded block, lines[begin:end], lands in its value.

    YAML joins two adjacent lines with a space, unless one of them is
    indented more than the block, in which case the line break stays. Blank
    lines between them become line breaks.
    """
    body = [(i, lines[i]) for i in range(begin, min(end, len(lines)))]
    base = next((indent_of(s) for _, s in body if s.strip()), 0)
    pieces: list[tuple[int, int, int, int]] = []
    vline, vlen, blanks, prev_more = 1, 0, 0, False
    for i, raw in body:
        if not raw.strip():
            blanks += 1
            continue
        more = indent_of(raw) > base
        if pieces and blanks == 0 and not more and not prev_more:
            vcol, fcol = vlen + 2, indent_of(raw) + 1
        else:
            if pieces:
                vline += blanks + (1 if more or prev_more else 0) or 1
            vcol, fcol = 1, base + 1
        pieces.append((vline, vcol, i + 1, fcol))
        vlen = vcol - 1 + len(raw[fcol - 1 :].rstrip())
        blanks, prev_more = 0, more
    return pieces


def plain_pieces(
    lines: list[str], first: int, column: int, last: int
) -> list[tuple[int, int, int, int]]:
    """Where each line of a plain scalar, lines[first:last + 1], lands.

    YAML joins the lines with spaces, and blank lines become line breaks.
    """
    pieces: list[tuple[int, int, int, int]] = []
    vline, vlen, blanks = 1, 0, 0
    for i in range(first, min(last + 1, len(lines))):
        start = column if i == first else indent_of(lines[i])
        text = lines[i][start:].rstrip()
        if not text:
            blanks += 1
            continue
        if pieces and blanks:
            vline, vlen = vline + blanks, 0
        vcol = vlen + 2 if vlen else 1
        pieces.append((vline, vcol, i + 1, start + 1))
        vlen = vcol - 1 + len(text)
        blanks = 0
    return pieces


def scalar_locator(node: yaml.ScalarNode, lines: list[str]) -> Locator:
    """Map (line, column) in a scalar's value back to its file.

    A literal block (|) keeps its lines, so only the indentation moves.
    Folded blocks (>) and plain scalars that span lines are joined with
    spaces, so each piece of the value is matched back to the line it came
    from.
    """
    first = node.start_mark.line  # 0-based line of the scalar or its indicator
    if node.style == "|":
        body = lines[first + 1 : node.end_mark.line + 1]
        indent = next((indent_of(s) for s in body if s.strip()), 0)
        return lambda line, col: (first + 1 + line, col + indent)
    if node.style == ">":
        pieces = folded_pieces(lines, first + 1, node.end_mark.line + 1)
        return piece_locator(pieces, first + 1)
    column = node.start_mark.column + (1 if node.style in ("'", '"') else 0)
    if node.style is None and node.end_mark.line > first:
        pieces = plain_pieces(lines, first, column, node.end_mark.line)
        return piece_locator(pieces, first + 1)
    return lambda line, col: (first + line, column + col if line == 1 else col)


# --------------------------------------------------------------------------
# Sources
# --------------------------------------------------------------------------


def git_files(*patterns: str) -> list[str]:
    """Tracked files matching the pathspecs."""
    # No shell, and the pathspecs are literals in this script, after "--".
    # git is found on PATH, so the script also runs on NixOS.
    out = subprocess.run(  # nosec B603 B607
        ["git", "ls-files", "-z", "--", *patterns],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    return [f for f in out.split("\0") if f and os.path.isfile(f)]


def shell_scripts() -> list[str]:
    """The files the ShellCheck workflow checks; see the ernic-lint skill."""
    found = []
    for f in git_files():
        name = f.rsplit("/", 1)[-1]
        if f.endswith((".sh", ".sh.in")):
            found.append(f)
        elif "." not in name and os.access(f, os.X_OK):
            with open(f, "rb") as fh:
                first = fh.readline()
            if re.match(rb"#!.*\b(ba)?sh\b", first):
                found.append(f)
    return found


def dialect(shell: str | None, default: str) -> str | None:
    """bash or sh for a shell: or executable: value, None for another one."""
    if not shell:
        return default
    word = shell.split()[0].rsplit("/", 1)[-1]
    return word if word in ("bash", "sh") else None


def from_scripts() -> list[Snippet]:
    """Whole scripts, checked here only for the heredocs inside them."""
    return [
        Snippet(
            path,
            "script",
            "bash",
            pathlib.Path(path).read_text(encoding="utf-8"),
            lambda line, col: (line, col),
        )
        for path in shell_scripts()
    ]


def run_step(
    path: str, lines: list[str], step: yaml.Node, what: str, shell: str
) -> Snippet | None:
    """The run: block of a workflow or composite-action step, if it has one."""
    run = get(step, "run")
    if not isinstance(run, yaml.ScalarNode):
        return None
    name = get_str(step, "name") or get_str(step, "id") or "?"
    return Snippet(
        path,
        f"{what}step {name}",
        shell,
        blank_out(run.value, GITHUB_EXPR),
        scalar_locator(run, lines),
    )


def from_workflows() -> tuple[list[Snippet], list[Snippet]]:
    """The run: blocks of the workflows.

    Returns the blocks GitHub runs with sh, which are checked here, and
    every bash or sh block, which is searched for heredocs.
    """
    as_sh, every = [], []
    for path in git_files(".github/workflows/*.yml", ".github/workflows/*.yaml"):
        root, lines = read_yaml(path)
        wf_shell = get_str(get(get(root, "defaults"), "run"), "shell")
        jobs = get(root, "jobs")
        for key, job in jobs.value if isinstance(jobs, yaml.MappingNode) else []:
            shell = get_str(get(get(job, "defaults"), "run"), "shell") or wf_shell
            in_container = get(job, "container") is not None
            for step in items(get(job, "steps")):
                step_shell = get_str(step, "shell") or shell
                if step_shell is None and in_container:
                    snippet = run_step(path, lines, step, f"job {key.value}, ", "sh")
                    if snippet:
                        snippet.what += ", runs under sh"
                        as_sh.append(snippet)
                        every.append(snippet)
                elif dialect(step_shell, "bash"):
                    snippet = run_step(
                        path,
                        lines,
                        step,
                        f"job {key.value}, ",
                        dialect(step_shell, "bash") or "bash",
                    )
                    if snippet:
                        every.append(snippet)
    return as_sh, every


def from_actions() -> list[Snippet]:
    """The run: steps of the composite actions."""
    snippets = []
    for path in git_files(
        ".github/actions/*/action.yml", ".github/actions/*/action.yaml"
    ):
        root, lines = read_yaml(path)
        for step in items(get(get(root, "runs"), "steps")):
            shell = dialect(get_str(step, "shell"), "bash")
            snippet = run_step(path, lines, step, "", shell) if shell else None
            if snippet:
                snippets.append(snippet)
    return snippets


ANSIBLE_SHELL_MODULES = ("ansible.builtin.shell", "shell", "ansible.builtin.raw", "raw")


def ansible_task(path: str, lines: list[str], task: yaml.Node) -> list[Snippet]:
    """The shell of an Ansible task, if it's a shell: or raw: task."""
    snippets = []
    for module in ANSIBLE_SHELL_MODULES:
        value = get(task, module)
        cmd = get(value, "cmd") if isinstance(value, yaml.MappingNode) else value
        if not isinstance(cmd, yaml.ScalarNode):
            continue
        # The shell module runs /bin/sh unless told otherwise.
        executable = get_str(value, "executable") or get_str(
            get(task, "args"), "executable"
        )
        shell = dialect(executable, "sh")
        if shell:
            snippets.append(
                Snippet(
                    path,
                    f"task {get_str(task, 'name') or '?'}",
                    shell,
                    blank_out(cmd.value, JINJA),
                    scalar_locator(cmd, lines),
                )
            )
    return snippets


def from_ansible() -> list[Snippet]:
    """The shell: and raw: tasks of the Ansible collection."""
    snippets = []
    for path in git_files("ansible/*.yml", "ansible/*.yaml"):
        root, lines = read_yaml(path)
        todo = [root]
        while todo:
            node = todo.pop()
            if isinstance(node, yaml.SequenceNode):
                todo.extend(node.value)
            elif isinstance(node, yaml.MappingNode):
                todo.extend(v for _, v in node.value)
                snippets.extend(ansible_task(path, lines, node))
    return snippets


# --------------------------------------------------------------------------
# Heredocs
# --------------------------------------------------------------------------

# The word after <<: anything but blanks and operators, with quoted parts.
HEREDOC_WORD = re.compile(
    r"""(-?)[ \t]*((?:[^\s;&|<>()'"\\]|'[^'\n]*'|"[^"\n]*"|\\.)+)"""
)

# A # after one of these, or at the start of the text, starts a comment.
WORD_BREAKS = " \t\n;&|()"

# ssh options that take an argument (ssh(1)).
SSH_OPTS_WITH_ARG = set("BbcDEeFIiJLlmOoPpQRSWw")


@dataclasses.dataclass
class Heredoc:
    """A heredoc found in a piece of shell."""

    line: int  # 0-based line of the << operator
    column: int  # 0-based column of the << operator
    delim: str
    quoted: bool
    command: str  # the command it's redirected to
    body_start: int  # 0-based line where the body starts
    body: str


def split_words(text: str) -> list[str]:
    """Split a command into words, and split quoted words again.

    A remote command such as ``'bash -s'`` is one word to the local shell,
    but its words are what say what reads the heredoc.
    """
    try:
        words = shlex.split(text)
    except ValueError:
        words = text.split()
    out = []
    for word in words:
        try:
            out.extend(shlex.split(word))
        except ValueError:
            out.append(word)
    return out


# Where one command ends and the next starts. A & is a separator unless it
# belongs to a redirection such as 2>&1, <&3 or &>file.
COMMAND_SEPARATOR = re.compile(r"\$\(|&&|\|\||[;|(]|(?<![<>])&(?!>)")

# A redirection word: its operator, and its target unless that is the next
# word.
REDIRECTION = re.compile(r"(?:\d+|&)?(?:>>?|<)&?")


def without_redirections(words: list[str]) -> list[str]:
    """The words of a command, without its redirections."""
    out, skip_target = [], False
    for word in words:
        if skip_target:
            skip_target = False
            continue
        match = REDIRECTION.match(word)
        if match:
            skip_target = match.end() == len(word)
            continue
        out.append(word)
    return out


def ssh_runs_login_shell(words: list[str]) -> bool:
    """Whether an ssh command names a host and no remote command."""
    if "ssh" not in words:
        return False
    rest = words[words.index("ssh") + 1 :]
    i = 0
    while i < len(rest) and rest[i].startswith("-"):
        takes_arg = len(rest[i]) == 2 and rest[i][1] in SSH_OPTS_WITH_ARG
        i += 2 if takes_arg else 1
    return len(rest) - i == 1


def heredoc_shell(command: str, body: str) -> str | None:
    """bash or sh if the command runs the heredoc as a script, else None."""
    # Only the last simple command reads the heredoc.
    raw = split_words(COMMAND_SEPARATOR.split(command)[-1])
    for i, word in enumerate(raw):
        written = word.endswith(".sh") and (
            word.startswith(">") or (i > 0 and raw[i - 1] in (">", ">>", "tee"))
        )
        if written:
            return "sh" if re.match(r"#!\s*\S*/sh\b", body) else "bash"
    words = without_redirections(raw)
    for word in words:
        name = word.rsplit("/", 1)[-1]
        if name in ("bash", "sh"):
            return name
    if ssh_runs_login_shell(words):
        # The guests' login shell.
        return "bash"
    return None


def local_expansion_removed(body: str) -> str:
    """What the remote shell reads from an unquoted heredoc.

    The local shell expands $name and ${...} first, so those become
    placeholders, and \\$, \\` and \\\\ become the character they escape.
    """
    out, i = [], 0
    while i < len(body):
        ch = body[i]
        nxt = body[i + 1] if i + 1 < len(body) else ""
        if ch == "\\" and nxt in "$`\\":
            out.append(nxt)
            i += 2
        elif ch == "$" and nxt == "{":
            end = body.find("}", i)
            end = len(body) - 1 if end < 0 else end
            out.append("_" * (end - i + 1))
            i = end + 1
        elif ch == "$" and (nxt.isalnum() or nxt == "_"):
            j = i + 1
            while j < len(body) and (body[j].isalnum() or body[j] == "_"):
                j += 1
            out.append("_" * (j - i))
            i = j
        else:
            out.append(ch)
            i += 1
    return "".join(out)


def command_before(lines: list[str], line: int, column: int) -> str:
    """The command ending at lines[line][column], with the lines it continues."""
    command = lines[line][:column]
    while line > 0 and lines[line - 1].endswith("\\"):
        line -= 1
        command = lines[line][:-1] + " " + command
    return command


class HeredocScanner:
    """Finds heredocs the way the shell does.

    A << is a heredoc only outside quotes, comments and arithmetic, so
    ``'<<EOF'``, ``# <<EOF`` and ``$((1 << n))`` aren't, but one in a
    command substitution is, even inside double quotes:
    ``out="$(ssh h 'bash -s' <<'EOF'``. Its body starts on the line after
    the operator, and a line can have more than one.
    """

    def __init__(self, text: str) -> None:
        self.text = text
        self.lines = text.split("\n")
        self.starts = [0]
        for line in self.lines[:-1]:
            self.starts.append(self.starts[-1] + len(line) + 1)
        self.pos = 0
        self.pending: list[tuple[int, str, bool, str]] = []
        self.found: list[Heredoc] = []
        # What pos is inside, innermost last: 0 for double quotes, or the
        # parenthesis depth of a $( ) command substitution.
        self.nest: list[int] = []

    def scan(self) -> list[Heredoc]:
        """Return the heredocs in the text, not counting nested ones."""
        text = self.text
        while self.pos < len(text):
            if self.nest and self.nest[-1] == 0:
                self.step_double_quoted()
            elif not self.skip_literal() and not self.step_nesting():
                self.step_code()
        if self.pending:
            line = self.line_of(self.pending[0][0])
            raise CheckError(
                f"no end to the heredoc <<{self.pending[0][1]} on line {line + 1}"
            )
        return self.found

    def step_code(self) -> None:
        """Move past one token of code outside double quotes."""
        text = self.text
        ch = text[self.pos]
        if ch == "\\":
            self.pos += 2
        elif ch == "#" and self.at_word_start():
            end = text.find("\n", self.pos)
            self.pos = len(text) if end < 0 else end
        elif text.startswith("<<<", self.pos):
            self.pos += 3
        elif text.startswith("<<", self.pos):
            self.operator()
        elif ch == "\n":
            self.pos += 1
            self.read_bodies()
        else:
            self.pos += 1

    def step_double_quoted(self) -> None:
        """Move past one character inside double quotes.

        Only a command substitution can hold a heredoc there.
        """
        text, pos = self.text, self.pos
        if text[pos] == "\\":
            self.pos += 2
        elif text[pos] == '"':
            self.nest.pop()
            self.pos += 1
        elif text.startswith("$((", pos):
            self.skip_arithmetic(pos + 3)
        elif text.startswith("$(", pos):
            self.nest.append(1)
            self.pos += 2
        else:
            self.pos += 1

    def step_nesting(self) -> bool:
        """Enter or leave double quotes or $( ), if pos does."""
        text, pos = self.text, self.pos
        in_substitution = bool(self.nest)
        if text[pos] == '"':
            self.nest.append(0)
        elif text.startswith("$(", pos):
            self.nest.append(1)
            self.pos += 1
        elif text[pos] == "(" and in_substitution:
            self.nest[-1] += 1
        elif text[pos] == ")" and in_substitution:
            self.nest[-1] -= 1
            if not self.nest[-1]:
                self.nest.pop()
        else:
            return False
        self.pos += 1
        return True

    def skip_literal(self) -> bool:
        """Move past a single-quoted string or arithmetic at pos, if one starts."""
        text, pos = self.text, self.pos
        if text[pos] == "'":
            self.skip_quoted("'", pos + 1, escapes=False)
        elif text.startswith("$'", pos):
            self.skip_quoted("'", pos + 2, escapes=True)
        elif text.startswith("$((", pos):
            self.skip_arithmetic(pos + 3)
        elif text.startswith("((", pos) and self.at_word_start():
            self.skip_arithmetic(pos + 2)
        else:
            return False
        return True

    def at_word_start(self) -> bool:
        """Whether the character at pos starts a word."""
        return self.pos == 0 or self.text[self.pos - 1] in WORD_BREAKS

    def line_of(self, pos: int) -> int:
        """The 0-based line that pos is on."""
        return bisect.bisect_right(self.starts, pos) - 1

    def skip_quoted(self, quote: str, pos: int, escapes: bool) -> None:
        """Move past a quoted string whose text starts at pos."""
        while pos < len(self.text) and self.text[pos] != quote:
            pos += 2 if escapes and self.text[pos] == "\\" else 1
        self.pos = pos + 1

    def skip_arithmetic(self, pos: int) -> None:
        """Move past $(( )) or (( )), whose text starts at pos."""
        depth = 2
        while pos < len(self.text) and depth:
            depth += {"(": 1, ")": -1}.get(self.text[pos], 0)
            pos += 1
        self.pos = pos

    def operator(self) -> None:
        """Note the heredoc whose << is at pos; its body comes later."""
        match = HEREDOC_WORD.match(self.text, self.pos + 2)
        if not match:
            self.pos += 2
            return
        dash, word = match.groups()
        delim = re.sub(r"\\(.)|['\"]", r"\1", word)
        self.pending.append((self.pos, delim, any(c in word for c in "'\"\\"), dash))
        self.pos = match.end()

    def read_bodies(self) -> None:
        """Read the bodies of the heredocs on the line that just ended."""
        line = self.line_of(self.pos)
        for op_pos, delim, quoted, dash in self.pending:
            end = line
            while (
                end < len(self.lines)
                and (self.lines[end].lstrip("\t") if dash else self.lines[end]) != delim
            ):
                end += 1
            if end == len(self.lines):
                raise CheckError(
                    f"no end to the heredoc <<{delim} "
                    f"on line {self.line_of(op_pos) + 1}"
                )
            op_line = self.line_of(op_pos)
            column = op_pos - self.starts[op_line]
            body = self.lines[line:end]
            self.found.append(
                Heredoc(
                    op_line,
                    column,
                    delim,
                    quoted,
                    command_before(self.lines, op_line, column),
                    line,
                    "\n".join(b.lstrip("\t") if dash else b for b in body) + "\n",
                )
            )
            line = end + 1
        self.pending = []
        self.pos = self.starts[line] if line < len(self.lines) else len(self.text)


def find_heredocs(text: str) -> list[Heredoc]:
    """The heredocs in a piece of shell, not counting nested ones."""
    return HeredocScanner(text).scan()


def offset_locator(parent: Locator, offset: int) -> Locator:
    """A locator for text that starts offset lines into its parent's."""
    return lambda line, col: parent(offset + line, col)


def heredocs(snippet: Snippet) -> list[Snippet]:
    """The heredocs in a snippet that are scripts, including nested ones."""
    try:
        found = find_heredocs(snippet.text)
    except CheckError as err:
        raise CheckError(f"{snippet.path} ({snippet.what}): {err}") from err
    snippets = []
    for doc in found:
        shell = heredoc_shell(doc.command, doc.body)
        if not shell:
            continue
        inner = Snippet(
            snippet.path,
            f"{snippet.what}, heredoc <<{doc.delim}",
            shell,
            doc.body if doc.quoted else local_expansion_removed(doc.body),
            offset_locator(snippet.locate, doc.body_start),
        )
        snippets.append(inner)
        snippets.extend(heredocs(inner))
    return snippets


# --------------------------------------------------------------------------
# ShellCheck
# --------------------------------------------------------------------------


def run_shellcheck(shell: str, names: list[str]) -> list[dict]:
    """ShellCheck's comments on the files, all checked as one dialect."""
    env = dict(os.environ)
    # The options are fixed here, so the result doesn't depend on the caller.
    env.pop("SHELLCHECK_OPTS", None)
    command = ["shellcheck", "--norc", "-s", shell, "-e", EXCLUDED_CHECKS]
    try:
        # No shell. The names are paths shellcheck() made in its own
        # temporary directory, so none starts with "-" and reads as an
        # option. ShellCheck only parses the snippets in them.
        result = subprocess.run(  # nosec B603
            [*command, "-f", "json1", *names],
            capture_output=True,
            text=True,
            env=env,
            check=False,
        )
    except FileNotFoundError as err:
        raise CheckError("shellcheck is not installed") from err
    if result.returncode not in (0, 1):
        raise CheckError(f"shellcheck failed: {result.stderr.strip()}")
    return json.loads(result.stdout)["comments"]


def shellcheck(snippets: list[Snippet]) -> list[str]:
    """Check the snippets and return the findings, mapped to their files."""
    findings = []
    with tempfile.TemporaryDirectory() as tmp:
        by_name = {}
        for n, snippet in enumerate(snippets):
            name = os.path.join(tmp, f"{n}.{snippet.shell}")
            pathlib.Path(name).write_text(snippet.text, encoding="utf-8")
            by_name[name] = snippet
        for shell in ("bash", "sh"):
            names = [n for n, s in by_name.items() if s.shell == shell]
            for comment in run_shellcheck(shell, names) if names else []:
                snippet = by_name[comment["file"]]
                line, col = snippet.locate(comment["line"], comment["column"])
                message = (
                    f"{comment['level']}: {comment['message']} "
                    f"[SC{comment['code']}] ({snippet.what})"
                )
                findings.append((snippet.path, line, col, message))
    return [f"{p}:{line}:{col}: {msg}" for p, line, col, msg in sorted(findings)]


def main() -> int:
    """Find the embedded shell, check it, and report."""
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", maxsplit=1)[0])
    parser.add_argument(
        "--list", action="store_true", help="list what would be checked"
    )
    args = parser.parse_args()
    try:
        as_sh, workflow_blocks = from_workflows()
        actions = from_actions()
        ansible = from_ansible()
        checked = as_sh + actions + ansible
        for container in from_scripts() + workflow_blocks + actions + ansible:
            checked.extend(heredocs(container))
        if args.list:
            for s in checked:
                print(f"{s.path}:{s.locate(1, 1)[0]}: {s.shell}: {s.what}")
            return 0
        findings = shellcheck(checked)
    except CheckError as err:
        print(f"error: {err}", file=sys.stderr)
        return 2
    for finding in findings:
        print(finding)
    print(f"{len(checked)} snippets checked, {len(findings)} findings", file=sys.stderr)
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
