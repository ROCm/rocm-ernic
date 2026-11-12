# CI images

The images CI runs in are defined here and published to GHCR
by `.github/workflows/ci-images.yml`:

| Directory | Image | What it is |
| --- | --- | --- |
| `builder/` | `ghcr.io/rocm/rocm-ernic/ci-builder` | QEMU with the vfio-user client, libvfio-user, and the tools to boot, provision and fetch a guest |
| `guest/` | `ghcr.io/rocm/rocm-ernic/guest-ionic` | The guest disk the lanes boot: Ubuntu on the pinned mainline kernel, for the ionic RDMA driver, as an OCI artifact |

Every VM lane runs in `ci-builder`, under the tag its
`container:` `image:` line names, and boots the guest
`ci/guest-image.env` names. `system-tests.yml` calls
`ci-images.yml` first, which checks those tags are this
tree's and publishes each image that isn't published yet. So
a pull request that changes an image runs the lanes with the
new one.

The builder's default user is `ci` (uid 1001), not root.
Anything needing root asks for it: the VM lanes, which are
written to run as root, run it with
`options: --user root ...`.

## Tags name contents

An image's tag is a readable prefix from its pins, then the
first 12 hex digits of a SHA-256 over every file that
defines it, for example:

```
ci-builder:qemu11.1.1-vfu.5b9c722-<12 hex digits>
guest-ionic:resolute-k7.2.6-<12 hex digits>
```

The files are those in the image's directory (its
`pins.env` included), `image-tag.sh` and `ensure-image.sh`.
Change any of them and the tag changes. `image-tag.sh NAME`
prints the tag, and `image-tag.sh --ref NAME` the full
reference.

A published tag isn't built or pushed again.
`ensure-image.sh NAME` looks for the tag in the registry,
and only if the registry says it isn't there does it build
the image and run the image's `check.sh` on it. It then
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
expression is pinned. The guest's is pinned there as
`GUEST_ARTIFACT_REPO` and `GUEST_ARTIFACT_TAG`, beside
`GUEST_ARTIFACT_INFO_TYPE`, `GUEST_ARTIFACT_DISK` and
`GUEST_ARTIFACT_USER`, which `guest/push.sh` and the guest's
pins decide. `image-tag.sh --check NAME` fails if any of
them isn't what the tree defines, and for the guest, if its
`KERNEL_REF` isn't `IONIC_KERNEL_REF` in
`cmake/ErnicKernelModule.cmake`, the ionic sources the lanes
build into it. After changing an image, run

```bash
ci/images/image-tag.sh --write NAME
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
Ubuntu's updates, or the guest's newer cloud image, bump
`REBUILD` in its `pins.env`.

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
`PREFIX_REPO` with `PREFIX_TAG`, so the builder and the
guest; it leaves other packages alone. It keeps a version if:

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

Edit the image's `pins.env`; nowhere else holds them, except
that the guest's `KERNEL_REF` must equal `IONIC_KERNEL_REF`,
so they change together. The
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

The guest builds in the builder, which has what `build.sh`
needs, with KVM, into `build-ci-images/guest/` (or
`CI_IMAGES_OUT`). With rootless podman, TAG being
`BUILDER_TAG` in `ci/guest-image.env`:

```bash
podman run --rm --user root --group-add keep-groups \
    --device /dev/kvm -v "$PWD:/src" -w /src \
    ghcr.io/rocm/rocm-ernic/ci-builder:TAG \
    ci/images/ensure-image.sh --build-only guest
```

Root in the container is you outside it, so it owns the
checkout, as git wants, and `keep-groups` lets it open
`/dev/kvm` if you can. With Docker, whose root isn't you,
also tell git to trust the checkout, with
`-e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory
-e GIT_CONFIG_VALUE_0=/src`. The build takes about 25
minutes, and needs cloud-images.ubuntu.com,
kernel.ubuntu.com and the Ubuntu archive. `CI_IMAGES_ROOT_CA`, a
path in the container, works for it too, and the guest then
trusts the certificate.

`tests/test_ci_images.sh` (the `ernic-ci-images` CTest)
checks both scripts and `guest/push.sh` against stand-ins for
Docker, oras and the guest's scripts, and
`tests/test_check_guest_image_tag.sh`
(`ernic-check-guest-image-tag`) checks
`tools/check-guest-image-tag.sh`.

## The guest

`guest/build.sh` builds the guest in the builder, booting it
under KVM once for each step:

1. Ubuntu's current cloud image for `RELEASE`, checked
   against its `SHA256SUMS`, boots once with a NoCloud seed,
   while cloud-init authorizes the build's key for the
   default user, installs `packages/ionic.txt`, the whole
   list beyond the cloud image's own, and powers it off;
2. a boot checks cloud-init reported no error, and takes
   out unattended-upgrades;
3. a boot installs the `KERNEL_REF` mainline kernel, checked
   against its `CHECKSUMS`, and pins grub to it;
4. `provision/ionic.sh` checks the release's rdma-core is
   `RDMA_CORE_VERSION`, writes the stamp `ernic_guest_setup`
   reads to skip building it, refreshes the PCI IDs, raises
   the memlock limit, keeps ionic out of the initramfs and
   loads `ionic_rdma` at boot;
5. `trim/kernel-only.sh` removes the release's kernel, then
   the disk is compacted;
6. a boot of a throwaway overlay checks what every boot
   relies on, and that every package `packages/ionic.txt`
   names is installed, runs `checks/ionic.sh`, and reads
   what `vm-info.json` records;
7. the seal: the build's key and the host keys are removed,
   the password is locked, sshd refuses passwords, and
   `cloud-init clean` resets cloud-init and the machine ID.

The image then has no way in. Whatever boots it authorizes a
key through a NoCloud seed, as `ci/boot-guest.sh` does, for
`USERNAME`, cloud-init's default user, which keeps its
passwordless sudo, and each boot is a new instance, which
makes host keys of its own. `guest/check.sh` boots the
finished image that way and fails unless only the boot's key
logs in, sudo needs no password, the password is locked and
sshd refuses passwords, the guest made host keys, and
cloud-init finished without an error or a warning.

`guest/push.sh` compresses the disk with zstd and pushes it
by digest, as `application/vnd.rocm-ernic.vm-image.v1`,
attaches `vm-info.json` to that digest, and only then tags
it, so a tag never names a disk without its `vm-info.json`.
GHCR has no referrers API, so oras attaches it under a
`sha256-DIGEST` tag, which `oras discover` finds and pruning
keeps. `vm-info.json` keeps the schema of the images it
replaced, which the lanes read, as version 10: without the
password and the `ssh_keys` block, with `login`,
`cloud_image`, `rdma_core` and `cloud_init`.

`ci/boot-guest.sh` isn't part of the guest's definition, so
changing it doesn't change the guest's tag.

## Where they came from

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
- it drops the `qemu-tool` CLI, which nothing here uses.

The guest replaces Stephen Bates'
`batesste-ci-images-ubuntu-qcow2-gen-ionic`, built from
`ubuntu-qcow2-gen/` in [batesste-ci-images][ref-ci-images]
at dca6903: `build-vm.sh` became `build.sh`, with
`probe-guest.sh`, `packages/ionic.txt`, `provision/ionic.sh`,
`trim/kernel-only.sh` and `checks/ionic.sh` beside it, and
`ci-images-tool.py push-artifact` became `push.sh`. It keeps
most of the pinned image's contents, the `ionic` flavour at
9d2be1e, and changes:

- it has no SSH key or password, published or not, and every
  boot makes its own host keys, where every copy of the old
  one trusted a key whose private half was published, and
  shared its host keys;
- it takes the later provisioning fixes: the memlock limit,
  the refreshed PCI IDs and `ionic_rdma` at boot;
- like the pinned image, and unlike its source's `ionic`
  flavour since e17bb29, it doesn't build the SRQ and P4
  ionic modules in: the lanes build them from this tree;
- it checks that the packages `packages/ionic.txt` names are
  installed, which its source skipped for a trimmed image,
  and that the kernel is `KERNEL_REF`;
- it boots the cloud image with a seed of its own, not with
  qemu-minimal's `qemu-tool gen-vm`, so it depends on nothing
  outside this repository but Ubuntu's: no password is ever
  set, the time zone stays UTC, and the packages are only
  those `packages/ionic.txt` names. Of qemu-minimal's default
  list it keeps what the lanes use or install, such as fio,
  nvme-cli and the guest agent, and drops the rest, such as
  emacs, mutt and gnuplot;
- it checks the downloads: the cloud image against its
  `SHA256SUMS`, from its dated directory, and the kernel
  `.deb`s against their `CHECKSUMS`;
- it drops the apt cache, the guest compiler cache, the
  parent-image adoption and the ROCm, amdgpu, fio and SDXI
  paths, which the `ionic` flavour doesn't use;
- it's built in the builder, not with buildx's
  `security.insecure`, and published only as an artifact,
  not also as a `FROM scratch` image.

[ref-ci-images]: https://github.com/sbates130272/batesste-ci-images
