# llmkit release process

How a version gets tagged, built, packaged and published. The scripts are
the source of truth; this page is the map. Everything runs from the repo
root on a machine with `gh` (installed and `gh auth login`-ed). Docker is
optional: without it the release ships the plain tarball only, with a note.

## the one command

```sh
make release TAG=v1.2.3        # or: tools/release.sh v1.2.3
```

In order, [tools/release.sh](../tools/release.sh):

1. **guards** - refuse to start unless: the tag matches `v1.2`/`v1.2.3`,
   the tag does not exist yet, the working tree is clean (no uncommitted
   and no untracked files), HEAD is not ahead of the upstream branch,
   `gh` is installed and authenticated. Everything fails before anything
   is built or tagged.
2. **tests** - `make check`, the full selfcheck suite.
3. **build** - the binary is rebuilt with the version stamped in
   (`llmkit version` reports `1.2.3`; the stamp comes from the `VERSION=`
   make variable and the `#ifndef LLMKIT_VERSION` guard in `src/llmkit.h`).
4. **tarball** - `dist/llmkit-v1.2.3-<os>-<arch>.tar.gz` with the binary,
   `LICENSE.md` and `README.md` inside a top-level directory.
5. **distro packages** - when docker is up, [tools/package.sh](../tools/package.sh)
   builds the matrix below (set `PACKAGES=0` to skip; no docker skips with
   a warning).
6. **checksums** - one `dist/checksums-v1.2.3.txt` covering every artifact.
7. **publish** - annotated tag, tag push, `gh release create` with everything
   in `dist/` uploaded. The release body comes from
   [tools/release-notes.sh](../tools/release-notes.sh) via `--notes-file`:
   a which-file-for-which-system table generated from what is actually in
   `dist/` (rows appear only for artifacts that were built), followed by
   the auto-generated changelog (commits since the previous release)
   fetched through the `generate-notes` api and appended - the text
   `--generate-notes` would produce, just below the table.

The tag and the GitHub release only happen at the very end: any earlier
failure leaves nothing behind to clean up.

To fix the text of a release that is already out (say v1.0.0 was published
with the bare changelog):

```sh
tools/package.sh v1.0.0        # repopulate dist/ if it is gone
tools/release-notes.sh v1.0.0 > /tmp/n.md
gh release edit v1.0.0 --notes-file /tmp/n.md
```

## distro packages only

```sh
tools/package.sh v1.2.3            # all four targets
tools/package.sh v1.2.3 deb arch   # just these
make packages TAG=v1.2.3           # same, via make
```

| target | container | artifact | installs on |
|---|---|---|---|
| `deb` | `ubuntu:22.04` | `llmkit_1.2.3-1_amd64.deb` | ubuntu 22.04/24.04+, debian 12+ |
| `rpm-fc` | `fedora:41` | `llmkit-1.2.3-1.fc41.x86_64.rpm` | fedora 41+ |
| `rpm-el9` | `rockylinux:9` | `llmkit-1.2.3-1.el9.x86_64.rpm` | rhel / alma / rocky 9+ |
| `arch` | `archlinux:base-devel` | `llmkit-1.2.3-1-x86_64.pkg.tar.zst` | arch, via `pacman -U` |

Install commands for users: `apt install ./llmkit_..._.deb`,
`dnf install ./llmkit-..._.rpm`, `pacman -U llmkit-..._.pkg.tar.zst`.

Each target builds inside its own distro's container with that distro's
own packaging tool (`dpkg-deb`, `rpmbuild`, `makepkg`), so the package
metadata is produced by the tool that will consume it.

### the glibc rule

A dynamically linked binary runs on the glibc of its build host **or
newer**, never older. That is why each target builds on the *oldest*
release it should support, and why the image list above is also the
compatibility list. To raise or lower the floor for a format, change the
image in `tools/package.sh` (one variable per target) - e.g. building
`deb` on `debian:bookworm` drops ubuntu 22.04 but keeps debian 12+.

## packaging design

- **deps: libcurl only.** Packages embed a pinned static `cjson`
  ([tools/pkg/cjson.sh](../tools/pkg/cjson.sh), currently v1.7.18) and
  link the system `libcurl.so.4`, whose soname is stable across every
  target distro. `libcjson` is not packaged everywhere (el9), libcurl is.
  The normal `make` build still links the system libcjson.
- **versioning.** Tag `v1.2.3` -> package versions `1.2.3-1` (deb/arch) and
  `1.2.3-1.<disttag>` (rpm). The release tarball keeps the leading `v`.
- **fail fast.** A failed target aborts the whole run; rerun with the
  remaining target list, e.g. `tools/package.sh v1.2.3 rpm-el9 arch`.

## adding a target

- a new distro is one `case` entry in `tools/package.sh` (image + which
  script to run) and, if the format is new, one script in `tools/pkg/`
  following the existing three: install deps, build static cjson, build
  with `make VERSION=... CJSON=/tmp/cj/libcjson.a EXTRA_CFLAGS=-I/tmp/cj/include`,
  verify the stamp, package, drop the artifact in `dist/`.
- a windows/mingw build later slots in the same way; it would replace the
  host-built plain tarball step or add to it.
- arch users are better served by an AUR PKGBUILD pointing at the release
  tarball than by the bundled `.pkg.tar.zst`; the PKGBUILD in
  `tools/pkg/arch.sh` is most of that work already.

## gotchas already hit

- `ubuntu:22.04` ships without `ca-certificates`: `apt` works, `curl`
  https does not. The install lists name it explicitly.
- el9 images ship `curl-minimal`, which conflicts with the full `curl`
  package; `dnf --allowerasing` swaps it.
- the package scripts must not run `make clean` - clean removes `dist/`,
  which already holds the release tarball at that point. They `rm -f`
  the binaries instead.
- the checksums step must skip its own output file.
