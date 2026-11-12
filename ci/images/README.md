# CI images

The images CI runs in are defined here and published to GHCR
by `.github/workflows/ci-images.yml`:

| Directory | Image | What it is |
| --- | --- | --- |
| `builder/` | `ghcr.io/rocm/rocm-ernic/ci-builder` | QEMU with the vfio-user client, libvfio-user, and the tools to boot, provision and fetch a guest |

Every VM lane runs in `ci-builder`, under the tag its
`container:` `image:` line names. `system-tests.yml` calls
`ci-images.yml` first, which checks that tag is this tree's
and publishes the image if it isn't published yet. So a pull
request that changes the builder runs the lanes in the new
image.

The image's default user is `ci` (uid 1001), not root.
Anything needing root asks for it: the VM lanes, which are
written to run as root, run it with
`options: --user root ...`.

## Tags name contents

An image's tag is a readable prefix from its pins, then the
first 12 hex digits of a SHA-256 over every file that
defines it, for example:

```
ci-builder:qemu11.1.1-vfu.5b9c722-<12 hex digits>
```

The files are those in the image's directory (its
`pins.env` included), `image-tag.sh` and `ensure-image.sh`.
Change any of them and the tag changes. `image-tag.sh NAME`
prints the tag, and `image-tag.sh --ref NAME` the full
reference.

A published tag isn't built or pushed again.
`ensure-image.sh NAME` looks for the tag in the registry,
and only if the registry says it isn't there does it build
the image and run the image's `check.sh` inside it. It then
looks again, and pushes only if the tag still isn't there.
Any other answer from the registry fails the run rather than
risk overwriting a tag.

So a pull request that changes an image's definition builds
and publishes the new image under its new tag, and a run that
changes nothing only looks. Two runs needing the same new
image at once both build it, and the one that finishes second
doesn't push it. Nothing makes the second look and the push
one step, so two runs finishing within seconds could both
push, publishing two checked builds of one definition, one
after the other, under the tag. A shared concurrency group
would prevent that, but GitHub keeps only one waiting job per
group, so a third run would cancel the second's build and
skip its lanes.

The builder's tag is pinned in `ci/guest-image.env`, as
`BUILDER_IMAGE` and `BUILDER_TAG`, and in each lane's
`container:` `image:` line, written out in full because the
security scan (zizmor) can't tell an image named by an
expression is pinned. `image-tag.sh --check builder` fails
if any of them isn't what the tree defines; after changing
the builder, run

```bash
ci/images/image-tag.sh --write builder
```

and commit the result. Without a NAME, `--check` and
`--write` cover every directory here holding a `pins.env`,
and fail on one `image-tag.sh` doesn't know, so a new image
can't go unchecked. `tools/check-guest-image-tag.sh` fails
if a builder tag appears anywhere else, or a workflow's
`image:` line names another.

Both checks run on every pull request, in
`guest-image-pin.yml`, which has no `paths:` filter, after
the tests of the scripts themselves; `ci-images.yml` runs
`image-tag.sh --check` again before it publishes anything.

Every lane names the image in `ghcr.io/rocm/rocm-ernic`,
while `ci-images.yml` publishes to the running repository's
owner. So a run in a fork whose builder isn't already
published by `ROCm/rocm-ernic` publishes it to the fork, and
its lanes, looking in `ROCm`'s registry, fail.

To rebuild an image whose definition hasn't changed, for
Ubuntu's updates, bump `REBUILD` in its `pins.env`.

A pull request from a fork gets a read-only token, so it
can't publish an image; if it changes one, `ci-images.yml`
fails and says so, and a maintainer pushes the branch to
`ROCm/rocm-ernic` instead.

## Pruning

Each tag stays in GHCR as a version of the image's package,
and so does each registry cache a build replaced, untagged.
`.github/workflows/ci-images-prune.yml` deletes the ones
nothing needs every Monday, with `prune-images.sh`.

The images it prunes are those `ci/guest-image.env` names in
`ghcr.io/rocm/rocm-ernic`, as `PREFIX_IMAGE` or
`PREFIX_REPO` with `PREFIX_TAG`; it leaves other packages
alone. It keeps a version if:

- develop's `ci/guest-image.env` pins it, or the running
  branch's does;
- it's one of the 10 newest versions with a tag
  `image-tag.sh` made;
- it's under 30 days old;
- it has any other tag, such as `buildcache`; or
- a kept version needs it: a manifest a kept index lists, or
  an artifact attached to a kept version with `oras attach`,
  found by its subject or, since GHCR has no referrers API,
  by the `sha256-DIGEST` tag oras uses instead.

It deletes everything else: older tags, the cache manifests
`buildcache` no longer names, and whatever only those
needed. A pruned tag that's wanted again, by an old branch
or a pull request open for weeks, is built again by
`ensure-image.sh`, as any tag the registry hasn't got is.
Nothing is deleted until every version of every image has
been read and every manifest's digest checked, so any
failure first deletes nothing, and it never deletes every
version of a package.

Every run lists what it keeps, and why, and what it deletes,
in the step summary. Only runs on develop delete: the weekly
one, or one started by hand with `delete` set. A pull request
changing the script runs its test and lists what it would
delete. To see the list yourself, with a token that can read
packages (`gh auth refresh -s read:packages`):

```bash
ci/images/prune-images.sh ci/guest-image.env
```

Deleting takes the package's Admin role, which the
repository that published it has, so `GITHUB_TOKEN` does.
After a move to another repository, give that repository
Admin in each package's "Manage Actions access" settings.

`tests/test_prune_images.sh` (the `ernic-prune-images`
CTest) checks the script against stand-ins for `gh` and the
registry.

## Changing a pin

Edit the image's `pins.env`; nowhere else holds them. The
builder's `LIBVFIO_USER_REPO` and `LIBVFIO_USER_COMMIT` are
also what every other CI build compiles the server against
(`build-and-test.yml`, `clang-tidy.yml`, `codeql.yml`,
`iwyu.yml`, `nightly.yml`, `release.yml`, `service-test.yml`
and `thread-safety.yml` source the file), so a libvfio-user
bump moves them all.

## Building locally

```bash
ci/images/ensure-image.sh --build-only builder
```

builds and checks the image under its tag, with Docker's
buildx or, failing that, podman, and doesn't touch the
registry. Behind a proxy that re-signs TLS, as on the AMD
network, give the build the proxy's root certificate:

```bash
CI_IMAGES_ROOT_CA=/usr/local/share/ca-certificates/AMD_RootCA.crt \
    ci/images/ensure-image.sh --build-only builder
```

The image then trusts it, so `ensure-image.sh` refuses to
publish one built that way.

`tests/test_ci_images.sh` (the `ernic-ci-images` CTest)
checks both scripts against a stand-in for Docker, and
`tests/test_check_guest_image_tag.sh`
(`ernic-check-guest-image-tag`) checks
`tools/check-guest-image-tag.sh`.

## Where it came from

The builder replaces Stephen Bates'
`batesste-ci-images-ubuntu-qemu-libvfio-user`, merging its
`ubuntu-base`, `ubuntu-libvfio-user` and
`ubuntu-qemu-libvfio-user` Dockerfiles from
[batesste-ci-images][ref-ci-images] at 7438e48, the commit
the pinned `20260918.g7438e48` image was built from. It keeps
that image's QEMU v11.1.1, and changes:

- it's built on Ubuntu 26.04, as the rest of CI is, not
  24.04, so the lanes compile the server with GCC 15;
- libvfio-user is 5b9c722, the commit the rest of CI builds
  against, not 8039244;
- it adds `libibverbs-dev`, `librdmacm-dev`, `iproute2`,
  `sudo` and `file`, which the lanes used to install or
  expect;
- it drops the Docker CLI, buildx and compose, the
  entrypoint and its `batesste` user, and ccache, whose
  cache mount doesn't outlive a CI runner; the registry
  cache does that job;
- it drops the `qemu-tool` CLI, which only building a guest
  needs, so that build will install it from its own pin.

[ref-ci-images]: https://github.com/sbates130272/batesste-ci-images
