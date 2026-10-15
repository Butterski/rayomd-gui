# RayoMD GUI

The desktop app of [RayoMD](https://github.com/Butterski/rayomd): write Markdown on the left,
see the PDF on the right as you type, export it. It is made for
[Omarchy](https://omarchy.org) and follows its theme live, and runs on any Linux desktop with
Wayland and on Windows.

It is built on its own small toolkit, with no Qt, GTK or browser engine: on Linux the program
needs only the C library, on Windows only system DLLs. The preview is the PDF itself, drawn by
the app's own PDF rasterizer from the bytes the engine just made in memory.

This repository is private while the app is prepared for its first release.

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

## License

Apache License 2.0; see [`LICENSE`](LICENSE). [`NOTICE`](NOTICE) lists the third-party
material, among it the Omarchy theme palettes and icon paths (MIT, `third_party/omarchy/`).
