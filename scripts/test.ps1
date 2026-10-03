<#
.SYNOPSIS
    使用两个位置参数构建并运行 Open-ST gTest 测试。

.DESCRIPTION
    第一个位置参数是模块名，第二个位置参数是该模块下的 case 名。
    两者都不提供时显式构建并运行全部测试；指定模块仅构建该模块测试目标及依赖。
    指定 case 仍构建所属模块全部测试目标，再按原有名称规则精确筛选执行。
    批量测试跳过人工窗口；指定 case 时自动启用，结束后清除环境开关。

    本脚本不调用产品构建入口 build.ps1。测试配置和二进制写入
    testing/testoutput/Debug-OCR-Translation（按顶部功能变量分档）；依赖缓存保存在 .cache/。

.EXAMPLE
    .\scripts\test.ps1

.EXAMPLE
    .\scripts\test.ps1 common

.EXAMPLE
    .\scripts\test.ps1 capture union_rectangles

.EXAMPLE
    .\scripts\test.ps1 capture GeometryTest.union_rectangles
#>

[CmdletBinding()]
param(
    [Parameter(Position = 0)][string]$moduleName = '',
    [Parameter(Position = 1)][string]$caseName = ''
)

# 任一配置、编译或测试错误都立即终止，防止失败后继续运行并给出假成功结果。
$ErrorActionPreference = 'Stop'
# 测试构建配置：默认验证 OCR 和翻译；关闭对应能力时直接改为 $false。
$EnableOcr = $true
$EnableTranslation = $true

# 清除当前进程遗留开关，保证批量回归跳过人工窗口。
$env:OPEN_ST_INTERACTIVE_UI_TESTS = $null

# 产品能力由顶部配置变量控制，命令行保留原有模块、case 两个位置参数。

if ([string]::IsNullOrWhiteSpace($moduleName) -and
    -not [string]::IsNullOrWhiteSpace($caseName)) {
    throw '指定 case 时必须同时指定模块名。'
}
# 限制参数字符集后再拼接 CTest 正则；所有用户输入仍通过 Regex.Escape 转义。
if (-not [string]::IsNullOrWhiteSpace($moduleName) -and
    $moduleName -notmatch '^[a-z][a-z0-9_]*$') {
    throw "模块名格式无效：$moduleName"
}
if (-not [string]::IsNullOrWhiteSpace($caseName) -and
    $caseName -notmatch '^[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)?$') {
    throw "case 名格式无效：$caseName"
}

# 测试拥有独立 CMake cache 和二进制目录，不复用产品 build/。
$projectRoot = Split-Path -Parent $PSScriptRoot
$testProfile = 'Debug'
$dependencyProfile = 'tests'
$manifestFeatures = [Collections.Generic.List[string]]::new()
$manifestFeatures.Add('tests')
if ($EnableOcr) { $testProfile += '-OCR'; $dependencyProfile += '-ocr'; $manifestFeatures.Add('ocr') }
if ($EnableTranslation) { $testProfile += '-Translation'; $dependencyProfile += '-translation'; $manifestFeatures.Add('translation') }
$testBuildDirectory = Join-Path $projectRoot ('testing/testoutput/' + $testProfile)
$vcpkgInstalledDir = Join-Path $projectRoot ('.cache/vcpkg_installed/x64-windows/' + $dependencyProfile)

# 当前开发机路径与 build.ps1 保持一致；后续支持其他安装位置时应统一改用 vswhere 发现。
. (Join-Path $PSScriptRoot 'release_helpers.ps1')
. (Join-Path $PSScriptRoot 'build_environment.ps1')
$vsRoot = Find-ReleaseVisualStudio
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe'
$vcvars = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'

# 显式 VCPKG_ROOT 优先；未设置时使用 Visual Studio 随附的 vcpkg，但依赖版本仍由 manifest 锁定。
$vcpkgRoot = if ([string]::IsNullOrWhiteSpace($env:VCPKG_ROOT)) {
    Join-Path $vsRoot 'VC\vcpkg'
}
else {
    $env:VCPKG_ROOT
}
$vcpkgToolchain = Join-Path $vcpkgRoot 'scripts\buildsystems\vcpkg.cmake'

# 在启动 cmd/vcpkg 前集中校验路径，让安装问题在最接近原因的位置报错。
foreach ($requiredFile in @($cmake, $ctest, $vcvars, $vcpkgToolchain)) {
    if (-not (Test-Path -LiteralPath $requiredFile -PathType Leaf)) {
        throw "找不到测试所需工具：$requiredFile"
    }
}

# installed tree 与生成目录分离，因此重新生成或清理测试产物时可复用第三方依赖。
New-Item -ItemType Directory -Force -Path $vcpkgInstalledDir | Out-Null

# 测试入口独立配置 CMake，显式启用 tests 及所选产品能力。
$configureArguments = [Collections.Generic.List[string]]::new()
$configureArguments.Add('-S "' + $projectRoot + '"')
$configureArguments.Add('-B "' + $testBuildDirectory + '"')
$configureArguments.Add('-G Ninja')                                  # 当前保留 VS 随附的 Ninja 生成器
$configureArguments.Add('-DCMAKE_BUILD_TYPE=Debug')                  # 测试统一使用带诊断信息的 Debug
$configureArguments.Add('-DCMAKE_TOOLCHAIN_FILE="' + $vcpkgToolchain + '"')
$configureArguments.Add('-DVCPKG_TARGET_TRIPLET=x64-windows')
$configureArguments.Add('-DVCPKG_INSTALLED_DIR="' + $vcpkgInstalledDir + '"')
$configureArguments.Add('-DVCPKG_MANIFEST_INSTALL=ON')
$configureArguments.Add('-DVCPKG_MANIFEST_FEATURES="' + ($manifestFeatures -join ';') + '"')
$configureArguments.Add('-DOPEN_ST_ALLOW_WARNINGS=OFF')              # 测试同样执行 /W4 /WX
$configureArguments.Add('-DOPEN_ST_BUILD_TESTS=ON')
$configureArguments.Add('-DOPEN_ST_ENABLE_OCR=' + $(if ($EnableOcr) { 'ON' } else { 'OFF' }))
$configureArguments.Add('-DOPEN_ST_ENABLE_TRANSLATION=' + $(if ($EnableTranslation) { 'ON' } else { 'OFF' }))

# 配置与按模块构建共享同一个工作区缓存作用域，分别加载相同vcvars后检查各自退出码。
Invoke-OpenStBuildEnvironment -Repository $projectRoot -Action {
    $deploymentPowerShell = Get-OpenStVcpkgPowerShell -VcpkgRoot $vcpkgRoot
    $configureArguments.Add('-DZ_VCPKG_PWSH_PATH:FILEPATH="' + $deploymentPowerShell + '"')
    $configureArguments.Add('-DZ_VCPKG_POWERSHELL_PATH:FILEPATH="' + $deploymentPowerShell + '"')
    $configureCommand = '"' + $cmake + '" ' + ($configureArguments -join ' ')
    $nativeEnvironment = 'chcp 65001 >NUL && set VSLANG=1033 && call "' + $vcvars + '" && '
    & cmd.exe /d /s /c ($nativeEnvironment + $configureCommand)
    if ($LASTEXITCODE -ne 0) {
        throw "测试配置失败，退出码：$LASTEXITCODE"
    }

    $buildCommand = '"' + $cmake + '" --build "' + $testBuildDirectory + '"'
    if (-not [string]::IsNullOrWhiteSpace($moduleName)) {
        # 目标数组由CMake在当前功能组合下生成，未知/未启用模块绝不退化为全量构建。
        $mappingPath = Join-Path $testBuildDirectory 'generated/test-module-targets.json'
        $mapping = Get-Content -LiteralPath $mappingPath -Raw -Encoding UTF8 | ConvertFrom-Json
        $schemaProperty = $mapping.PSObject.Properties['schemaVersion']
        $modulesProperty = $mapping.PSObject.Properties['modules']
        if ($null -eq $schemaProperty -or $schemaProperty.Value -ne 1 -or $null -eq $modulesProperty) {
            throw '测试模块目标清单格式无效，请重新配置。'
        }
        $moduleProperty = $modulesProperty.Value.PSObject.Properties[$moduleName]
        if ($null -eq $moduleProperty) {
            throw "未知或当前功能组合未启用的测试模块：$moduleName"
        }
        if ($moduleProperty.Value -isnot [Array] -or $moduleProperty.Value.Count -eq 0) {
            throw "测试模块没有有效目标数组：$moduleName"
        }
        $targets = @($moduleProperty.Value)
        foreach ($target in $targets) {
            if ($target -isnot [string] -or $target -notmatch '^[A-Za-z_][A-Za-z0-9_.+-]*$') {
                throw "测试模块目标名称无效：$moduleName"
            }
        }
        $buildCommand += ' --target "' + ($targets -join '" "') + '"'
        Write-Host "构建测试模块 $moduleName：$($targets -join ', ')"
    }
    & cmd.exe /d /s /c ($nativeEnvironment + $buildCommand)
    if ($LASTEXITCODE -ne 0) {
        throw "测试构建失败，退出码：$LASTEXITCODE"
    }
}

# --output-on-failure 保持成功输出简洁，失败时再展开 gTest 详情；没有发现测试也视为错误。
$ctestArguments = [Collections.Generic.List[string]]::new()
$ctestArguments.Add('--test-dir')
$ctestArguments.Add($testBuildDirectory)
$ctestArguments.Add('--output-on-failure')
$ctestArguments.Add('--no-tests=error')

if ([string]::IsNullOrWhiteSpace($moduleName)) {
    Write-Host '测试范围：全部测试'
}
elseif ([string]::IsNullOrWhiteSpace($caseName)) {
    # gtest_discover_tests 注册名以“模块.”开头，因此锚定前缀即可选中整个模块。
    $ctestArguments.Add('--tests-regex')
    $ctestArguments.Add('^' + [Regex]::Escape($moduleName) + '\.')
    Write-Host "测试范围：模块 $moduleName"
}
else {
    $ctestArguments.Add('--tests-regex')
    if ($caseName.Contains('.')) {
        # 完整 Suite.case 精确匹配；只写 case 时允许脚本跨 suite 查找同名 case。
        $fullCaseName = "$moduleName.$caseName"
        $ctestArguments.Add('^' + [Regex]::Escape($fullCaseName) + '$')
    }
    else {
        $ctestArguments.Add(
            '^' + [Regex]::Escape($moduleName) + '\..*\.' +
            [Regex]::Escape($caseName) + '$')
    }
    Write-Host "测试范围：模块 $moduleName 的 case $caseName"
}

try {
    # 指定 case 时自动启用人工窗口，普通 case 仍正常执行。
    if (-not [string]::IsNullOrWhiteSpace($caseName)) {
        $env:OPEN_ST_INTERACTIVE_UI_TESTS = '1'
    }
    & $ctest @ctestArguments
    if ($LASTEXITCODE -ne 0) {
        throw "测试失败，退出码：$LASTEXITCODE"
    }
}
finally {
    # 成功或失败均清除开关，不恢复调用前的遗留值。
    $env:OPEN_ST_INTERACTIVE_UI_TESTS = $null
}
