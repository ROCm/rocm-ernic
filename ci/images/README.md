# CI images

The images CI runs in are defined here and published to GHCR
by `.github/workflows/ci-images.yml`:

| Directory | Image | What it is |
| --- | --- | --- |
| `builder/` | `ghcr.io/rocm/rocm-ernic/ci-builder` | QEMU with the vfio-user client, libvfio-user, and the tools to boot, provision and fetch a guest |

Every VM lane runs in `ci-builder`. `system-tests.yml` calls
`ci-images.yml` first, which publishes the image if this
tree's definition of it isn't published yet, and gives each
lane its reference. So a pull request that changes the
builder runs the lanes in the new image.

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

A published tag is never built or pushed again, so a tag
always names one build. `ensure-image.sh NAME` looks for the
tag in the registry, and only if the registry says it isn't
there does it build the image, run the image's `check.sh`
inside it, and push it. Any other answer from the registry
fails the run rather than risk overwriting a tag.

So a pull request that changes an image's definition builds
and publishes the new image under its new tag, and a run that
changes nothing only looks. Two runs needing the same new
image build it once.

`ci/guest-image.env` pins the builder too, as `BUILDER_IMAGE`
and `BUILDER_TAG`, for anything outside the workflows that
wants it. `image-tag.sh --check builder` fails, and so does
`ci-images.yml`, if it doesn't pin what the tree defines;
after changing the builder, run

```bash
ci/images/image-tag.sh --write builder
```

and commit the result. `tools/check-guest-image-tag.sh`
fails if a builder tag appears anywhere else.

To rebuild an image whose definition hasn't changed, for
Ubuntu's updates, bump `REBUILD` in its `pins.env`.

A pull request from a fork gets a read-only token, so it
can't publish an image; if it changes one, `ci-images.yml`
fails and says so, and a maintainer pushes the branch to
`ROCm/rocm-ernic` instead.

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
checks both scripts against a stand-in for Docker.

## Where it came from

The builder replaces Stephen Bates'
`batesste-ci-images-ubuntu-qemu-libvfio-user`, merging its
`ubuntu-base`, `ubuntu-libvfio-user` and
`ubuntu-qemu-libvfio-user` Dockerfiles from
[batesste-ci-images][ref-ci-images] at 7438e48, the commit
the pinned `20260918.g7438e48` image was built from. It keeps
that image's Ubuntu 24.04 and QEMU v11.1.1, and changes:

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
