# Quickshell Dusk

[Quickshell](https://quickshell.org) is a Qt Quick toolkit for building desktop
shells, panels, launchers, and widgets on Wayland and X11. **Quickshell Dusk** is an
independent fork focused on reducing QML startup overhead, maintained for
[ii-dusk](https://github.com/totti786/ii-dusk). It keeps the upstream toolkit and
APIs while adding scanner caches and compiled-QML disk caching.

Based on upstream revision
`9b0f339b19057f4731bf3b48293307d7ef241671` (0.3.2 + 11 commits).
This is an independent fork; report fork bugs here rather than to upstream.
Upstream history, licenses, QML APIs, and the `qs` / `quickshell` command names are preserved.
Quickshell and the original toolkit are the work of its upstream contributors.

## Changes

- Cache canonical directory paths and directory membership during scans.
- Serve the config from a symlink mirror as local files so Qt can reuse its compiled QML disk cache.
- Resolve the config root once, pre-register synthesized modules and filter composite singletons per document.
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

## Disk cache and module metadata

Qt only reuses its compiled-QML cache for local files, and Quickshell serves the
config on a `qs:` scheme. The config is therefore mirrored as symlinks (plus the
synthesized `qmldir` files) under the shell's runtime VFS directory and loaded from
`file://`. Nothing is written to your config directory. `QS_NO_FILE_VFS=1` turns
the mirror off.

Three more patches cut the QML loader thread's work: the config root's paths are
resolved once, every synthesized `qs.*` module is registered up front, and each
document only resolves the composite singletons whose names appear in its text
(`QS_NO_SINGLETON_FILTER=1` turns that off; it needs a Qt that calls
`resolvedCompositeSingletons()` through the PLT). The patches come from the
ii-dusk `quickshell-build` set. Conditional preprocessing, symlink aliases and
singleton schema reloads have regression checks under `tests/startup`.

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
