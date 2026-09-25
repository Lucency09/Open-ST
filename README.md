# Open-ST

[中文说明](docs/README.md)

Open-ST is a Windows screenshot and annotation tool with an installer and a portable ZIP. Offline Chinese, English and Japanese OCR is included, with editable results and fast/best models. Translation remains planned.

The current version is **0.4.0**, targeting Windows 10 22H2 and Windows 11 x64. See the [release notes](docs/releases/v0.4.0.md) for tested paths and known limitations.

## Download and use

Download from [v0.4.0 Releases](https://github.com/Lucency09/Open-ST/releases/tag/v0.4.0):

- **Setup.exe**: current-user or all-users installation and a custom local directory. A verified Microsoft VC++ x64 runtime installer is included and runs only after confirmation when required.
- **ZIP**: extract the complete archive into a writable folder and run Open-ST.exe. The [Microsoft VC++ x64 runtime](https://learn.microsoft.com/cpp/windows/latest-supported-vc-redist) is required separately.
- **SHA256SUMS.txt**: checksums for both packages. Open-ST and Setup are unsigned; checksums are not publisher signatures.

Version 0.4.0 adds window preselection, full annotations, inline text, local erasing, mosaic, undo/redo, configurable shortcuts, installer packaging and updates from the About window. It also fixes the reported cross-monitor selection-drag frame pacing issue.

Default keys: Ctrl+Alt+Q captures, Ctrl+C or Enter copies, Ctrl+S saves, and Esc cancels by layer. Right-click edits an annotation's properties; it no longer cancels capture. Text editing uses Enter for a new line and Ctrl+Enter to finish.

Before upgrading, exit all instances using the target directory and back up data. Installation preserves data by default. To move from an older portable copy to Setup, install into a new directory and migrate data explicitly. For all-users uninstall, other accounts must disable their own startup entries beforehand.

Current-user installation and repair were exercised on Windows 11. Windows 10 and all-users/multi-account scenarios have remaining manual acceptance work. The existing pinned-window redundant-position-request test remains failing; complex annotation cold builds may be slow. See the release notes for details.

## Build

Requirements:

- Visual Studio 2022 17.10+
- MSVC v143
- Windows SDK 10.0.22621+
- CMake 3.28+
- PowerShell 5.1 or 7
- vcpkg manifest for the base JSON dependency, optional features, and tests; the Visual Studio bundled vcpkg is used when `VCPKG_ROOT` is unset

**Before building:** product builds discover Visual Studio through vswhere or -VisualStudioPath. The test script also discovers Visual Studio through vswhere. Installer builds require a prepared, verified Inno Setup compiler and VC++ Redistributable; missing tools produce an error rather than an automatic installation.

```powershell
.\scripts\build.ps1 -Configuration Debug
.\scripts\build.ps1 -Configuration Release
.\scripts\build.ps1 -Configuration Release -Package
.\scripts\build.ps1 -Configuration Release -Package -Installer -InnoSetupCompiler "<path to ISCC.exe>"
.\scripts\test.ps1
.\scripts\test.ps1 ocr
```

OCR is enabled by the fixed `$EnableOcr = $true` setting near the top of the build and test scripts. Edit that variable to disable it; no OCR command-line flag is needed.

Debug keeps its incremental build tree. Release produces build/Release/. -Package adds a ZIP and retains the versioned portable directory; -Installer adds Setup. Packages and SHA256SUMS.txt are written to artifacts/<version>/. These commands do not publish a release.

See `docs/development.md` for the agreed product and engineering specification.
