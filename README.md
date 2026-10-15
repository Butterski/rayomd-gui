# RayoMD GUI

The desktop app of [RayoMD](https://github.com/Butterski/rayomd): write Markdown on the left,
see the PDF on the right as you type, export it. It is made for
[Omarchy](https://omarchy.org) and follows its theme live, and runs on any Linux desktop with
Wayland and on Windows.

It is built on its own small toolkit, with no Qt, GTK or browser engine: on Linux the program
needs only the C library, on Windows only system DLLs. The preview is the PDF itself, drawn by
the app's own PDF rasterizer from the bytes the engine just made in memory.

This repository is private while the app is prepared for its first release.

## Using it

```sh
rayomd-gui [file.md | file.pdf]
```

- The editor is on the left, the PDF on the right, rebuilt as you type: a keystroke goes to the
  engine in its own frame, with no waiting timer, and what you type while a build runs goes
  into the next one. A reversible RayoMD PDF opens as its embedded Markdown source.
- The footer holds Open (`Ctrl+O`), Save (`Ctrl+S`, `Ctrl+Shift+S` to save as), Export PDF
  (`Ctrl+E`, `Ctrl+Shift+E` to choose the file) and Options (`Ctrl+,`): style, margins, page
  size, PDF theme file, table of contents, page numbers, code highlighting, PDF/A-3b, PDF/UA-1,
  compression, embedded source, and the interface theme. `Ctrl+=`, `Ctrl+-` and `Ctrl+0` change
  the editor's text size, `Ctrl+wheel` over the preview its zoom.
- On Omarchy the interface follows the current theme and font as they change. Elsewhere it
  follows the system's light or dark mode with Omarchy's bundled themes, or the one you pick.
- File dialogs are the system's (the desktop portal on Linux, the common dialogs on Windows).
- `rayomd-gui --snapshot out.png file.md` writes the first complete frame to a PNG and exits;
  the end-to-end tests use it.

URL images are not fetched on Linux (the build links no libcurl); they show their alt text.

## Building

The engine is the `external/rayomd` submodule; clone with `--recursive`, or run
`git submodule update --init` after cloning. Engine changes go to the `rayomd` repository
first; the submodule then moves to that commit.

On Linux (GCC or Clang, CMake 3.21 or newer):

```sh
cmake --preset linux-release && cmake --build --preset linux-release && ctest --preset linux-release
```

On Windows, in an MSYS2 UCRT64 shell:

```sh
cmake --preset windows-mingw && cmake --build --preset windows-mingw
build/windows/tests/rayomd-gui-tests.exe
```

`tests/e2e/run_compositor.sh weston|sway` runs the app on a headless compositor and checks its
snapshot; ctest runs both when weston and sway are installed. On Arch, `linux/pkgbuild` builds
a package of the working tree: `cd linux/pkgbuild && makepkg -f`.

## Builds

Every push to `main` runs the Release workflow (`.github/workflows/release.yml`). It builds the
Linux binary on Ubuntu 22.04 (so it runs on glibc 2.34 or newer), the Arch package and the
Windows executable, and publishes them as the `continuous` pre-release, replaced on each push.
A tag `v<VERSION>` publishes the release of that version instead.

## License

Apache License 2.0; see [`LICENSE`](LICENSE). [`NOTICE`](NOTICE) lists the third-party
material, among it the Omarchy theme palettes and icon paths (MIT, `third_party/omarchy/`).
