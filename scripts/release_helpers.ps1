# 内部发行辅助：唯一清单、双模式视图和当次原子发布；由 build.ps1 调用。
Set-StrictMode -Version Latest

# 校验文件操作目标位于指定根目录之下，限制递归清理范围。
# 入参：Root 为允许的根目录；Candidate 为待验证的目标路径。
# 返回：规范化的绝对目标路径；目标不属于根目录子路径时抛出异常。
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

# 将文本按 UTF-8 写入，无依赖默认 PowerShell 版本编码。
# 入参：Path 为文件；Text 为内容。
# 返回：无。
function Write-ReleaseText {
    param([string]$Path, [string]$Text)
    [IO.File]::WriteAllText($Path, $Text.Replace("`r`n", "`n").Replace("`n", "`r`n"), [Text.UTF8Encoding]::new($false))
}

# 在已验证根目录内计算相对路径，兼容 Windows PowerShell 5.1 的 .NET Framework。
# 入参：Root 为允许根；Path 为其下已有文件。
# 返回：不含根目录的相对路径。
function Get-ReleaseRelativePath {
    param([string]$Root, [string]$Path)
    $verified = Get-VerifiedChildPath -Root $Root -Candidate $Path
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    return $verified.Substring($prefix.Length)
}

# 验证目录及已有祖先均非重解析点，避免清理/复制绕出工作区。
# 入参：Path 为待使用的绝对路径。
# 返回：无；发现重解析点抛出错误。
function Assert-ReleasePath {
    param([string]$Path)
    $cursor = [IO.Path]::GetFullPath($Path)
    while (-not [string]::IsNullOrEmpty($cursor)) {
        if (Test-Path -LiteralPath $cursor) {
            if ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) {
                throw "发行路径不允许重解析点：$cursor"
            }
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
}

# 发现已安装的工具，普通构建不下载或安装任何编译器。
# 入参：Explicit 为可选安装位置。
# 返回：Visual Studio 安装根。
function Find-ReleaseVisualStudio {
    param([string]$Explicit)
    if ($Explicit) { return [IO.Path]::GetFullPath($Explicit) }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (-not (Test-Path -LiteralPath $vswhere)) { throw '找不到 vswhere.exe，请使用 -VisualStudioPath 指定 VS 安装目录。' }
    $locations = @(& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    if ($LASTEXITCODE -ne 0 -or $locations.Count -eq 0) { throw '找不到具备 MSVC x64 工具的 Visual Studio。' }
    return $locations[0]
}

# 提前校验安装器和固定运行库，失败时不进行昂贵产品构建。
# 入参：Repository 为仓库；Compiler、Redist 为可选显式工具；VisualStudio 为发现结果。
# 返回：已校验工具信息。
function Resolve-InstallerInputs {
    param([string]$Repository, [string]$Compiler, [string]$Redist, [string]$VisualStudio)
    $pin = Get-Content -LiteralPath (Join-Path $Repository 'packaging/windows/toolchain.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not $Compiler) {
        $command = Get-Command ISCC.exe -ErrorAction SilentlyContinue
        $candidates = @()
        if ($command) { $candidates += $command.Source }
        $candidates += (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6/ISCC.exe')
        $candidates += (Join-Path $env:LOCALAPPDATA 'Programs/Inno Setup 6/ISCC.exe')
        $Compiler = $candidates | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
    }
    if (-not $Compiler -or -not (Test-Path -LiteralPath $Compiler -PathType Leaf)) {
        throw '缺少 Inno Setup 编译器。请自行准备后传入 -InnoSetupCompiler；构建不会自动安装工具。'
    }
    $compilerVersion = [version]$pin.innoSetupVersion
    $encodedVersion = ($compilerVersion.Major -shl 24) -bor ($compilerVersion.Minor -shl 16) -bor ($compilerVersion.Build -shl 8)
    $compilerSignature = Get-AuthenticodeSignature -LiteralPath $Compiler
    if ($compilerSignature.Status -ne 'Valid' -or $compilerSignature.SignerCertificate.Subject -notmatch '(^|, )O=Pyrsys B.V.(,|$)') {
        throw 'Inno Setup 编译器签名校验失败。'
    }
    & $Compiler /Q "/DExpectedCompilerVersion=$encodedVersion" (Join-Path $Repository 'packaging/windows/compiler-version.iss')
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup 实际引擎版本不满足固定版本 $($pin.innoSetupVersion)。" }
    if (-not $Redist) {
        $Redist = Get-ChildItem -LiteralPath (Join-Path $VisualStudio 'VC/Redist/MSVC') -Filter vc_redist.x64.exe -File -Recurse |
            Where-Object { $_.VersionInfo.FileVersion -eq $pin.redistributable.version } | Select-Object -First 1 -ExpandProperty FullName
    }
    if (-not $Redist -or -not (Test-Path -LiteralPath $Redist -PathType Leaf)) {
        throw "缺少已固定的 VC++ x64 运行库 $($pin.redistributable.version)，请传入 -VcRedistributable。"
    }
    $signature = Get-AuthenticodeSignature -LiteralPath $Redist
    if ((Get-FileHash -LiteralPath $Redist -Algorithm SHA256).Hash -ne $pin.redistributable.sha256 -or
        (Get-Item -LiteralPath $Redist).VersionInfo.FileVersion -ne $pin.redistributable.version -or
        $signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '(^|, )O=Microsoft Corporation(,|$)') {
        throw 'VC++ 运行库版本、SHA-256 或微软签名校验失败。'
    }
    $toolsetVersion = (Get-Content -LiteralPath (Join-Path $VisualStudio 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt') -Raw -Encoding UTF8).Trim()
    if ([version]$toolsetVersion -gt [version]$pin.redistributable.version) {
        throw "固定运行库 $($pin.redistributable.version) 低于当前 MSVC 工具集 $toolsetVersion；请先更新并审核运行库固定输入。"
    }
    return [pscustomobject]@{ Compiler=[IO.Path]::GetFullPath($Compiler); CompilerVersion=$pin.innoSetupVersion;
        Redist=[IO.Path]::GetFullPath($Redist); RedistVersion=$pin.redistributable.version }
}

# 从唯一清单创建无运行数据的基础 staging，并生成实际文件所有权清单。
# 入参：Repository、Build 为来源；Destination 为全新目录；Dumpbin 为依赖查询工具。
# 返回：生成的发行元数据。
function New-ReleaseStaging {
    param([string]$Repository, [string]$Build, [string]$Destination, [string]$Dumpbin)
    $spec = Get-Content -LiteralPath (Join-Path $Repository 'packaging/windows/release-files.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $launcher = Join-Path $Build 'src/Launcher'
    $entries = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($relative in $spec.launcherFiles) { $entries.Add($relative, (Join-Path $launcher $relative)) }
    foreach ($relative in $spec.repositoryFiles) {
        $target = if ($relative -like 'packaging/*') { [IO.Path]::GetFileName($relative) } else { $relative }
        $entries.Add($target, (Join-Path $Repository $relative))
    }
    foreach ($relative in $spec.repositoryDirectories) {
        foreach ($file in Get-ChildItem -LiteralPath (Join-Path $Repository $relative) -Recurse -File) {
            $entries.Add((Get-ReleaseRelativePath $Repository $file.FullName), $file.FullName)
        }
    }
    # 只遍历从 EXE 可达的 app-local DLL，不把工作树中无关 DLL 混入发行包。
    $pending = [Collections.Generic.Queue[string]]::new()
    $pending.Enqueue((Join-Path $launcher 'Open-ST.exe'))
    while ($pending.Count -gt 0) {
        $binary = $pending.Dequeue()
        $lines = @(& $Dumpbin /NOLOGO /DEPENDENTS $binary)
        if ($LASTEXITCODE -ne 0) { throw "读取运行依赖失败：$binary" }
        foreach ($line in $lines) {
            if ($line -match '^\s+([A-Za-z0-9_.+-]+\.dll)\s*$') {
                $name = $Matches[1]
                $source = Join-Path $launcher $name
                if ((Test-Path -LiteralPath $source -PathType Leaf) -and -not $entries.ContainsKey($name)) {
                    $entries.Add($name, $source); $pending.Enqueue($source)
                }
            }
        }
    }
    foreach ($entry in $entries.GetEnumerator()) {
        Assert-ReleasePath $entry.Value
        $target = Get-VerifiedChildPath -Root $Destination -Candidate (Join-Path $Destination $entry.Key)
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force | Out-Null
        Copy-Item -LiteralPath $entry.Value -Destination $target
    }
    $metadata = Get-Content -LiteralPath (Join-Path $Build 'generated/release-metadata.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $commit = (& git -C $Repository rev-parse HEAD) -join ''
    if ($LASTEXITCODE -ne 0) { throw '读取发行提交号失败。' }
    $metadata | Add-Member -NotePropertyName commit -NotePropertyValue $commit
    $metadata | Add-Member -NotePropertyName dirty -NotePropertyValue ([bool](@(& git -C $Repository status --porcelain).Count))
    Write-ReleaseText (Join-Path $Destination 'release-metadata.json') ($metadata | ConvertTo-Json -Depth 8)
    Set-ReleaseDistribution -Directory $Destination -Mode portable
    return $metadata
}

# 从同源 staging 写入该视图的发行模式和清单，绝不修改仓库资源。
# 入参：Directory 为独立视图；Mode 为 installed/portable。
# 返回：无。
function Set-ReleaseDistribution {
    param([string]$Directory, [ValidateSet('installed','portable')][string]$Mode)
    $path = Join-Path $Directory 'resources/default_settings.json'
    $settings = Get-Content -LiteralPath $path -Raw -Encoding UTF8 | ConvertFrom-Json
    $settings.settings | Add-Member -NotePropertyName 'distribution.mode' -NotePropertyValue $Mode -Force
    Write-ReleaseText $path ($settings | ConvertTo-Json -Depth 64)
    $records = @(Get-ChildItem -LiteralPath $Directory -Recurse -File | Where-Object { $_.Name -ne 'release-files.sha256' } |
        Sort-Object FullName | ForEach-Object {
            $relative = (Get-ReleaseRelativePath $Directory $_.FullName).Replace('\','/')
            if ($relative -match '(^|/)(data|logs|testoutput|\.cache)(/|$)' -or $_.Extension -in '.pdb','.part') {
                throw "发行清单含禁止内容：$relative"
            }
            '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $relative
        })
    Write-ReleaseText (Join-Path $Directory 'release-files.sha256') (($records -join "`n") + "`n")
}

# 仅从基础清单复制文件并复核原始哈希，运行后新增的 data 不进入任何发行视图。
# 入参：Source 为基础目录；Destination 为独立视图。
# 返回：无；原始清单文件被修改时立即失败。
function Copy-ReleaseView {
    param([string]$Source, [string]$Destination)
    $manifest = Join-Path $Source 'release-files.sha256'
    foreach ($line in Get-Content -LiteralPath $manifest -Encoding UTF8) {
        if ($line -notmatch '^([a-fA-F0-9]{64})  (.+)$') { throw '基础发行清单格式错误。' }
        $hash = $Matches[1]; $relative = $Matches[2]
        $file = Get-VerifiedChildPath -Root $Source -Candidate (Join-Path $Source $relative)
        Assert-ReleasePath $file
        if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash -ne $hash) { throw "基础发行文件已变化：$relative" }
        $target = Get-VerifiedChildPath -Root $Destination -Candidate (Join-Path $Destination $relative)
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($target)) -Force | Out-Null
        Copy-Item -LiteralPath $file -Destination $target
    }
}

# 一次构建生成独立 ZIP/Setup 视图，成功后仅发布当次请求的完整产物。
# 入参：Repository、Staging、Metadata 为同源产物；Portable/Installer 为用户选择；Tools 为校验工具。
# 返回：无；任何失败均传播且不公布混合的新旧校验清单。
function Publish-ReleaseArtifacts {
    param([string]$Repository, [string]$Staging, $Metadata, [bool]$Portable, [bool]$Installer, $Tools)
    $root = Join-Path $Repository 'artifacts'
    $name = "Open-ST-$($Metadata.version)-win-x64"
    $work = Get-VerifiedChildPath -Root $root -Candidate (Join-Path $root ('.pending-' + [guid]::NewGuid().ToString('N')))
    Assert-ReleasePath $work
    New-Item -ItemType Directory -Path $work -Force | Out-Null
    $outputs = [Collections.Generic.List[string]]::new()
    try {
        if ($Portable) {
            $view = Join-Path $work $name
            New-Item -ItemType Directory -Path $view | Out-Null
            Copy-ReleaseView $Staging $view
            Set-ReleaseDistribution $view portable
            Compress-Archive -LiteralPath $view -DestinationPath (Join-Path $work "$name.zip") -CompressionLevel Optimal
            $outputs.Add("$name.zip")
        }
        if ($Installer) {
            $view = Join-Path $work 'installed'
            New-Item -ItemType Directory -Path $view | Out-Null
            Copy-ReleaseView $Staging $view
            Set-ReleaseDistribution $view installed
            $fileLines = @('[Files]')
            foreach ($file in Get-ChildItem -LiteralPath $view -Recurse -File | Sort-Object FullName) {
                $relative = Get-ReleaseRelativePath $view $file.FullName
                $subdirectory = [IO.Path]::GetDirectoryName($relative)
                $fileLines += 'Source: "' + $file.FullName + '"; DestDir: "{app}\' + $subdirectory + '"; Flags: ignoreversion'
            }
            $include = Join-Path $work 'files.iss'
            Write-ReleaseText $include ($fileLines -join "`n")
            & $Tools.Compiler "/DAppVersion=$($Metadata.version)" "/DGeneratedFiles=$include" "/DOutputPath=$work" "/DRedistPath=$($Tools.Redist)" "/DRedistVersion=$($Tools.RedistVersion)" (Join-Path $Repository 'packaging/windows/setup.iss')
            if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath (Join-Path $work "$name-Setup.exe"))) {
                throw "安装器编译失败，退出码 $LASTEXITCODE。已暂存：$($outputs -join ', ')；未发布本次产物。"
            }
            $outputs.Add("$name-Setup.exe")
        }
        $sums = @($outputs | ForEach-Object { '{0}  {1}' -f (Get-FileHash -LiteralPath (Join-Path $work $_) -Algorithm SHA256).Hash.ToLowerInvariant(), $_ })
        Write-ReleaseText (Join-Path $work 'SHA256SUMS.txt') (($sums -join "`n") + "`n")
        $record = [ordered]@{ version=$Metadata.version; commit=$Metadata.commit; dirty=$Metadata.dirty; compiler=$Metadata.compiler; ocr=$Metadata.ocr; translation=$Metadata.translation; installerTools=$Tools; artifacts=@($outputs) }
        Write-ReleaseText (Join-Path $work 'build-record.json') ($record | ConvertTo-Json -Depth 8)
        $publish = Get-VerifiedChildPath -Root $root -Candidate (Join-Path $root $Metadata.version)
        Assert-ReleasePath $publish
        New-Item -ItemType Directory -Path $publish -Force | Out-Null
        # 先使旧校验清单失效，最后才发布新清单；旧产物不计入当次 hashes。
        $oldSums = Join-Path $publish 'SHA256SUMS.txt'
        if (Test-Path -LiteralPath $oldSums) { Remove-Item -LiteralPath $oldSums -Force }
        foreach ($output in @($outputs) + @('build-record.json')) {
            Move-Item -LiteralPath (Join-Path $work $output) -Destination (Join-Path $publish $output) -Force
        }
        if ($Portable) {
            $directory = Get-VerifiedChildPath -Root $root -Candidate (Join-Path $root $name)
            Assert-ReleasePath $directory
            if (Test-Path -LiteralPath $directory) { Remove-Item -LiteralPath $directory -Recurse -Force }
            Move-Item -LiteralPath (Join-Path $work $name) -Destination $directory
        }
        Move-Item -LiteralPath (Join-Path $work 'SHA256SUMS.txt') -Destination $oldSums
        Write-Host "本次发行产物：$publish；$($outputs -join ', ')。候选包未公开发布。"
    }
    finally {
        Assert-ReleasePath $work
        if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
    }
}
