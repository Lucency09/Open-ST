# 为产品与测试原生构建提供相同的工作区缓存和依赖工具选择，调用结束后恢复宿主环境。
# 依赖同目录 release_helpers.ps1 的路径检查，不读取或修改用户级环境变量。

# 在工作区环境作用域中取得 vcpkg 管理的 PowerShell，避免 DLL 部署沿用宿主缓存路径。
# 入参：VcpkgRoot 为已选择的 vcpkg 工具根；缓存环境须由 Invoke-OpenStBuildEnvironment 设置。
# 返回：已存在且属于工作区 downloads 的 pwsh.exe 绝对路径；获取失败抛异常。
function Get-OpenStVcpkgPowerShell {
    param([Parameter(Mandatory)][string]$VcpkgRoot)

    $vcpkgExecutable = Join-Path $VcpkgRoot 'vcpkg.exe'
    $toolOutput = @(& $vcpkgExecutable fetch powershell-core --x-stderr-status)
    if ($LASTEXITCODE -ne 0 -or $toolOutput.Count -ne 1) {
        throw '无法取得工作区缓存中的 vcpkg PowerShell。'
    }
    $toolPath = Get-VerifiedChildPath -Root $env:VCPKG_DOWNLOADS -Candidate $toolOutput[0].Trim()
    Assert-ReleasePath -Path $toolPath
    if (-not (Test-Path -LiteralPath $toolPath -PathType Leaf)) {
        throw "vcpkg PowerShell 不存在：$toolPath"
    }
    return $toolPath
}

# 在限定作用域内启用工作区 vcpkg 缓存，确保 Codex 和普通终端使用同一批文件。
# 入参：Repository 为仓库根；Action 为同步执行的原生配置/构建动作，失败须抛出异常。
# 返回：透传动作输出；任何失败都恢复调用前的进程环境，不清理已有缓存。
function Invoke-OpenStBuildEnvironment {
    param(
        [Parameter(Mandatory)][string]$Repository,
        [Parameter(Mandatory)][scriptblock]$Action
    )

    $cacheRoot = Get-VerifiedChildPath -Root $Repository -Candidate (Join-Path $Repository '.cache/vcpkg')
    Assert-ReleasePath -Path $cacheRoot
    $downloads = Join-Path $cacheRoot 'downloads'
    $archives = Join-Path $cacheRoot 'archives'
    $registries = Join-Path $cacheRoot 'registries'
    # vcpkg 的二进制源使用反引号转义分隔符，不把工作区名称当成额外配置。
    $escape = [string][char]96
    $archiveSource = $archives.Replace($escape, $escape + $escape).Replace(',', $escape + ',').Replace(';', $escape + ';')
    $values = [ordered]@{
        VCPKG_DOWNLOADS = $downloads
        VCPKG_DEFAULT_BINARY_CACHE = $archives
        VCPKG_BINARY_SOURCES = 'clear;files,' + $archiveSource + ',readwrite'
        X_VCPKG_REGISTRIES_CACHE = $registries
        VCPKG_FORCE_DOWNLOADED_BINARIES = '1'
        VCPKG_FORCE_SYSTEM_BINARIES = $null
    }
    $previous = @{}
    foreach ($name in $values.Keys) {
        $previous[$name] = [Environment]::GetEnvironmentVariable($name, 'Process')
    }
    try {
        foreach ($directory in @($downloads, $archives, $registries)) {
            Assert-ReleasePath -Path $directory
            New-Item -ItemType Directory -Path $directory -Force -ErrorAction Stop | Out-Null
        }
        foreach ($name in $values.Keys) {
            $value = if ($null -eq $values[$name]) { [NullString]::Value } else { $values[$name] }
            [Environment]::SetEnvironmentVariable($name, $value, 'Process')
        }
        Write-Host "vcpkg 工作区缓存：$cacheRoot"
        Write-Host 'vcpkg 自管工具：按当前工具清单复用 downloads/tools；项目编译工具仍来自所选 VS。'
        & $Action
    }
    finally {
        foreach ($name in $values.Keys) {
            $value = if ($null -eq $previous[$name]) { [NullString]::Value } else { $previous[$name] }
            [Environment]::SetEnvironmentVariable($name, $value, 'Process')
        }
    }
}
