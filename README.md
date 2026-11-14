# Quickshell Dusk

A startup-performance fork of [Quickshell](https://quickshell.org), maintained for
[ii-dusk](https://github.com/totti786/ii-dusk). Based on upstream revision
`9b0f339b19057f4731bf3b48293307d7ef241671` (0.3.2 + 11 commits).
This is an independent fork; report fork bugs here rather than to upstream.
Upstream history, licenses, QML APIs, and the `qs` / `quickshell` command names are preserved.

## Changes

- Cache canonical directory paths and directory membership during scans.
- Serve eligible QML as local files so Qt can reuse its compiled QML disk cache.
- Materialize generated `qmldir` metadata, rewriting it only when its content changes.
- Avoid repeated QML header parsing and import metadata lookups.
- Route crash reports to this fork's issue tracker.

These optimizations do not remove the startup scan. The ii-dusk config separately
stages wallpaper, bar and Dynamic Island, other panels/services, then widgets.

## Arch Linux

```sh
yay -S quickshell-dusk-git
```

The AUR recipe is in `packaging/arch`. It builds the `dusk` branch and conflicts
with stock Quickshell because it installs the same executables and QML modules.
It provides `quickshell` for dependent packages. Rebuild after incompatible Qt
updates; Quickshell uses private Qt APIs.

```sh
cd packaging/arch
makepkg -si
```

See [BUILD.md](BUILD.md) for direct CMake builds and dependencies. Fork builds
should set `-DDISTRIBUTOR="Quickshell Dusk (local build)"`; the AUR recipe identifies
itself automatically. Inherited upstream CI workflows are available for manual runs.

## Generated module metadata

The scanner creates real `qmldir` files in writable config directories where there
is no authored module metadata. Generated files end with:

```text
# quickshell-generated qmldir
```

They are runtime artifacts: exclude their exact paths from your config repository,
while keeping authored module files tracked. ii-dusk already ignores its current
218 generated paths. Add ignore entries when introducing new generated directories.
Source changes are scanned again and generated content is refreshed as needed.

Do not delete this metadata as a runtime cleanup step. Simply suppressing writes
while retaining the file-backed loader breaks some implicit sibling-type imports.
Read-only configs retain virtual metadata for explicit imports, but implicit sibling
inheritance is a known limitation of the current fallback; writable configs are the
supported fast path. Symlink aliases, conditional preprocessing, and singleton schema
reloads have focused regression checks under `tests/startup`.

## Measurements

Warm-cache ii-dusk session restarts on one development machine, median of three:

| Milestone | File-backed loader | Virtual loader without generated files |
|---|---:|---:|
| Wallpaper painted | 1.256 s | 2.096 s |
| Dynamic Island painted | 2.080 s | 4.094 s |
| Bar painted | 2.166 s | 4.221 s |
| Desktop widgets begin loading | 4.956 s | 9.365 s |

These compare the two working loading paths, not the isolated cost of files.
They are not cold-boot results or a general performance guarantee. The virtual
comparison used a temporary test build and is not a user-facing runtime option.

## Development

```sh
for test in tests/startup/check-*.py; do
    python3 "$test" ./build/src/quickshell
done
```

The checks use the offscreen Qt platform by default and do not restart your desktop.
Read-only checks must run as an unprivileged user. The original upstream README is
preserved as [README.upstream.md](README.upstream.md). See [CONTRIBUTING.md](CONTRIBUTING.md)
and [HACKING.md](HACKING.md) for upstream development guidance.
