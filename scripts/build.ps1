<#
.SYNOPSIS
    配置并构建 Open-ST。

.DESCRIPTION
    脚本负责准备 MSVC x64 环境、调用 CMake 生成 Ninja 工程、通过 vcpkg 恢复基础及可选依赖，
    最后执行编译。Release 会把运行文件整理到 build/Release 并删除中间构建目录；
    该产品构建入口永远不加载 testing/。

.EXAMPLE
    .\scripts\build.ps1 -Configuration Debug

.EXAMPLE
    .\scripts\build.ps1 -Clean

.EXAMPLE
    .\scripts\build.ps1 -Configuration Release -Package

.EXAMPLE
    .\scripts\build.ps1 -Configuration Debug -EnableOcr
#>
[CmdletBinding()]
param(
    # 选择调试构建或发布构建。
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Debug',

    # 只删除对应配置的构建/运行目录，完成后立即退出，不执行构建。
    [switch]$Clean,

    # 临时允许编译警告存在；未指定时项目使用 /WX 将警告视为错误。
    [switch]$AllowWarnings,

    # 启用本地 OCR 功能，同时启用 vcpkg manifest 中的 ocr feature。
    [switch]$EnableOcr,

    # 启用翻译功能，同时启用 vcpkg manifest 中的 translation feature。
    [switch]$EnableTranslation,

    # 把干净的 Release 运行目录再复制到版本化 artifacts 目录。
    [switch]$Package
)

# 任意 PowerShell 错误都立即终止脚本，防止失败后继续打包不完整产物。
$ErrorActionPreference = 'Stop'

# PSScriptRoot 是 scripts/ 所在位置，因此它的父目录就是项目根目录。
$projectRoot = Split-Path -Parent $PSScriptRoot
$buildRoot = Join-Path $projectRoot 'build'
$releaseDir = Join-Path $buildRoot 'Release'
$releaseWorkDir = Join-Path $buildRoot '.release-work'
$releaseStagingDir = Join-Path $buildRoot '.release-staging'
$buildDir = if ($Configuration -eq 'Release') { $releaseWorkDir } else { Join-Path $buildRoot $Configuration }
$vcpkgCacheRoot = Join-Path $projectRoot '.cache\vcpkg_installed\x64-windows'

# 返回经过验证的绝对路径；所有递归清理目标必须严格位于指定根目录下。
function Get-VerifiedChildPath {
    param(
        [Parameter(Mandatory)]
        [string]$Root,

        [Parameter(Mandatory)]
        [string]$Candidate
    )

    $resolvedRoot = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar) +
        [IO.Path]::DirectorySeparatorChar
    $resolvedCandidate = [IO.Path]::GetFullPath($Candidate)
    if (-not $resolvedCandidate.StartsWith($resolvedRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "路径超出预期范围，拒绝操作：$resolvedCandidate"
    }
    return $resolvedCandidate
}

# 把一个目录的直接子项复制到另一个目录，避免用通配符决定文件操作范围。
function Copy-DirectoryContents {
    param(
        [Parameter(Mandatory)]
        [string]$Source,

        [Parameter(Mandatory)]
        [string]$Destination
    )

    foreach ($item in Get-ChildItem -LiteralPath $Source -Force) {
        Copy-Item -LiteralPath $item.FullName -Destination $Destination -Recurse -Force
    }
}

if ($Package -and $Configuration -ne 'Release') {
    throw '-Package 只能与 -Configuration Release 一起使用。'
}

# 当前开发机使用 Visual Studio 自带的 CMake、Ninja 和 MSVC 环境脚本。
# 后续若需要支持不同安装位置，应改为通过 vswhere.exe 自动发现，而不是再增加硬编码路径。
$vsRoot = 'D:\Program Files (x86)\Visual Studio\2022\Community'
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'

# -Clean 是独立的终止型操作，只负责清理当前 Configuration 对应的构建目录。
# 删除前先转换为绝对路径并验证前缀，避免路径拼接错误导致误删其他目录。
if ($Clean) {
    $cleanTargets = if ($Configuration -eq 'Release') {
        @($releaseDir, $releaseWorkDir, $releaseStagingDir)
    }
    else {
        @($buildDir)
    }
    foreach ($cleanTarget in $cleanTargets) {
        $resolvedTarget = Get-VerifiedChildPath -Root $buildRoot -Candidate $cleanTarget
        if (Test-Path -LiteralPath $resolvedTarget) {
            Remove-Item -LiteralPath $resolvedTarget -Recurse -Force
            Write-Host "已清理构建目录：$resolvedTarget"
        }
        else {
            Write-Host "构建目录不存在，无需清理：$resolvedTarget"
        }
    }

    # 明确返回，保证 -Clean 不会继续配置、编译或打包。
    return
}
# 把命令行功能开关映射为 vcpkg.json 中同名的可选 feature。
# nlohmann/json 是本地化与设置文件的基础依赖，不受可选 feature 控制。
$manifestFeatures = [Collections.Generic.List[string]]::new()
if ($EnableOcr) { $manifestFeatures.Add('ocr') }
if ($EnableTranslation) { $manifestFeatures.Add('translation') }

# PowerShell switch 需要转换成 CMake 能识别的 ON/OFF 字符串。
$warnings = if ($AllowWarnings) { 'ON' } else { 'OFF' }
$ocr = if ($EnableOcr) { 'ON' } else { 'OFF' }
$translation = if ($EnableTranslation) { 'ON' } else { 'OFF' }

# 使用参数列表逐项构造 CMake 配置命令，避免把所有路径和选项写成一条难以维护的长字符串。
$configureArguments = [Collections.Generic.List[string]]::new()
$configureArguments.Add('-S "' + $projectRoot + '"')                 # 源码根目录
$configureArguments.Add('-B "' + $buildDir + '"')                    # 生成文件和编译产物目录
$configureArguments.Add('-G Ninja')                                   # 使用 VS 自带的 Ninja 生成器
$configureArguments.Add('-DCMAKE_BUILD_TYPE=' + $Configuration)       # Ninja 是单配置生成器，需在配置时指定类型
$configureArguments.Add('-DOPEN_ST_ALLOW_WARNINGS=' + $warnings)
# 产品构建入口永远关闭测试基础设施。
# testing/ 只能由专用测试入口加载，build.ps1 不提供任何开启测试的参数。
$configureArguments.Add('-DOPEN_ST_BUILD_TESTS=OFF')
$configureArguments.Add('-DOPEN_ST_ENABLE_OCR=' + $ocr)
$configureArguments.Add('-DOPEN_ST_ENABLE_TRANSLATION=' + $translation)

# 基础产品也需要 nlohmann/json，因此始终接入 vcpkg manifest mode。
# 显式 VCPKG_ROOT 优先；未设置时使用 Visual Studio 随附的 vcpkg。
$vcpkgRoot = if ([string]::IsNullOrWhiteSpace($env:VCPKG_ROOT)) {
    Join-Path $vsRoot 'VC\vcpkg'
}
else {
    $env:VCPKG_ROOT
}
$toolchain = Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'
foreach ($requiredFile in @($cmake, $vcvars, $toolchain)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "找不到构建所需工具：$requiredFile"
    }
}

# 按 feature 组合隔离 installed tree；-Clean 只删除 build/，不会重复删除依赖。
$dependencyProfile = if ($manifestFeatures.Count -eq 0) { 'product-base' } else { 'product-' + ($manifestFeatures -join '+') }
$vcpkgInstalledDir = Join-Path $vcpkgCacheRoot $dependencyProfile
New-Item -ItemType Directory -Force -Path $vcpkgInstalledDir | Out-Null
$configureArguments.Add('-DCMAKE_TOOLCHAIN_FILE="' + $toolchain + '"')
$configureArguments.Add('-DVCPKG_TARGET_TRIPLET=x64-windows')
$configureArguments.Add('-DVCPKG_INSTALLED_DIR="' + $vcpkgInstalledDir + '"')
if ($manifestFeatures.Count -gt 0) {
    $configureArguments.Add('-DVCPKG_MANIFEST_FEATURES="' + ($manifestFeatures -join ';') + '"')
}

# 先调用 vcvars64.bat，把 cl.exe、link.exe 和 Windows SDK 路径加入当前 cmd 环境，
# 再在同一个 cmd 进程中连续执行 CMake 配置与构建，确保环境变量不会丢失。
$configure = '"' + $cmake + '" ' + ($configureArguments -join ' ')
$build = '"' + $cmake + '" --build "' + $buildDir + '"'

# PowerShell 7 默认按 UTF-8 接收原生程序输出，但系统 cmd 默认可能仍是代码页 936。
# chcp 65001 统一子进程代码页；VSLANG=1033 固定 MSVC 工具输出为英文，
# 既避免 /showIncludes 文本乱码，也让 CMake/Ninja 在不同系统语言上稳定识别依赖扫描前缀。
$commands = 'chcp 65001 >NUL && set VSLANG=1033 && call "' + $vcvars + '" && ' + $configure + ' && ' + $build
& cmd.exe /d /s /c $commands
if ($LASTEXITCODE -ne 0) {
    throw "构建失败，退出码：$LASTEXITCODE"
}

# Release 的 CMake 工作树不是交付物。先完整准备临时运行目录，成功后再替换旧目录，
# 最后删除 CMakeFiles、Ninja 元数据、静态库、PDB 等全部中间产物。
if ($Configuration -eq 'Release') {
    $verifiedReleaseDir = Get-VerifiedChildPath -Root $buildRoot -Candidate $releaseDir
    $verifiedWorkDir = Get-VerifiedChildPath -Root $buildRoot -Candidate $releaseWorkDir
    $verifiedStagingDir = Get-VerifiedChildPath -Root $buildRoot -Candidate $releaseStagingDir
    if (Test-Path -LiteralPath $verifiedStagingDir) {
        Remove-Item -LiteralPath $verifiedStagingDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $verifiedStagingDir | Out-Null

    try {
        $launcherBuildDir = Join-Path $verifiedWorkDir 'src\Launcher'
        $executable = Join-Path $launcherBuildDir 'Open-ST.exe'
        $builtResources = Join-Path $launcherBuildDir 'resources'
        foreach ($requiredPath in @($executable, $builtResources)) {
            if (-not (Test-Path -LiteralPath $requiredPath)) {
                throw "Release 运行文件不完整：$requiredPath"
            }
        }

        Copy-Item -LiteralPath $executable -Destination $verifiedStagingDir
        foreach ($runtimeLibrary in Get-ChildItem -LiteralPath $launcherBuildDir -File -Filter '*.dll') {
            Copy-Item -LiteralPath $runtimeLibrary.FullName -Destination $verifiedStagingDir
        }
        Copy-Item -LiteralPath $builtResources -Destination $verifiedStagingDir -Recurse
        Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE.txt') -Destination $verifiedStagingDir
        Copy-Item -LiteralPath (Join-Path $projectRoot 'THIRD_PARTY_NOTICES.txt') -Destination $verifiedStagingDir
        Copy-Item -LiteralPath (Join-Path $projectRoot 'licenses') -Destination $verifiedStagingDir -Recurse

        if (Test-Path -LiteralPath $verifiedReleaseDir) {
            Remove-Item -LiteralPath $verifiedReleaseDir -Recurse -Force
        }
        Move-Item -LiteralPath $verifiedStagingDir -Destination $verifiedReleaseDir
        Remove-Item -LiteralPath $verifiedWorkDir -Recurse -Force

        $releaseBytes = (Get-ChildItem -LiteralPath $verifiedReleaseDir -Recurse -File |
            Measure-Object -Property Length -Sum).Sum
        Write-Host "Release 可运行环境已整理到：$verifiedReleaseDir（$releaseBytes 字节）"
    }
    catch {
        if (Test-Path -LiteralPath $verifiedStagingDir) {
            Remove-Item -LiteralPath $verifiedStagingDir -Recurse -Force
        }
        throw
    }
}

# -Package 基于已经修剪过的 Release 目录生成版本化交付目录，不复制任何编译工作树。
if ($Package) {
    $artifactDir = Join-Path $projectRoot 'artifacts\Open-ST-0.1.0-win-x64'
    $artifactRoot = Join-Path $projectRoot 'artifacts'
    $verifiedArtifactDir = Get-VerifiedChildPath -Root $artifactRoot -Candidate $artifactDir
    if (Test-Path -LiteralPath $verifiedArtifactDir) {
        Remove-Item -LiteralPath $verifiedArtifactDir -Recurse -Force
    }
    New-Item -ItemType Directory -Path $verifiedArtifactDir | Out-Null
    Copy-DirectoryContents -Source $releaseDir -Destination $verifiedArtifactDir
    Write-Host "版本化发布目录已整理到：$verifiedArtifactDir"
}
