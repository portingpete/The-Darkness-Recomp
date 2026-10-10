# Windows PowerShell 5.1. Update to the exact official main commit, build and
# test locally, then launch only after success. Source checkouts fast-forward;
# portable installs use a retained external clone and transactional installation.
# Exit: 0 current/declined/installed; 2 update available; 10 network unavailable;
# 20 unsafe checkout, missing prerequisites, or failed build/installation.
[CmdletBinding()]
param(
    [string]$InstallRoot = '',
    [string]$GameDirectory = '',
    [string]$SourceRoot = '',
    [switch]$CheckOnly,
    [switch]$AcceptUpdate,
    [switch]$NonInteractive,
    [switch]$LibraryOnly
)

$ErrorActionPreference = 'Stop'
if (!$InstallRoot -and $PSScriptRoot) { $InstallRoot = Split-Path -Parent $PSScriptRoot }
if (!$InstallRoot) { throw 'InstallRoot is required when running the updater in memory.' }
# Resolve against InstallRoot because this script can be loaded into memory by
# the CMD before any update replaces either updater on disk.
$commitLibraryOnly = $LibraryOnly
. (Join-Path $InstallRoot 'tools/update_release.ps1') -InstallRoot $InstallRoot -CheckOnly:$CheckOnly -AcceptUpdate:$AcceptUpdate -NonInteractive:$NonInteractive -LibraryOnly
$LibraryOnly = $commitLibraryOnly

function Get-OfficialMainCommit {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $uri = "https://api.github.com/repos/$script:ReleaseRepository/commits/main"
    $result = Invoke-RestMethod -Uri $uri -Headers @{
        'User-Agent' = 'The-Darkness-Recomp-Commit-Updater'; 'Accept' = 'application/vnd.github+json'
    } -TimeoutSec 20
    if ($result.sha -isnot [string] -or $result.sha -cnotmatch '^[0-9a-f]{40}$') {
        throw 'GitHub did not return a valid main commit.'
    }
    return $result.sha
}

function Invoke-CommitGit([string]$Directory, [string[]]$Arguments) {
    # PS 5.1 represents redirected native stderr as ErrorRecords. Git progress
    # is not a failure; use the actual native exit status.
    $ErrorActionPreference = 'Continue'
    $lines = @(& git -C $Directory @Arguments 2>&1)
    $code = $LASTEXITCODE
    return [pscustomobject]@{ ExitCode = $code; Output = (($lines | ForEach-Object { "$_" }) -join "`n").Trim() }
}

function Invoke-CheckedCommitGit([string]$Directory, [string[]]$Arguments) {
    $result = Invoke-CommitGit $Directory $Arguments
    if ($result.ExitCode -ne 0) { throw "Git $($Arguments[0]) failed: $($result.Output)" }
    return $result.Output
}

function Assert-OfficialSourceCheckout([string]$Root) {
    $top = Invoke-CheckedCommitGit $Root @('rev-parse', '--show-toplevel')
    if ([IO.Path]::GetFullPath($top).TrimEnd('\') -ine $Root.TrimEnd('\')) { throw 'Expected a repository rooted at the source directory.' }
    $origin = Invoke-CheckedCommitGit $Root @('remote', 'get-url', 'origin')
    if ($origin -notmatch '^(?:https://github\.com/|git@github\.com:|ssh://git@github\.com/)portingpete/The-Darkness-Recomp(?:\.git)?/?$') {
        throw 'The source checkout origin must be the official portingpete/The-Darkness-Recomp repository.'
    }
    $status = Invoke-CheckedCommitGit $Root @('status', '--porcelain', '--untracked-files=all')
    if ($status) { throw "Source checkout contains local edits or untracked files. Commit or move them before updating; use Launch.cmd to play without updating.`n$status" }
    return (Invoke-CheckedCommitGit $Root @('rev-parse', 'HEAD'))
}

function Get-CommitPythonVersion {
    # No quote characters in the Python code: PowerShell 5.1 otherwise strips
    # Python string-literal quotes while constructing Windows native argv.
    $version = & python -c 'import sys; print(sys.version_info.major,sys.version_info.minor,sep=chr(46))'
    if ($LASTEXITCODE -ne 0 -or "$version" -notmatch '^([0-9]+)\.([0-9]+)$') { throw 'Could not determine the installed Python version.' }
    return "$version"
}

function Assert-CommitCodecPrerequisites([string]$MsysRoot = 'C:\msys64', [string]$LlvmRoot = 'C:\Program Files\LLVM') {
    # build_xma_codec.py currently uses these fixed installations even when
    # ClangCL is supplied separately by Visual Studio.
    $required = @(
        (Join-Path $MsysRoot 'usr/bin/bash.exe'),
        (Join-Path $MsysRoot 'usr/bin/make.exe'),
        (Join-Path $MsysRoot 'mingw64/bin/gcc.exe'),
        (Join-Path $MsysRoot 'mingw64/bin/libwinpthread-1.dll'),
        (Join-Path $MsysRoot 'mingw64/share/licenses/winpthreads/COPYING'),
        (Join-Path $LlvmRoot 'bin/llvm-lib.exe')
    )
    $missing = @($required | Where-Object { !(Test-Path -LiteralPath $_ -PathType Leaf) })
    if ($missing.Count) {
        throw "The audio-codec build requires MSYS2 with make and the MinGW64 GCC/winpthreads packages at C:\msys64, and LLVM at C:\Program Files\LLVM. Install these tools before updating, or use Launch.cmd to play the installed build. Missing files: $($missing -join ', ')"
    }
}

function Assert-CommitBuildPrerequisites {
    foreach ($name in @('git', 'python', 'cmake', 'powershell.exe')) {
        if (!(Get-Command $name -ErrorAction SilentlyContinue)) {
            throw "Latest-commit builds require Git, Python 3.11+, CMake and Visual Studio 2022 with Desktop development with C++ and Clang tools. Missing: $name. Install these tools, or use Launch.cmd to play the installed build."
        }
    }
    $pythonVersion = Get-CommitPythonVersion
    if ([Version]$pythonVersion -lt [Version]'3.11') {
        throw 'Latest-commit builds require Python 3.11 or newer. Use Launch.cmd to play without updating.'
    }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'Install Visual Studio 2022 with Desktop development with C++ and C++ Clang tools; then rerun this launcher.' }
    $installations = @(& $vswhere -version '[17.0,18.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath)
    $clangAvailable = $false
    foreach ($installation in $installations) {
        foreach ($relative in @('VC/Tools/Llvm/bin/clang-cl.exe', 'VC/Tools/Llvm/x64/bin/clang-cl.exe')) {
            if (Test-Path -LiteralPath (Join-Path $installation $relative) -PathType Leaf) { $clangAvailable = $true }
        }
    }
    if (!$clangAvailable) { throw 'Install the Visual Studio 2022 C++ Clang compiler and Clang build tools, then rerun this launcher.' }
    Assert-CommitCodecPrerequisites
}

function Get-SourceProgramPaths([string]$Root) {
    $release = Join-Path $Root 'build_native/Release'
    $paths = @()
    if (Test-Path -LiteralPath $release -PathType Container) {
        foreach ($file in Get-ChildItem -LiteralPath $release -Force -File) {
            $relative = 'build_native/Release/' + $file.Name
            try { Assert-ManagedPath $relative } catch { continue }
            $null = Assert-SafeDestination $Root $relative
            $paths += $relative
        }
    }
    return $paths
}

function Get-SuccessfulBuildCommit([string]$Root) {
    $path = Join-Path $Root 'build_native/commit-update.json'
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { return '' }
    try {
        $item = Get-Item -LiteralPath $path -Force
        if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint -or $item.Length -gt 65536) { return '' }
        $stamp = [IO.File]::ReadAllText($path) | ConvertFrom-Json
        if ($stamp.schema -ne 1 -or $stamp.commit -isnot [string] -or $stamp.commit -cnotmatch '^[0-9a-f]{40}$' -or
            $stamp.sha256 -isnot [pscustomobject]) { return '' }
        foreach ($required in @('build_native/Release/DarkRecomp.exe', 'build_native/Release/DarkRecompPreview.exe')) {
            if (!$stamp.sha256.PSObject.Properties[$required]) { return '' }
        }
        foreach ($entry in $stamp.sha256.PSObject.Properties) {
            if ($entry.Name -cnotmatch '^build_native/Release/' -or $entry.Value -isnot [string] -or $entry.Value -cnotmatch '^[0-9a-f]{64}$') { return '' }
            $file = Assert-SafeDestination $Root $entry.Name
            if (!(Test-Path -LiteralPath $file -PathType Leaf) -or (Get-ReleaseSha256 $file) -cne $entry.Value) { return '' }
        }
        return $stamp.commit
    } catch { return '' }
}

function Test-InstalledCommitFiles([string]$Root, $Manifest) {
    foreach ($entry in $Manifest.sha256.PSObject.Properties) {
        $path = Assert-SafeDestination $Root $entry.Name
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or (Get-ReleaseSha256 $path) -ine $entry.Value) { return $false }
    }
    return $true
}

function Get-CommitPackageVersion([string]$Commit) {
    # Prefix the abbreviated hash so even a leading-zero all-numeric hash is a
    # legal nonnumeric SemVer prerelease identifier.
    return ('v0.0.0-main.g' + $Commit.Substring(0, 12))
}

function Write-SuccessfulBuildCommit([string]$Root, [string]$Commit) {
    $hashes = [ordered]@{}
    foreach ($relative in @(Get-SourceProgramPaths $Root | Sort-Object)) {
        $hashes[$relative] = Get-ReleaseSha256 (Assert-SafeDestination $Root $relative)
    }
    foreach ($required in @('build_native/Release/DarkRecomp.exe', 'build_native/Release/DarkRecompPreview.exe')) {
        if (!$hashes.Contains($required)) { throw "Successful build did not produce $required" }
    }
    $stamp = [ordered]@{ schema = 1; commit = $Commit; sha256 = $hashes } | ConvertTo-Json -Depth 10
    $path = Join-Path $Root 'build_native/commit-update.json'
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
    $temporary = $path + '.' + [Guid]::NewGuid().ToString('N')
    [IO.File]::WriteAllText($temporary, $stamp, (New-Object Text.UTF8Encoding($false)))
    if (Test-Path -LiteralPath $path) { [IO.File]::Replace($temporary, $path, ($path + '.previous-' + [Guid]::NewGuid().ToString('N'))) }
    else { [IO.File]::Move($temporary, $path) }
}

function New-CommitUpdateWork {
    $work = Join-Path ([IO.Path]::GetTempPath()) ('DarkRecomp-commit-update-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($work) | Out-Null
    Assert-NoReparseTree $work
    Write-Host "Build logs, update files and recovery backups are retained at: $work"
    return $work
}

function Invoke-SourceCommitBuild([string]$Root, [string]$Log) {
    Push-Location -LiteralPath $Root
    try {
        & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Root 'tools/build.ps1') *>&1 | Tee-Object -FilePath $Log | Out-Host
        if ($LASTEXITCODE -ne 0) { throw "Build or tests failed. See $Log" }
    } finally { Pop-Location }
}

function Invoke-CommitPackage([string]$Root, [string]$Output, [string]$Version, [string]$Commit) {
    # Fixed Python code; all paths and identifiers are separate argv values.
    $code = 'from pathlib import Path; import sys; from tools.package_release import package, find_crt; r=Path(sys.argv[1]); print(package(r, find_crt(r), Path(sys.argv[2]), sys.argv[3], sys.argv[4]))'
    Push-Location -LiteralPath $Root
    try {
        & python -c $code $Root $Output $Version $Commit | Out-Host
        if ($LASTEXITCODE -ne 0) { throw 'Packaging the locally built commit failed.' }
    } finally { Pop-Location }
    return (Join-Path $Output "The-Darkness-Recomp-$Version-windows-x64.zip")
}

function Sync-OfficialCommit([string]$Root, [string]$Commit, [switch]$Detached) {
    $before = Assert-OfficialSourceCheckout $Root
    $fetch = Invoke-CommitGit $Root @('fetch', '--no-tags', 'origin', 'refs/heads/main')
    if ($fetch.ExitCode -ne 0) { throw "Could not fetch official main: $($fetch.Output)" }
    $null = Invoke-CheckedCommitGit $Root @('cat-file', '-e', ($Commit + '^{commit}'))
    $fetched = Invoke-CheckedCommitGit $Root @('rev-parse', 'FETCH_HEAD')
    $reachable = Invoke-CommitGit $Root @('merge-base', '--is-ancestor', $Commit, $fetched)
    if ($reachable.ExitCode -ne 0) { throw 'The requested GitHub commit is not in the fetched official main history. Retry the update.' }
    if ($Detached) {
        $null = Invoke-CheckedCommitGit $Root @('checkout', '--detach', $Commit)
    } else {
        $forward = Invoke-CommitGit $Root @('merge-base', '--is-ancestor', $before, $Commit)
        if ($forward.ExitCode -ne 0) { throw 'Local commits diverge from official main. Merge or preserve them yourself; this updater never resets or discards commits.' }
        $null = Invoke-CheckedCommitGit $Root @('merge', '--ff-only', $Commit)
    }
    Assert-NoReparseTree $Root
    $after = Assert-OfficialSourceCheckout $Root
    if ($after -cne $Commit) { throw 'Source checkout did not reach the requested main commit.' }
}

function Get-CommitCacheSource([string]$Root, [string]$RequestedSource) {
    if ($RequestedSource) { $source = [IO.Path]::GetFullPath($RequestedSource).TrimEnd('\') }
    else {
        $digest = [Security.Cryptography.SHA256]::Create()
        try { $key = ([BitConverter]::ToString($digest.ComputeHash([Text.Encoding]::UTF8.GetBytes($Root.ToLowerInvariant())))).Replace('-', '').ToLowerInvariant() }
        finally { $digest.Dispose() }
        $source = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) ("TheDarknessRecomp/source-builds/$key/source")
    }
    if ($source -ieq $Root -or $source.StartsWith($Root + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $Root.StartsWith($source + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'The retained build clone must be outside the installation.' }
    $parent = [IO.Path]::GetDirectoryName($source)
    [IO.Directory]::CreateDirectory($parent) | Out-Null
    Assert-NoReparseTree $parent
    $ownerPath = Join-Path $parent 'source-owner.json'
    if (!(Test-Path -LiteralPath $ownerPath)) {
        if (@(Get-ChildItem -LiteralPath $parent -Force).Count) { throw "The source-cache directory is not empty or owned by this updater: $parent" }
        [IO.File]::WriteAllText($ownerPath, ([ordered]@{ schema = 1; installation = $Root; repository = $script:ReleaseRepository } | ConvertTo-Json))
    }
    $ownerItem = Get-Item -LiteralPath $ownerPath -Force
    if ($ownerItem.PSIsContainer -or $ownerItem.Length -gt 4096) { throw 'Invalid source-cache ownership record.' }
    $owner = [IO.File]::ReadAllText($ownerPath) | ConvertFrom-Json
    if ($owner.schema -ne 1 -or $owner.installation -ine $Root -or $owner.repository -cne $script:ReleaseRepository) { throw 'This source cache belongs to a different installation.' }
    if (!(Test-Path -LiteralPath (Join-Path $source '.git'))) {
        if (Test-Path -LiteralPath $source) { throw "Incomplete source clone retained at $source. Preserve or move it, then retry; no files were removed." }
        $clone = Invoke-CommitGit $parent @('clone', '--no-checkout', '--config', 'core.autocrlf=false', '--', "https://github.com/$script:ReleaseRepository.git", $source)
        if ($clone.ExitCode -ne 0) { throw "Could not clone official source. Files retained at $source. $($clone.Output)" }
        # A no-checkout clone has no tracked working tree yet; inspect its origin
        # before materializing any remote content.
        $origin = Invoke-CheckedCommitGit $source @('remote', 'get-url', 'origin')
        if ($origin -cne "https://github.com/$script:ReleaseRepository.git") { throw 'The new clone does not use the expected official origin.' }
        $null = Invoke-CheckedCommitGit $source @('checkout', '--detach', 'origin/main')
    }
    Assert-NoReparseTree $parent
    $null = Assert-OfficialSourceCheckout $source
    return $source
}

function Copy-OwnedBuildGame([string]$Game, [string]$Source) {
    $Game = [IO.Path]::GetFullPath($Game).TrimEnd('\')
    if (!(Test-Path -LiteralPath (Join-Path $Game 'default.xex') -PathType Leaf)) {
        throw "Copy your complete supported game dump into $Game before building, or supply -GameDirectory with its location."
    }
    Assert-NoReparseTree $Game
    Assert-NoReparseTree $Source
    $destinationRoot = Join-Path $Source 'Darkness'
    if ($destinationRoot.StartsWith($Game + '\', [StringComparison]::OrdinalIgnoreCase) -or
        $Game.StartsWith($destinationRoot + '\', [StringComparison]::OrdinalIgnoreCase) -or $Game -ieq $destinationRoot) { throw 'The game input and retained build copy must be separate directories.' }
    [IO.Directory]::CreateDirectory($destinationRoot) | Out-Null
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($Game)
    while ($pending.Count) {
        $directory = $pending.Pop()
        foreach ($item in Get-ChildItem -LiteralPath $directory -Force) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point appeared in the game input: $($item.FullName)" }
            $relative = $item.FullName.Substring($Game.Length + 1)
            # This small translation-analysis file is supplied by the exact
            # source commit, not by the user's extracted disc.
            if ($relative -ieq 'darkness_switch_tables.toml') { continue }
            $destination = Join-Path $destinationRoot $relative
            if (Test-Path -LiteralPath $destination) {
                $existing = Get-Item -LiteralPath $destination -Force
                if ($existing.Attributes -band [IO.FileAttributes]::ReparsePoint -or $existing.PSIsContainer -ne $item.PSIsContainer) { throw "Unsafe build-game destination: $destination" }
            }
            if ($item.PSIsContainer) { [IO.Directory]::CreateDirectory($destination) | Out-Null; $pending.Push($item.FullName) }
            else { [IO.File]::Copy($item.FullName, $destination, $true) }
        }
    }
    Assert-NoReparseTree $Source
    Write-Host "Physical build copy of your game: $destinationRoot (original files preserved)."
}

function Invoke-SourceUpdateBuild([string]$Root, [string]$Commit, [string]$Work) {
    $backup = Join-Path $Work 'source-program-backup'
    [IO.Directory]::CreateDirectory($backup) | Out-Null
    $original = @(Get-SourceProgramPaths $Root)
    foreach ($relative in $original) {
        $to = Assert-SafeDestination $backup $relative
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($to)) | Out-Null
        [IO.File]::Copy((Assert-SafeDestination $Root $relative), $to, $false)
    }
    try {
        Invoke-SourceCommitBuild $Root (Join-Path $Work 'build.log')
        Assert-NoReparseTree $Root
        if ((Assert-OfficialSourceCheckout $Root) -cne $Commit) { throw 'Source checkout changed during the build; refusing to mark it successful.' }
        Write-SuccessfulBuildCommit $Root $Commit
    } catch {
        $buildError = $_.Exception.Message
        try {
            Assert-NoReparseTree $Root
            foreach ($relative in @(Get-SourceProgramPaths $Root)) {
                if ($original -cnotcontains $relative) { Remove-Item -LiteralPath (Assert-SafeDestination $Root $relative) -Force }
            }
            foreach ($relative in $original) { [IO.File]::Copy((Assert-SafeDestination $backup $relative), (Assert-SafeDestination $Root $relative), $true) }
        } catch { throw "Build failed: $buildError. Program rollback needs attention: $($_.Exception.Message). Backups: $backup" }
        throw "Build failed; previous program files were restored. $buildError. Backups: $backup"
    }
}

function Invoke-DarkRecompCommitUpdate([string]$Root, [string]$Game = '', [string]$CacheSource = '', [switch]$CheckOnly, [switch]$AcceptUpdate, [switch]$NonInteractive) {
    $lock = $null
    try {
        $Root = [IO.Path]::GetFullPath($Root).TrimEnd('\')
        if ($Root -eq [IO.Path]::GetPathRoot($Root).TrimEnd('\')) { throw 'Use a dedicated game installation directory, outside the volume root.' }
        Assert-NoReparseTree $Root
        $isSource = Test-Path -LiteralPath (Join-Path $Root '.git')
        if ($isSource) {
            if (!(Get-Command git -ErrorAction SilentlyContinue)) { throw 'Source updates require Git. Install Git or use Launch.cmd to play without updating.' }
            $head = Assert-OfficialSourceCheckout $Root
            $currentCommit = Get-SuccessfulBuildCommit $Root
        } else {
            $current = Read-ReleaseManifest (Join-Path $Root 'RELEASE.json')
            if ($current.commit -isnot [string] -or $current.commit -cnotmatch '^[0-9a-f]{40}$') { throw 'The installed RELEASE.json has no valid source commit; extract a current official release before using commit updates.' }
            if (Test-InstalledCommitFiles $Root $current) { $currentCommit = $current.commit }
            else { $currentCommit = ''; Write-Host 'Installed program files are missing or changed; the current main commit needs rebuilding.' }
            $originalManifestHash = Get-ReleaseSha256 (Join-Path $Root 'RELEASE.json')
        }
        try { $commit = Get-OfficialMainCommit } catch { Write-Host "Could not check official main: $($_.Exception.Message)"; return 10 }
        if ($currentCommit -ceq $commit -and (!$isSource -or $head -ceq $commit)) {
            Write-Host "Installed build is current with official main: $commit"
            return 0
        }
        Write-Host "Official main commit to build: $commit"
        if ($CheckOnly -or ($NonInteractive -and !$AcceptUpdate)) { return 2 }
        Assert-CommitBuildPrerequisites
        Assert-NoRunningGame $Root
        if (!$AcceptUpdate) {
            $answer = Read-Host 'Build and install this commit before playing? [y/N]'
            if ($answer -notmatch '^(?i)y(es)?$') { Write-Host 'Keeping the installed build.'; return 0 }
        }
        $work = New-CommitUpdateWork
        $lock = Open-DarkRecompUpdateLock $Root
        Assert-NoReparseTree $Root
        Assert-NoRunningGame $Root
        if ($isSource) {
            Sync-OfficialCommit $Root $commit
            Invoke-SourceUpdateBuild $Root $commit $work
        } else {
            $source = Get-CommitCacheSource $Root $CacheSource
            Sync-OfficialCommit $source $commit -Detached
            if (!$Game) { $Game = Join-Path $Root 'Darkness' }
            Copy-OwnedBuildGame $Game $source
            Invoke-SourceCommitBuild $source (Join-Path $work 'build.log')
            Assert-NoReparseTree $source
            if ((Assert-OfficialSourceCheckout $source) -cne $commit) { throw 'Source clone changed during the build; refusing to install.' }
            $version = Get-CommitPackageVersion $commit
            $archive = Invoke-CommitPackage $source (Join-Path $work 'package') $version $commit
            $stage = Join-Path $work 'stage'; $backup = Join-Path $work 'installation-backup'
            [IO.Directory]::CreateDirectory($stage) | Out-Null
            [IO.Directory]::CreateDirectory($backup) | Out-Null
            $candidate = Expand-VerifiedRelease $archive ($archive + '.sha256') $stage $version
            if ($candidate.commit -cne $commit) { throw 'Locally built package does not identify the requested main commit.' }
            Assert-NoReparseTree $Root
            if ((Get-ReleaseSha256 (Join-Path $Root 'RELEASE.json')) -cne $originalManifestHash) { throw 'Installation changed during the build; refusing to overwrite it.' }
            $fresh = Read-ReleaseManifest (Join-Path $Root 'RELEASE.json')
            Install-VerifiedRelease $Root $stage $backup $fresh $candidate
        }
        Write-Host "Built and installed official main $commit. Game files, saves and settings were preserved."
        return 0
    } catch {
        Write-Host "Commit update refused or failed: $($_.Exception.Message)"
        return 20
    } finally { if ($lock) { $lock.Dispose() } }
}

if (!$LibraryOnly) {
    exit (Invoke-DarkRecompCommitUpdate -Root $InstallRoot -Game $GameDirectory -CacheSource $SourceRoot -CheckOnly:$CheckOnly -AcceptUpdate:$AcceptUpdate -NonInteractive:$NonInteractive)
}
