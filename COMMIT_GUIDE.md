# Git Commit Guide for vfu-rdma

## GPG Signing Status

✓ GPG signing has been **disabled** for this repository:
```bash
git config commit.gpgsign false
```

## Available Commit Scripts

### 1. `commit-msix.sh` - MSI-X Only
Commits only the MSI-X implementation changes:
- `src/vfu_pvrdma.c` - MSI-X capability implementation
- `MSI-X_SUCCESS.md` - Documentation

**Usage:**
```bash
./commit-msix.sh
```

### 2. `commit-all.sh` - All Changes (Recommended)
Commits all pending changes in separate logical commits:
1. **Build system fixes** (`meson.build`)
2. **Documentation updates** (`README.md`) 
3. **MSI-X implementation** (`src/vfu_pvrdma.c`, `MSI-X_SUCCESS.md`)

**Usage:**
```bash
./commit-all.sh
```

You'll be prompted for each commit group, allowing you to skip or commit as needed.

## Manual Commit (Alternative)

If you prefer manual control:

```bash
# Commit build fixes
git add meson.build
git commit -m "Fix build configuration"

# Commit documentation
git add README.md  
git commit -m "Update dependencies in README"

# Commit MSI-X implementation
git add src/vfu_pvrdma.c MSI-X_SUCCESS.md
git commit -m "Add MSI-X capability support to vfu_pvrdma"
```

## Current Changes

```
modified:   README.md         - Updated dependencies
modified:   meson.build        - Fixed include paths
modified:   src/vfu_pvrdma.c  - MSI-X implementation
untracked:  MSI-X_SUCCESS.md  - Documentation
```

## After Committing

Push to remote:
```bash
git push origin stephen
```

Or create a pull request to merge into main/dev branch.

## Useful Commands

```bash
# View what will be committed
git status
git diff

# View commit history
git log --oneline -5

# Amend last commit (if you forgot something)
git commit --amend

# Undo last commit (keeps changes)
git reset HEAD~1

# Undo last commit (discards changes)
git reset --hard HEAD~1
```

## Re-enable GPG Signing (Later)

If you want to re-enable GPG signing:
```bash
git config commit.gpgsign true
```
