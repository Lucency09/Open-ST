#Requires -Version 5.1
# 验证发行脚本的模式隔离、当次校验清单和污染排除，不安装程序或运行系统设置。
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../..'))
. (Join-Path $repository 'scripts/release_helpers.ps1')
$testRoot = Get-VerifiedChildPath -Root (Join-Path $repository 'testing/testoutput') -Candidate (Join-Path $repository ('testing/testoutput/release-' + [guid]::NewGuid().ToString('N')))
New-Item -ItemType Directory -Path $testRoot | Out-Null

# 提供固定依赖查询结果，验证只收录 EXE 可达的 DLL。
# 入参：自动收集脚本调用参数。
# 返回：与 dumpbin 相同形态的依赖行。
function Invoke-FixtureDumpbin {
    $global:LASTEXITCODE = 0
    '    required.dll'
}

# 模拟安装器编译失败，验证组合产物不会发布一半。
# 入参：自动收集工具参数。
# 返回：失败退出码。
function Invoke-FailingCompiler {
    $global:LASTEXITCODE = 1
}

try {
    $build = Join-Path $testRoot 'build'
    $launcher = Join-Path $build 'src/Launcher'
    New-Item -ItemType Directory -Path (Join-Path $launcher 'resources') -Force | Out-Null
    New-Item -ItemType Directory -Path (Join-Path $build 'generated') -Force | Out-Null
    foreach ($name in @('Open-ST.exe','required.dll','unrelated.dll')) { Write-ReleaseText (Join-Path $launcher $name) 'fixture' }
    foreach ($name in @('ui_text.json','default_settings.json','setting_windows.json')) {
        Copy-Item -LiteralPath (Join-Path $repository "resources/$name") -Destination (Join-Path $launcher "resources/$name")
    }
    New-Item -ItemType Directory -Path (Join-Path $launcher 'data') | Out-Null
    Write-ReleaseText (Join-Path $launcher 'data/secret.json') '{"secret":"never package"}'
    Write-ReleaseText (Join-Path $build 'generated/release-metadata.json') '{"version":"0.3.0","architecture":"x64","compiler":"fixture","ocr":"OFF","translation":"OFF"}'
    $staging = Join-Path $testRoot 'base'
    $metadata = New-ReleaseStaging -Repository $repository -Build $build -Destination $staging -Dumpbin Invoke-FixtureDumpbin
    if ((Test-Path -LiteralPath (Join-Path $staging 'data')) -or (Test-Path -LiteralPath (Join-Path $staging 'unrelated.dll'))) { throw '污染文件进入 staging。' }
    if (-not (Test-Path -LiteralPath (Join-Path $staging 'required.dll'))) { throw '实际 DLL 依赖丢失。' }
    $installed = Join-Path $testRoot 'installed'
    New-Item -ItemType Directory -Path $installed | Out-Null
    Copy-ReleaseView $staging $installed
    Set-ReleaseDistribution $installed installed
    $portableMode = (Get-Content -LiteralPath (Join-Path $staging 'resources/default_settings.json') -Raw | ConvertFrom-Json).settings.'distribution.mode'
    $installedMode = (Get-Content -LiteralPath (Join-Path $installed 'resources/default_settings.json') -Raw | ConvertFrom-Json).settings.'distribution.mode'
    if ($portableMode -ne 'portable' -or $installedMode -ne 'installed') { throw '发行模式串写。' }
    foreach ($view in @($staging,$installed)) {
        foreach ($line in Get-Content -LiteralPath (Join-Path $view 'release-files.sha256')) {
            if ($line.Length -lt 67) { throw '清单行无效。' }
            if ((Get-FileHash -LiteralPath (Join-Path $view $line.Substring(66)) -Algorithm SHA256).Hash -ne $line.Substring(0,64)) { throw '文件清单校验错误。' }
        }
    }
    New-Item -ItemType Directory -Path (Join-Path $staging 'data') | Out-Null
    Write-ReleaseText (Join-Path $staging 'data/after-build-secret.json') '{"secret":"never copy"}'
    # 独立测试仓库只输出至 testing/testoutput，确保不污染真实 artifacts。
    Publish-ReleaseArtifacts -Repository $testRoot -Staging $staging -Metadata $metadata -Portable $true -Installer $false -Tools $null
    $published = Join-Path $testRoot 'artifacts/0.3.0'
    Write-ReleaseText (Join-Path $published 'old-Setup.exe') 'historical'
    Publish-ReleaseArtifacts -Repository $testRoot -Staging $staging -Metadata $metadata -Portable $true -Installer $false -Tools $null
    $sums = @(Get-Content -LiteralPath (Join-Path $published 'SHA256SUMS.txt'))
    if ($sums.Count -ne 1 -or $sums[0] -match 'old-Setup') { throw '历史产物被混入当次校验清单。' }
    $zip = Join-Path $published 'Open-ST-0.3.0-win-x64.zip'
    if ((Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash -ne $sums[0].Substring(0,64)) { throw 'ZIP 校验失败。' }
    $expanded = Join-Path $testRoot 'expanded'
    Expand-Archive -LiteralPath $zip -DestinationPath $expanded
    if (Test-Path -LiteralPath (Join-Path $expanded 'Open-ST-0.3.0-win-x64/data')) { throw '构建后新建 data 混入 ZIP。' }
    $packagedMode = (Get-Content -LiteralPath (Join-Path $expanded 'Open-ST-0.3.0-win-x64/resources/default_settings.json') -Raw -Encoding UTF8 | ConvertFrom-Json).settings.'distribution.mode'
    if ($packagedMode -ne 'portable') { throw 'ZIP 内发行模式错误。' }
    $beforeSums = (Get-FileHash -LiteralPath (Join-Path $published 'SHA256SUMS.txt') -Algorithm SHA256).Hash
    $failed = $false
    try {
        Publish-ReleaseArtifacts -Repository $testRoot -Staging $staging -Metadata $metadata -Portable $true -Installer $true -Tools ([pscustomobject]@{ Compiler='Invoke-FailingCompiler'; Redist='fixture'; RedistVersion='14.44.35211.0' })
    } catch { $failed = $true }
    if (-not $failed -or (Get-FileHash -LiteralPath (Join-Path $published 'SHA256SUMS.txt') -Algorithm SHA256).Hash -ne $beforeSums) { throw '失败构建公布了部分产物。' }
    Write-Host '发行脚本验证通过：依赖清单、污染排除、双模式隔离、文件哈希和当次 ZIP 校验。'
}
finally {
    Assert-ReleasePath $testRoot
    Remove-Item -LiteralPath $testRoot -Recurse -Force
}
