# Windows PowerShell 5.1; also callable directly to check or install without launching.
# Exit: 0 current/declined/installed; 2 update available; 10 network unavailable;
# 20 unsafe installation, failed validation, or failed installation.
[CmdletBinding()]
param(
    [string]$InstallRoot = '',
    [switch]$CheckOnly,
    [switch]$AcceptUpdate,
    [switch]$NonInteractive,
    [switch]$LibraryOnly
)

$ErrorActionPreference = 'Stop'
$script:ReleaseRepository = 'portingpete/The-Darkness-Recomp'

function ConvertTo-ReleaseVersion([string]$Text) {
    if ($Text -cnotmatch '^v?(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(?:-([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$') {
        throw "Invalid release version: $Text"
    }
    $parts = @($Matches[1], $Matches[2], $Matches[3])
    $pre = @()
    if ($Matches[4]) {
        $pre = @($Matches[4].Split('.'))
        foreach ($part in $pre) {
            if ($part -cmatch '^[0-9]+$' -and $part.Length -gt 1 -and $part.StartsWith('0')) {
                throw "Invalid numeric prerelease identifier: $Text"
            }
        }
    }
    return [pscustomobject]@{ Core = $parts; Pre = $pre }
}

function Compare-ReleaseNumber([string]$Left, [string]$Right) {
    if ($Left.Length -ne $Right.Length) { return [Math]::Sign($Left.Length - $Right.Length) }
    return [Math]::Sign([string]::CompareOrdinal($Left, $Right))
}

function Compare-ReleaseVersion([string]$Left, [string]$Right) {
    $a = ConvertTo-ReleaseVersion $Left
    $b = ConvertTo-ReleaseVersion $Right
    for ($i = 0; $i -lt 3; $i++) {
        $c = Compare-ReleaseNumber $a.Core[$i] $b.Core[$i]
        if ($c) { return $c }
    }
    if (!$a.Pre.Count -and !$b.Pre.Count) { return 0 }
    if (!$a.Pre.Count) { return 1 }
    if (!$b.Pre.Count) { return -1 }
    for ($i = 0; $i -lt [Math]::Min($a.Pre.Count, $b.Pre.Count); $i++) {
        $x = $a.Pre[$i]; $y = $b.Pre[$i]
        $xn = $x -cmatch '^[0-9]+$'; $yn = $y -cmatch '^[0-9]+$'
        if ($xn -and $yn) { $c = Compare-ReleaseNumber $x $y }
        elseif ($xn -ne $yn) { if ($xn) { $c = -1 } else { $c = 1 } }
        else { $c = [Math]::Sign([string]::CompareOrdinal($x, $y)) }
        if ($c) { return $c }
    }
    return [Math]::Sign($a.Pre.Count - $b.Pre.Count)
}

function Assert-ManagedPath([string]$Relative) {
    # These are release-owned files. No game data, settings, saves, run logs, or
    # arbitrary root/tools files are eligible for replacement, even in a manifest.
    if (!$Relative -or $Relative.Length -gt 240 -or $Relative.Contains('\') -or
        $Relative -cmatch '(^/|:|[\x00-\x1F]|//)' -or $Relative -match '(^|/)\.{1,2}(/|$)') {
        throw "Unsafe archive path: $Relative"
    }
    foreach ($segment in $Relative.Split('/')) {
        if (!$segment -or $segment.EndsWith('.') -or $segment.EndsWith(' ') -or
            $segment -match '^(CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)' -or
            $segment -match '[<>"|?*]') { throw "Unsafe Windows path: $Relative" }
    }
    $rootFiles = @('RELEASE.json', 'Launch.cmd', 'LaunchWithUpdates.cmd', 'LaunchWithSettings.cmd',
        'LaunchStallProfiler.cmd', 'LaunchShadowCapture.cmd', 'Launch.sh', 'SetupLinux.cmd',
        'PlayLinux.cmd', 'START_HERE.txt', 'README.md', 'CONTROLS.md', 'RENDERING.md', 'STEAM_DECK.md', 'COPYING')
    $toolsFiles = @('tools/update_release.ps1', 'tools/add_steam_shortcut.py', 'tools/setup_linux.ps1',
        'tools/setup_linux.py', 'tools/wsl_graphics.py')
    $audioFiles = @('ThirdParty/README.txt', 'ThirdParty/audio/COPYING.LGPLv2.1',
        'ThirdParty/audio/COPYING.winpthreads', 'ThirdParty/audio/LICENSE.md', 'ThirdParty/audio/PROVENANCE.json',
        'ThirdParty/audio/ffmpeg-darkxma-upstream.tar.gz', 'ThirdParty/audio/ffmpeg-darkxma.patch',
        'ThirdParty/audio/build_xma_codec.py')
    if ($rootFiles -ccontains $Relative -or $toolsFiles -ccontains $Relative -or $audioFiles -ccontains $Relative) { return }
    if ($Relative -cmatch '^build_native/Release/(?:DarkRecomp(?:Preview|Settings)?\.exe|[A-Za-z0-9_.-]+\.dll|CubeWnd\.pc\.xcr(?:\.source\.sha256)?|GameContext_Create\.pc\.xdf)$') { return }
    throw "File is outside the release-owned paths: $Relative"
}

function Assert-NoReparseTree([string]$Root) {
    $item = Get-Item -LiteralPath $Root -Force
    if (!$item.PSIsContainer) { throw "Installation is not a directory: $Root" }
    $ancestor = $item
    while ($null -ne $ancestor) {
        if ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point is not supported: $($ancestor.FullName)" }
        $ancestor = $ancestor.Parent
    }
    $pending = New-Object 'System.Collections.Generic.Stack[string]'
    $pending.Push($item.FullName)
    while ($pending.Count) {
        $directory = $pending.Pop()
        foreach ($child in Get-ChildItem -LiteralPath $directory -Force -ErrorAction Stop) {
            if ($child.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point is not supported: $($child.FullName)" }
            if ($child.PSIsContainer) { $pending.Push($child.FullName) }
        }
    }
}

function Assert-SafeDestination([string]$Root, [string]$Relative) {
    Assert-ManagedPath $Relative
    $path = [IO.Path]::GetFullPath((Join-Path $Root ($Relative.Replace('/', '\'))))
    if (!$path.StartsWith($Root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) { throw 'Destination escapes installation.' }
    $cursor = $path
    while ($cursor -and $cursor.Length -ge $Root.TrimEnd('\').Length) {
        if (Test-Path -LiteralPath $cursor) {
            $item = Get-Item -LiteralPath $cursor -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point in destination: $cursor" }
            if ($cursor -eq $path -and $item.PSIsContainer) { throw "Expected a file at $cursor" }
        }
        $cursor = [IO.Path]::GetDirectoryName($cursor)
    }
    return $path
}

function Assert-NoRunningGame([string]$Root) {
    foreach ($process in @(Get-Process -Name DarkRecomp, DarkRecompPreview, DarkRecompSettings -ErrorAction SilentlyContinue)) {
        try { $path = $process.Path } catch { throw 'Cannot verify a running game process; close it before updating.' }
        if (!$path -or $path.StartsWith($Root.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Close the game and its settings window before updating.'
        }
    }
}

function Read-ReleaseManifest([string]$Path, [string]$ExpectedVersion = '') {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.Length -gt 4194304) { throw 'Release manifest is too large.' }
    $manifest = [IO.File]::ReadAllText($Path) | ConvertFrom-Json
    if (!$manifest -or $manifest.version -isnot [string] -or !$manifest.sha256 -or
        $manifest.sha256 -isnot [pscustomobject]) { throw 'Invalid RELEASE.json.' }
    $null = ConvertTo-ReleaseVersion $manifest.version
    if ($ExpectedVersion -and $manifest.version -cne $ExpectedVersion) { throw 'Archive version does not match the release tag.' }
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $manifest.sha256.PSObject.Properties) {
        Assert-ManagedPath $entry.Name
        if ($entry.Name -ceq 'RELEASE.json' -or !$seen.Add($entry.Name) -or $entry.Value -isnot [string] -or
            $entry.Value -cnotmatch '^[0-9a-fA-F]{64}$') { throw "Invalid manifest entry: $($entry.Name)" }
    }
    foreach ($required in @('Launch.cmd', 'build_native/Release/DarkRecomp.exe', 'build_native/Release/DarkRecompPreview.exe')) {
        if (!$seen.Contains($required)) { throw "Manifest is missing $required" }
    }
    return $manifest
}

function Get-OfficialReleases {
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    $all = @()
    for ($page = 1; $page -le 100; $page++) {
        $uri = "https://api.github.com/repos/$script:ReleaseRepository/releases?per_page=100&page=$page"
        $response = Invoke-RestMethod -Uri $uri -Headers @{ 'User-Agent' = 'The-Darkness-Recomp-Updater'; 'Accept' = 'application/vnd.github+json' } -TimeoutSec 20
        $batch = @($response)
        $all += $batch
        if ($batch.Count -lt 100) { return $all }
    }
    throw 'Release listing exceeds the supported page limit.'
}

function Select-NewestRelease($Releases, [string]$Current) {
    $best = $null
    foreach ($release in @($Releases)) {
        if ($release.draft -or !$release.published_at -or $release.tag_name -isnot [string]) { continue }
        try { $null = ConvertTo-ReleaseVersion $release.tag_name } catch { continue }
        if ((Compare-ReleaseVersion $release.tag_name $Current) -le 0) { continue }
        if (!$best -or (Compare-ReleaseVersion $release.tag_name $best.tag_name) -gt 0) { $best = $release }
    }
    return $best
}

function Get-OfficialAsset($Release, [string]$Name) {
    $assets = @($Release.assets | Where-Object { $_.name -ceq $Name })
    if ($assets.Count -ne 1) { throw "Release must have exactly one asset named $Name" }
    $expected = "https://github.com/$script:ReleaseRepository/releases/download/$([Uri]::EscapeDataString($Release.tag_name))/$([Uri]::EscapeDataString($Name))"
    $actual = [Uri]$assets[0].browser_download_url
    if (!$actual.IsAbsoluteUri -or $actual.AbsoluteUri -cne ([Uri]$expected).AbsoluteUri -or
        $actual.UserInfo -or $actual.Query -or $actual.Fragment -or !$actual.IsDefaultPort) {
        throw "Asset does not use the official repository download URL: $Name"
    }
    return $actual.AbsoluteUri
}

function Save-ReleaseAsset([string]$Uri, [string]$Path) {
    Invoke-WebRequest -UseBasicParsing -Uri $Uri -OutFile $Path -Headers @{ 'User-Agent' = 'The-Darkness-Recomp-Updater' } -TimeoutSec 120 | Out-Null
}

function Get-ReleaseSha256([string]$Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString($algorithm.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}

function Expand-VerifiedRelease([string]$Archive, [string]$Checksum, [string]$Stage, [string]$Version) {
    if ((Get-Item -LiteralPath $Archive).Length -gt 1073741824 -or (Get-Item -LiteralPath $Checksum).Length -gt 1024) { throw 'Release download exceeds the size limit.' }
    $line = [IO.File]::ReadAllText($Checksum).Trim()
    $fileName = [IO.Path]::GetFileName($Archive)
    if ($line -cnotmatch ('^([0-9a-fA-F]{64})  ' + [Regex]::Escape($fileName) + '$')) { throw 'Invalid archive checksum file.' }
    $expectedHash = $Matches[1]
    if ((Get-ReleaseSha256 $Archive) -ine $expectedHash) { throw 'Release archive checksum does not match.' }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        if ($zip.Entries.Count -gt 4096) { throw 'Release has too many archive entries.' }
        $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        [long]$total = 0
        $entries = @()
        foreach ($entry in $zip.Entries) {
            $name = $entry.FullName
            if ($name.EndsWith('/')) { throw "Unexpected archive directory entry: $name" }
            if (!$seen.Add($name)) { throw "Duplicate archive path: $name" }
            # ZIP DOS reparse attribute or Unix symbolic link/special file.
            $attributes = [long]$entry.ExternalAttributes -band 0xffffffffL
            $unixType = ($attributes -shr 16) -band 0xf000
            if (($attributes -band 0x400) -or ($unixType -ne 0 -and $unixType -ne 0x8000)) { throw "Archive contains a link or special file: $name" }
            $total += $entry.Length
            if ($entry.Length -gt 536870912 -or $total -gt 1073741824) { throw 'Unpacked release exceeds the size limit.' }
            if ($name -ceq 'Darkness/PUT_GAME_FILES_HERE.txt') { continue }
            Assert-ManagedPath $name
            $entries += $entry
        }
        if (!$seen.Contains('RELEASE.json')) { throw 'Archive has no RELEASE.json.' }
        foreach ($entry in $entries) {
            $destination = Assert-SafeDestination $Stage $entry.FullName
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
            $inputStream = $entry.Open()
            $outputStream = [IO.File]::Open($destination, [IO.FileMode]::CreateNew)
            try { $inputStream.CopyTo($outputStream) } finally { $outputStream.Dispose(); $inputStream.Dispose() }
        }
    } finally { $zip.Dispose() }
    $manifest = Read-ReleaseManifest (Join-Path $Stage 'RELEASE.json') $Version
    $managed = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($property in $manifest.sha256.PSObject.Properties) {
        [void]$managed.Add($property.Name)
        $path = Assert-SafeDestination $Stage $property.Name
        if (!(Test-Path -LiteralPath $path -PathType Leaf) -or
            (Get-ReleaseSha256 $path) -ine $property.Value) {
            throw "Release file checksum does not match: $($property.Name)"
        }
    }
    foreach ($entry in $entries) {
        if ($entry.FullName -cne 'RELEASE.json' -and !$managed.Contains($entry.FullName)) { throw "Unmanifested release file: $($entry.FullName)" }
    }
    return $manifest
}

function Copy-ReleaseFile([string]$Source, [string]$Destination) {
    [IO.File]::Copy($Source, $Destination, $true)
}

function Install-VerifiedRelease([string]$Root, [string]$Stage, [string]$Backup, $Current, $Candidate) {
    Assert-NoReparseTree $Root
    Assert-NoReparseTree $Stage
    Assert-NoRunningGame $Root
    if (Test-Path -LiteralPath (Join-Path $Root '.git')) { throw 'Source checkouts must be updated with Git; no release files were installed.' }
    $currentPaths = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($property in $Current.sha256.PSObject.Properties) { [void]$currentPaths.Add($property.Name) }
    [void]$currentPaths.Add('RELEASE.json')
    $paths = @($Candidate.sha256.PSObject.Properties.Name | Sort-Object) + @('RELEASE.json')
    $journal = New-Object 'System.Collections.Generic.List[object]'
    # Validate every overwrite and finish every backup before the first change.
    foreach ($relative in $paths) {
        $destination = Assert-SafeDestination $Root $relative
        $exists = Test-Path -LiteralPath $destination -PathType Leaf
        if ($exists -and !$currentPaths.Contains($relative)) { throw "Existing file is not managed by the installed release: $relative" }
        if ($exists) {
            $backupPath = Assert-SafeDestination $Backup $relative
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($backupPath)) | Out-Null
            [IO.File]::Copy($destination, $backupPath, $false)
        }
        $journal.Add([pscustomobject]@{ Relative = $relative; Existed = $exists; Applied = $false })
    }
    try {
        foreach ($record in $journal) {
            $destination = Assert-SafeDestination $Root $record.Relative
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($destination)) | Out-Null
            $record.Applied = $true
            Copy-ReleaseFile (Assert-SafeDestination $Stage $record.Relative) $destination
        }
    } catch {
        $installError = $_.Exception.Message
        $rollbackErrors = @()
        for ($i = $journal.Count - 1; $i -ge 0; $i--) {
            $record = $journal[$i]
            if (!$record.Applied) { continue }
            try {
                $destination = Assert-SafeDestination $Root $record.Relative
                if ($record.Existed) { [IO.File]::Copy((Assert-SafeDestination $Backup $record.Relative), $destination, $true) }
                elseif (Test-Path -LiteralPath $destination -PathType Leaf) { Remove-Item -LiteralPath $destination -Force }
            } catch { $rollbackErrors += $_.Exception.Message }
        }
        if ($rollbackErrors.Count) { throw "Installation failed: $installError. Rollback needs attention: $($rollbackErrors -join '; '). Backups: $Backup" }
        throw "Installation failed and original files were restored: $installError. Backups: $Backup"
    }
}

function Invoke-DarkRecompUpdate([string]$Root, [switch]$CheckOnly, [switch]$AcceptUpdate, [switch]$NonInteractive) {
    $lock = $null
    $work = $null
    try {
        $Root = [IO.Path]::GetFullPath($Root).TrimEnd('\')
        if ($Root -eq [IO.Path]::GetPathRoot($Root).TrimEnd('\')) { throw 'Install the release in its own directory, outside the volume root.' }
        Assert-NoReparseTree $Root
        if (Test-Path -LiteralPath (Join-Path $Root '.git')) { throw 'Source checkouts must be updated with Git.' }
        $current = Read-ReleaseManifest (Join-Path $Root 'RELEASE.json')
        try { $releases = Get-OfficialReleases } catch {
            Write-Host "Could not check GitHub releases: $($_.Exception.Message)"
            return 10
        }
        $release = Select-NewestRelease $releases $current.version
        if (!$release) {
            Write-Host "No newer published release is available. Installed: $($current.version)."
            Write-Host 'Source-code fixes become available here after a new release is published.'
            return 0
        }
        Write-Host "Update available: $($current.version) -> $($release.tag_name)"
        if ($CheckOnly -or ($NonInteractive -and !$AcceptUpdate)) { return 2 }
        if (!$AcceptUpdate) {
            $answer = Read-Host 'Install this release before playing? [y/N]'
            if ($answer -notmatch '^(?i)y(es)?$') { Write-Host 'Keeping the installed release.'; return 0 }
        }
        Assert-NoRunningGame $Root
        # The retained work directory holds the verified archive and recovery copy.
        # Never recursively delete it: manual cleanup can inspect its contents first.
        $work = Join-Path ([IO.Path]::GetTempPath()) ('DarkRecomp-update-' + [Guid]::NewGuid().ToString('N'))
        [IO.Directory]::CreateDirectory($work) | Out-Null
        Assert-NoReparseTree $work
        $stage = Join-Path $work 'stage'; $backup = Join-Path $work 'backup'
        [IO.Directory]::CreateDirectory($stage) | Out-Null
        [IO.Directory]::CreateDirectory($backup) | Out-Null
        Write-Host "Update files and recovery backups: $work"
        $archiveName = "The-Darkness-Recomp-$($release.tag_name)-windows-x64.zip"
        $archiveUri = Get-OfficialAsset $release $archiveName
        $checksumUri = Get-OfficialAsset $release ($archiveName + '.sha256')
        $archive = Join-Path $work $archiveName; $checksum = $archive + '.sha256'
        try {
            Save-ReleaseAsset $archiveUri $archive
            Save-ReleaseAsset $checksumUri $checksum
        } catch { Write-Host "Could not download release: $($_.Exception.Message)"; return 10 }
        $candidate = Expand-VerifiedRelease $archive $checksum $stage $release.tag_name
        Assert-NoReparseTree $Root
        $lockPath = Join-Path $Root '.DarkRecomp-update.lock'
        if (Test-Path -LiteralPath $lockPath) {
            $lockItem = Get-Item -LiteralPath $lockPath -Force
            if ($lockItem.PSIsContainer -or ($lockItem.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Unsafe update lock path.' }
        }
        $lock = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
        $fresh = Read-ReleaseManifest (Join-Path $Root 'RELEASE.json')
        if ((Compare-ReleaseVersion $candidate.version $fresh.version) -le 0) { throw 'Installed release changed during this update; refusing a downgrade or reinstall.' }
        Install-VerifiedRelease $Root $stage $backup $fresh $candidate
        Write-Host "Installed $($candidate.version). Your game files, saves and settings were preserved."
        return 0
    } catch {
        Write-Host "Update refused or failed: $($_.Exception.Message)"
        return 20
    } finally {
        if ($lock) { $lock.Dispose() }
    }
}

if (!$LibraryOnly) {
    if (!$InstallRoot) {
        if ($PSScriptRoot) { $InstallRoot = Split-Path -Parent $PSScriptRoot }
        else { throw 'InstallRoot is required when running the updater in memory.' }
    }
    exit (Invoke-DarkRecompUpdate -Root $InstallRoot -CheckOnly:$CheckOnly -AcceptUpdate:$AcceptUpdate -NonInteractive:$NonInteractive)
}
