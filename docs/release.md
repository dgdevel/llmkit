# llmkit release process

This page explains how a version gets tagged, built, packaged, and published. The scripts are the source of truth. This page is the map. Everything runs from the repo root on a machine with `gh` (installed and authenticated with `gh auth login`). Docker is optional. Without docker, the release ships the plain tarball only, with a note.

## the one command

```sh
make release TAG=v1.2.3        # or: tools/release.sh v1.2.3
```

[tools/release.sh](../tools/release.sh) runs these steps in order:

1. Guards. The script refuses to start unless all of these hold: the tag matches `v1.2`/`v1.2.3`, the tag does not exist yet, the working tree is clean (no uncommitted and no untracked files), HEAD is not ahead of the upstream branch, and `gh` is installed and authenticated. Everything fails before anything is built or tagged.
2. Tests. `make check` runs the full selfcheck suite.
3. Build. The binary is rebuilt with the version stamped in. `llmkit version` reports `1.2.3`. The stamp comes from the `VERSION=` make variable and the `#ifndef LLMKIT_VERSION` guard in `src/llmkit.h`.
4. Tarball. The build produces `dist/llmkit-v1.2.3-<os>-<arch>.tar.gz` with the binary, `LICENSE.md`, and `README.md` inside a top-level directory.
5. Distro packages. When docker is up, [tools/package.sh](../tools/package.sh) builds the matrix below. Set `PACKAGES=0` to skip this step. No docker skips with a warning.
6. Checksums. One `dist/checksums-v1.2.3.txt` covers every artifact.
7. Publish. The script creates the annotated tag, pushes the tag, and runs `gh release create` with everything in `dist/` uploaded. The release body comes from [tools/release-notes.sh](../tools/release-notes.sh) via `--notes-file`. It contains a which-file-for-which-system table generated from what is actually in `dist/` (rows appear only for artifacts that were built), followed by the auto-generated changelog (commits since the previous release) fetched through the `generate-notes` api and appended. The changelog is the text that `--generate-notes` produces, below the table.

The tag and the GitHub release only happen at the very end. Any earlier failure leaves nothing behind to clean up.

To fix the text of a release that is already out (say v1.0.0 was published with the bare changelog):

```sh
tools/package.sh v1.0.0        # repopulate dist/ if it is gone
tools/release-notes.sh v1.0.0 > /tmp/n.md
gh release edit v1.0.0 --notes-file /tmp/n.md
```

## distro packages only

```sh
tools/package.sh v1.2.3            # all six targets
tools/package.sh v1.2.3 deb arch   # just these
make packages TAG=v1.2.3           # same, via make
```

| target | container | artifact | installs on |
|---|---|---|---|
| `deb` | `ubuntu:22.04` | `llmkit_1.2.3-1_amd64.deb` | ubuntu 22.04/24.04+, debian 12+ |
| `rpm-fc` | `fedora:41` | `llmkit-1.2.3-1.fc41.x86_64.rpm` | fedora 41+ |
| `rpm-el9` | `rockylinux:9` | `llmkit-1.2.3-1.el9.x86_64.rpm` | rhel / alma / rocky 9+ |
| `arch` | `archlinux:base-devel` | `llmkit-1.2.3-1-x86_64.pkg.tar.zst` | arch, via `pacman -U` |
| `win` | `africanfuture/msys2-base:linux-latest` | `llmkit-1.2.3-windows-x86_64.zip` | windows 10+ 64-bit |
| `mac` | `llmkit-osxcross:11.3` (local build) | `llmkit-1.2.3-macos-x86_64.zip` | macOS 10.13+, rosetta 2 on apple silicon |

Install commands for users: `apt install ./llmkit_..._.deb`,
`dnf install ./llmkit-..._.rpm`, `pacman -U llmkit-..._.pkg.tar.zst`.
The windows zip unzips in place. `llmkit.exe` carries everything it needs.

Each target builds inside the container of its own distro, with the packaging tool of that distro (`dpkg-deb`, `rpmbuild`, `makepkg`). The tool that consumes the package metadata also produces it.

### the glibc rule

A dynamically linked binary runs on the glibc of its build host or newer, never older. Each target therefore builds on the oldest release it must support. The image list above is also the compatibility list. To raise or lower the floor for a format, change the image in `tools/package.sh` (one variable per target). For example, a `deb` build on `debian:bookworm` drops ubuntu 22.04 but keeps debian 12+.

## packaging design

- deps: libcurl only. Packages embed a pinned static `cjson` ([tools/pkg/cjson.sh](../tools/pkg/cjson.sh), currently v1.7.18) and link the system `libcurl.so.4`. The soname of `libcurl.so.4` is stable across every target distro. `libcjson` is not packaged everywhere (el9). libcurl is. The normal `make` build still links the system libcjson.
- windows: fully static, built from source. The cross container (`africanfuture/msys2-base:linux-latest`) is arch linux with the mingw-w64 toolchain and no target curl/cjson. `tools/pkg/win.sh` cross-builds a pinned libcurl (schannel, every optional third-party dependency off: the `.a` needs only `ws2_32`/`crypt32` and friends) and the pinned cjson, then links `llmkit.exe` fully static. No dlls, no ca bundle (schannel uses the windows cert store), windows 10+. The installed curl prefix is cached across runs in `${XDG_CACHE_HOME:-$HOME/.cache}/llmkit-pkg/win` (bind-mounted into the container at `/pkgcache`). A stamp of the curl version, the triplet, and the configure flags decides reuse. Only the first build compiles curl. Later releases copy it. Delete that dir to force a rebuild.
- macOS: cross-compiled with osxcross. The toolchain image (`crazymax/osxcross:11.3`, MacOSX 11.3 SDK, `OSX_VERSION_MIN=10.13`) is `FROM scratch`. It holds the `/osxcross` directory with the `o64-clang` wrappers, the cctools `ld64`/`ar`/`strip`, and the SDK. [tools/pkg/mac.Dockerfile](../tools/pkg/mac.Dockerfile) copies it into a stock ubuntu (`tools/package.sh` builds that as `llmkit-osxcross:11.3` on first use). There `tools/pkg/mac.sh` cross-builds a pinned openssl and a pinned libcurl against it (every optional third-party dependency off) and the pinned cjson exactly like `win`. Then it links `llmkit` against libSystem only. A fully static darwin binary does not exist, so the zip holds the binary, the license, and the readme. CA verification anchors at `/etc/ssl/cert.pem`, the trust store that macOS itself ships (curl dropped its secure-transport backend in 8.22.0, so apple-system TLS via libcurl is not an option). x86_64 only for now (`-mmacosx-version-min=10.13`). Apple silicon runs it via rosetta 2. A lipo universal binary needs an arm64-capable image. The ssl + curl prefixes cache in `${XDG_CACHE_HOME:-$HOME/.cache}/llmkit-pkg/mac` like the windows one.
- versioning. Tag `v1.2.3` gives package versions `1.2.3-1` (deb/arch) and `1.2.3-1.<disttag>` (rpm). The release tarball keeps the leading `v`.
- fail fast. A failed target aborts the whole run. Rerun with the remaining target list, e.g. `tools/package.sh v1.2.3 rpm-el9 arch`.

## adding a target

A new distro is one `case` entry in `tools/package.sh` (image + which script to run). If the format is new, add one script in `tools/pkg/` that follows the existing three: install deps, build static cjson, build with `make VERSION=... CJSON=/tmp/cj/libcjson.a EXTRA_CFLAGS=-I/tmp/cj/include`, verify the stamp, package, and drop the artifact in `dist/`.
A windows/mingw build later slots in the same way. It replaces the host-built plain tarball step or adds to it.
Arch users are better served by an AUR PKGBUILD pointing at the release tarball than by the bundled `.pkg.tar.zst`. The PKGBUILD in `tools/pkg/arch.sh` is most of that work already.

## gotchas already hit

- `ubuntu:22.04` ships without `ca-certificates`: `apt` works, `curl` https does not. The install lists name it explicitly.
- el9 images ship `curl-minimal`, which conflicts with the full `curl` package. `dnf --allowerasing` swaps it.
- The package scripts must not run `make clean`. Clean removes `dist/`, which already holds the release tarball at that point. They `rm -f` the binaries instead.
- The checksums step must skip its own output file.
