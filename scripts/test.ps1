<#
.SYNOPSIS
    使用两个位置参数构建并运行 Open-ST gTest 测试。

.DESCRIPTION
    第一个位置参数是模块名，第二个位置参数是该模块下的 case 名。
    两者都不提供时运行全部测试；只提供模块名时运行该模块全部测试。
    批量测试跳过人工窗口；指定 case 时自动启用，结束后清除环境开关。

    本脚本不调用产品构建入口 build.ps1。测试配置和二进制写入
    testing/testoutput/Debug；GoogleTest installed tree 保存在 .cache/。

.EXAMPLE
    .\scripts\test.ps1

.EXAMPLE
    .\scripts\test.ps1 foundation

.EXAMPLE
    .\scripts\test.ps1 foundation union_rectangles

.EXAMPLE
    .\scripts\test.ps1 foundation GeometryTest.union_rectangles
#>

# 任一配置、编译或测试错误都立即终止，防止失败后继续运行并给出假成功结果。
$ErrorActionPreference = 'Stop'

# 清除当前进程遗留开关，保证批量回归跳过人工窗口。
$env:OPEN_ST_INTERACTIVE_UI_TESTS = $null

# 使用 $args 而不是 param()，是为了保持“模块、case”两个纯位置参数的调用形式。
if ($args.Count -gt 2) {
    throw 'test.ps1 最多接受两个位置参数：模块名和 case 名。'
}

$moduleName = if ($args.Count -ge 1) { [string]$args[0] } else { '' }
$caseName = if ($args.Count -ge 2) { [string]$args[1] } else { '' }

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
$testBuildDirectory = Join-Path $projectRoot 'testing\testoutput\Debug'
$vcpkgInstalledDir = Join-Path $projectRoot '.cache\vcpkg_installed\x64-windows\tests'

# 当前开发机路径与 build.ps1 保持一致；后续支持其他安装位置时应统一改用 vswhere 发现。
$vsRoot = 'D:\Program Files (x86)\Visual Studio\2022\Community'
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

# 测试入口独立配置 CMake，并只启用 vcpkg manifest 中的 tests feature。
$configureArguments = [Collections.Generic.List[string]]::new()
$configureArguments.Add('-S "' + $projectRoot + '"')
$configureArguments.Add('-B "' + $testBuildDirectory + '"')
$configureArguments.Add('-G Ninja')                                  # 当前保留 VS 随附的 Ninja 生成器
$configureArguments.Add('-DCMAKE_BUILD_TYPE=Debug')                  # 测试统一使用带诊断信息的 Debug
$configureArguments.Add('-DCMAKE_TOOLCHAIN_FILE="' + $vcpkgToolchain + '"')
$configureArguments.Add('-DVCPKG_TARGET_TRIPLET=x64-windows')
$configureArguments.Add('-DVCPKG_INSTALLED_DIR="' + $vcpkgInstalledDir + '"')
$configureArguments.Add('-DVCPKG_MANIFEST_FEATURES=tests')           # 只安装 GoogleTest/GoogleMock
$configureArguments.Add('-DOPEN_ST_ALLOW_WARNINGS=OFF')              # 测试同样执行 /W4 /WX
$configureArguments.Add('-DOPEN_ST_BUILD_TESTS=ON')
$configureArguments.Add('-DOPEN_ST_ENABLE_OCR=OFF')
$configureArguments.Add('-DOPEN_ST_ENABLE_TRANSLATION=OFF')

$configureCommand = '"' + $cmake + '" ' + ($configureArguments -join ' ')
$buildCommand = '"' + $cmake + '" --build "' + $testBuildDirectory + '"'
# vcvars、配置和构建必须在同一个 cmd 进程中执行；UTF-8/英文工具输出可避免依赖扫描乱码。
$nativeCommands =
    'chcp 65001 >NUL && set VSLANG=1033 && call "' + $vcvars + '" && ' +
    $configureCommand + ' && ' + $buildCommand

& cmd.exe /d /s /c $nativeCommands
if ($LASTEXITCODE -ne 0) {
    throw "测试构建失败，退出码：$LASTEXITCODE"
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
