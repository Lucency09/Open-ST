# Open-ST

[中文说明](docs/README.md)

Open-ST is a portable Windows screenshot tool. Annotation, offline OCR, and translation are planned.

The project targets Windows 10 22H2 and Windows 11 on x64. The latest release is `0.2.0`; the project is under active development.

## Download and use

Download the Windows x64 ZIP from [Releases](https://github.com/Lucency09/Open-ST/releases), extract the whole archive to a writable folder, and run `Open-ST.exe`.
The [Microsoft Visual C++ v14 x64 runtime](https://aka.ms/vc14/vc_redist.x64.exe) is required and is not bundled.

Version 0.2.0 adds a screenshot toolbar, multiple pinned images, a redesigned settings window, a first-run welcome screen, and optional startup at sign-in. Multi-monitor selection, clipboard copy, PNG/JPEG saving, HDR capture with SDR output, and Chinese/English/Japanese UI remain available.
Use `Ctrl+Alt+Q` to capture, `Ctrl+C` or `Enter` to copy, `Ctrl+S` to save, and `Esc` or right-click to cancel.
Use the toolbar to pin a selection. Click a pinned image before using the wheel to zoom or `Ctrl+wheel` to adjust opacity; use its context menu to copy, save, raise, or close it. Recapturing includes visible pinned images.
Automatic window selection, annotations, OCR, translation, and configurable capture shortcuts are not available yet.
HDR color and cross-monitor performance still have known limitations; see [release notes](docs/releases/v0.2.0.md).

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
