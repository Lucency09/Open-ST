# Open-ST

[中文说明](docs/README.md)

Open-ST is a portable Windows screenshot tool. Annotation, offline OCR, and translation are planned.

The project targets Windows 10 22H2 and Windows 11 on x64. It is under active development; the first development version is `0.1.0`.

## Download and use

Download the Windows x64 ZIP from [Releases](https://github.com/Lucency09/Open-ST/releases), extract the whole archive to a writable folder, and run `Open-ST.exe`.
The [Microsoft Visual C++ v14 x64 runtime](https://aka.ms/vc14/vc_redist.x64.exe) is required and is not bundled.

Version 0.1.0 supports multi-monitor selection, clipboard copy, PNG/JPEG saving, HDR capture and SDR output, and Chinese/English/Japanese UI.
Use `Ctrl+Alt+Q` to capture, `Ctrl+C` or `Enter` to copy, `Ctrl+S` to save, and `Esc` or right-click to cancel.
The screenshot toolbar, automatic window selection, annotations, pinned images, OCR, and translation are not available yet.
HDR color and cross-monitor performance still have known limitations; see [release notes](docs/releases/v0.1.0.md).

## Build

Requirements:

- Visual Studio 2022 17.10+
- MSVC v143
- Windows SDK 10.0.22621+
- CMake 3.28+
- PowerShell 5.1 or 7
- vcpkg manifest for the base JSON dependency, optional features, and tests; the Visual Studio bundled vcpkg is used when `VCPKG_ROOT` is unset

**Before building:** `scripts/build.ps1` and `scripts/test.ps1` currently contain hard-coded development-machine paths for Visual Studio tools (including MSVC setup, CMake and Ninja). Check and adjust these paths for your installation before running the commands below; installing the listed prerequisites alone may not be sufficient.

```powershell
.\scripts\build.ps1 -Configuration Debug
.\scripts\build.ps1 -Configuration Release
.\scripts\build.ps1 -Configuration Release -Package
.\scripts\test.ps1
```

Debug keeps its incremental build tree. Release produces a trimmed runnable directory at
`build/Release/`; `-Package` also copies that directory to the versioned `artifacts/` location.

See `docs/development.md` for the agreed product and engineering specification.
