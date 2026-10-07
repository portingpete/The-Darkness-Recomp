# Offline regression tests, run with Windows PowerShell 5.1; no Pester required.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
. (Join-Path (Split-Path -Parent $PSScriptRoot) 'tools/update_release.ps1') -LibraryOnly
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('DarkRecomp-updater-tests-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($testRoot) | Out-Null
$script:TestCount = 0
$script:FixtureNumber = 0
$script:NetworkFailure = $false
$script:Downloads = 0
$script:Releases = @()
$script:DownloadFiles = @{}
$script:FailCopy = ''

function Assert-True($Condition, [string]$Message) {
    $script:TestCount++
    if (!$Condition) { throw "Assertion failed: $Message" }
}
function Assert-Throws([scriptblock]$Action, [string]$Message) {
    $threw = $false
    try { & $Action | Out-Null } catch { $threw = $true }
    Assert-True $threw $Message
}
function Write-TestFile([string]$Root, [string]$Relative, [string]$Content) {
    $path = Join-Path $Root $Relative.Replace('/', '\')
    [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($path)) | Out-Null
    [IO.File]::WriteAllText($path, $Content, (New-Object Text.UTF8Encoding($false)))
}
function New-Manifest([string]$Version, $Files) {
    $hashes = [ordered]@{}
    foreach ($name in $Files.Keys) {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try { $hashes[$name] = ([BitConverter]::ToString($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($Files[$name])))).Replace('-', '').ToLowerInvariant() }
        finally { $algorithm.Dispose() }
    }
    return ([ordered]@{ version = $Version; commit = ('a' * 40); sha256 = $hashes } | ConvertTo-Json -Depth 10)
}
function New-Installation {
    $script:FixtureNumber++
    $root = Join-Path $testRoot ('install & files!-' + $script:FixtureNumber)
    $files = [ordered]@{
        'Launch.cmd' = "@echo off`r`necho ORIGINAL-LAUNCH`r`nexit /b 0`r`n"
        'build_native/Release/DarkRecomp.exe' = 'original executable'
        'build_native/Release/DarkRecompPreview.exe' = 'original preview'
        'README.md' = 'original readme'
    }
    foreach ($key in $files.Keys) { Write-TestFile $root $key $files[$key] }
    Write-TestFile $root 'RELEASE.json' (New-Manifest 'v1.0.0' $files)
    Write-TestFile $root 'Darkness/default.xex' 'owned game'
    Write-TestFile $root 'saves/player.sav' 'saved progress'
    Write-TestFile $root 'DarkRecomp.settings.ini' 'user settings'
    Write-TestFile $root 'build_native/run/test.log' 'run log'
    return $root
}
function New-Archive([string]$Version = 'v1.1.0', [string]$Variant = '') {
    $script:FixtureNumber++
    $output = Join-Path $testRoot ('archive-' + $script:FixtureNumber)
    [IO.Directory]::CreateDirectory($output) | Out-Null
    $files = [ordered]@{
        'Launch.cmd' = "@echo off`r`necho UPDATED-LAUNCH`r`nexit /b 0`r`n"
        'build_native/Release/DarkRecomp.exe' = 'updated executable'
        'build_native/Release/DarkRecompPreview.exe' = 'updated preview'
        'README.md' = 'updated readme'
        'LaunchWithUpdates.cmd' = [IO.File]::ReadAllText((Join-Path (Split-Path -Parent $PSScriptRoot) 'LaunchWithUpdates.cmd')) + "`r`nrem changed launcher length`r`n"
        'tools/update_release.ps1' = "throw 'Replacement updater should run on the next launch only.'"
    }
    if ($Variant -eq 'protected') { $files['saves/player.sav'] = 'attack' }
    $manifestVersion = $Version
    if ($Variant -eq 'wrong-version') { $manifestVersion = 'v9.0.0' }
    $manifest = New-Manifest $manifestVersion $files
    $archive = Join-Path $output "The-Darkness-Recomp-$Version-windows-x64.zip"
    $zip = [IO.Compression.ZipFile]::Open($archive, [IO.Compression.ZipArchiveMode]::Create)
    try {
        $entries = @()
        foreach ($key in $files.Keys) { $entries += [pscustomobject]@{ Name = $key; Content = $files[$key]; Attr = 0 } }
        $entries += [pscustomobject]@{ Name = 'RELEASE.json'; Content = $manifest; Attr = 0 }
        $entries += [pscustomobject]@{ Name = 'Darkness/PUT_GAME_FILES_HERE.txt'; Content = 'do not replace the existing marker'; Attr = 0 }
        if ($Variant -eq 'traversal') { $entries += [pscustomobject]@{ Name = '../outside.txt'; Content = 'attack'; Attr = 0 } }
        if ($Variant -eq 'duplicate') { $entries += [pscustomobject]@{ Name = 'readme.md'; Content = 'duplicate'; Attr = 0 } }
        if ($Variant -eq 'unmanifested') { $entries += [pscustomobject]@{ Name = 'CONTROLS.md'; Content = 'unlisted'; Attr = 0 } }
        if ($Variant -eq 'link') { $entries += [pscustomobject]@{ Name = 'CONTROLS.md'; Content = 'link'; Attr = [int]0xa0000000 } }
        if ($Variant -eq 'reparse') { $entries += [pscustomobject]@{ Name = 'CONTROLS.md'; Content = 'link'; Attr = 0x400 } }
        if ($Variant -eq 'hash') { ($entries | Where-Object { $_.Name -eq 'README.md' }).Content = 'tampered content' }
        foreach ($item in $entries) {
            $entry = $zip.CreateEntry($item.Name)
            $entry.ExternalAttributes = $item.Attr
            $stream = $entry.Open()
            $bytes = [Text.Encoding]::UTF8.GetBytes($item.Content)
            try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
        }
    } finally { $zip.Dispose() }
    $checksum = $archive + '.sha256'
    [IO.File]::WriteAllText($checksum, (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() + '  ' + [IO.Path]::GetFileName($archive) + "`n")
    if ($Variant -eq 'zip-hash') { [IO.File]::AppendAllText($archive, 'tamper') }
    return [pscustomobject]@{ Archive = $archive; Checksum = $checksum; Version = $Version }
}
function Set-TestRelease($Bundle) {
    $base = "https://github.com/portingpete/The-Darkness-Recomp/releases/download/$($Bundle.Version)/"
    $script:Releases = @([pscustomobject]@{
        tag_name = $Bundle.Version; draft = $false; prerelease = $true; published_at = '2026-10-07T00:00:00Z'
        assets = @(
            [pscustomobject]@{ name = [IO.Path]::GetFileName($Bundle.Archive); browser_download_url = $base + [IO.Path]::GetFileName($Bundle.Archive) },
            [pscustomobject]@{ name = [IO.Path]::GetFileName($Bundle.Checksum); browser_download_url = $base + [IO.Path]::GetFileName($Bundle.Checksum) }
        )
    })
    $script:DownloadFiles = @{}
    $script:DownloadFiles[$base + [IO.Path]::GetFileName($Bundle.Archive)] = $Bundle.Archive
    $script:DownloadFiles[$base + [IO.Path]::GetFileName($Bundle.Checksum)] = $Bundle.Checksum
}
# Replace only network I/O and one injectable copy operation. All release,
# checksum, path, extraction, destination and rollback checks remain production code.
function Get-OfficialReleases {
    if ($script:NetworkFailure) { throw 'offline fixture' }
    return $script:Releases
}
function Save-ReleaseAsset([string]$Uri, [string]$Path) {
    $script:Downloads++
    if ($script:NetworkFailure) { throw 'offline download fixture' }
    if (!$script:DownloadFiles.ContainsKey($Uri)) { throw "Unexpected download: $Uri" }
    [IO.File]::Copy($script:DownloadFiles[$Uri], $Path)
}
function Copy-ReleaseFile([string]$Source, [string]$Destination) {
    if ($script:FailCopy -and $Destination.EndsWith($script:FailCopy, [StringComparison]::OrdinalIgnoreCase)) {
        [IO.File]::WriteAllText($Destination, 'partial write')
        throw 'simulated mid-copy failure'
    }
    [IO.File]::Copy($Source, $Destination, $true)
}
function Assert-Preserved([string]$Root) {
    foreach ($pair in @(@('Darkness/default.xex', 'owned game'), @('saves/player.sav', 'saved progress'),
        @('DarkRecomp.settings.ini', 'user settings'), @('build_native/run/test.log', 'run log'))) {
        Assert-True ([IO.File]::ReadAllText((Join-Path $Root $pair[0])) -ceq $pair[1]) "Preserved $($pair[0])"
    }
}

$orderedVersions = @('v1.0.0-alpha', 'v1.0.0-alpha.1', 'v1.0.0-alpha.beta', 'v1.0.0-beta',
    'v1.0.0-beta.2', 'v1.0.0-beta.11', 'v1.0.0-rc.1', 'v1.0.0', 'v1.0.1', 'v1.1.0', 'v2.0.0')
for ($i = 1; $i -lt $orderedVersions.Count; $i++) {
    Assert-True ((Compare-ReleaseVersion $orderedVersions[$i] $orderedVersions[$i - 1]) -gt 0) 'Semantic version precedence'
}
Assert-True ((Compare-ReleaseVersion 'v1.0.0+build.9' 'v1.0.0+build.1') -eq 0) 'Build metadata ignored'
Assert-True ((Compare-ReleaseVersion 'v999999999999999999999.0.0' 'v99999999999999999999.9.9') -gt 0) 'Arbitrary-length semantic version numbers'
foreach ($bad in @('v01.0.0', 'v1.00.0', 'v1.0', 'v1.0.0-01', 'v1.0.0-alpha..1', 'v1.0.0-', 'v1.0.0/evil')) {
    Assert-Throws { ConvertTo-ReleaseVersion $bad } "Invalid version $bad"
}
$releaseList = @(
    [pscustomobject]@{ tag_name = 'v2.0.0-beta.2'; draft = $false; prerelease = $true; published_at = 'yesterday' },
    [pscustomobject]@{ tag_name = 'v1.1.0'; draft = $false; published_at = 'today' },
    [pscustomobject]@{ tag_name = 'v9.0.0'; draft = $true; published_at = 'today' },
    [pscustomobject]@{ tag_name = 'v8.0.0'; draft = $false; published_at = $null },
    [pscustomobject]@{ tag_name = 'bad-tag'; draft = $false; published_at = 'today' }
)
Assert-True ((Select-NewestRelease $releaseList 'v1.0.0').tag_name -eq 'v2.0.0-beta.2') 'Includes prereleases and sorts by semantic version'
Assert-True ($null -eq (Select-NewestRelease $releaseList 'v2.0.0')) 'No downgrade'

$bundle = New-Archive
Set-TestRelease $bundle
$goodRelease = $script:Releases[0]
$assetName = [IO.Path]::GetFileName($bundle.Archive)
Assert-True ((Get-OfficialAsset $goodRelease $assetName).StartsWith('https://github.com/portingpete/The-Darkness-Recomp/releases/download/')) 'Official asset selected'
foreach ($badUrl in @('http://github.com/portingpete/The-Darkness-Recomp/releases/download/v1.1.0/' + $assetName,
    'https://github.com.evil.example/' + $assetName, 'https://github.com/other/repo/releases/download/v1.1.0/' + $assetName)) {
    $badRelease = [pscustomobject]@{ tag_name = 'v1.1.0'; assets = @([pscustomobject]@{ name = $assetName; browser_download_url = $badUrl }) }
    Assert-Throws { Get-OfficialAsset $badRelease $assetName } 'Untrusted download URL refused'
}
$duplicateAssets = [pscustomobject]@{ tag_name = 'v1.1.0'; assets = @($goodRelease.assets[0], $goodRelease.assets[0]) }
Assert-Throws { Get-OfficialAsset $duplicateAssets $assetName } 'Duplicate download asset refused'

$root = New-Installation
$before = [IO.File]::ReadAllText((Join-Path $root 'RELEASE.json'))
$code = Invoke-DarkRecompUpdate -Root $root -CheckOnly
Assert-True ($code -eq 2 -and $script:Downloads -eq 0) 'Check-only detects update without downloading'
Assert-True ([IO.File]::ReadAllText((Join-Path $root 'RELEASE.json')) -ceq $before) 'Check-only did not mutate installation'
Assert-True ((Invoke-DarkRecompUpdate -Root $root -NonInteractive) -eq 2) 'Noninteractive requires explicit acceptance'
$script:NetworkFailure = $true
Assert-True ((Invoke-DarkRecompUpdate -Root $root -AcceptUpdate) -eq 10) 'Network failure has fallback exit code'
$script:NetworkFailure = $false
Assert-True ((Invoke-DarkRecompUpdate -Root $root -AcceptUpdate) -eq 0) 'Verified update succeeds'
Assert-True ((Read-ReleaseManifest (Join-Path $root 'RELEASE.json')).version -eq 'v1.1.0') 'Updated manifest installed'
Assert-True ([IO.File]::ReadAllText((Join-Path $root 'README.md')) -eq 'updated readme') 'Updated file installed'
Assert-True (!(Test-Path -LiteralPath (Join-Path $root 'Darkness/PUT_GAME_FILES_HERE.txt'))) 'Game placeholder omitted from update'
Assert-Preserved $root
Assert-True ((Invoke-DarkRecompUpdate -Root $root -AcceptUpdate) -eq 0) 'Same version is not reinstalled'

foreach ($variant in @('traversal', 'duplicate', 'unmanifested', 'link', 'reparse', 'hash', 'zip-hash', 'wrong-version', 'protected')) {
    $root = New-Installation
    Set-TestRelease (New-Archive 'v1.1.0' $variant)
    Assert-True ((Invoke-DarkRecompUpdate -Root $root -AcceptUpdate) -eq 20) "Rejected $variant archive"
    Assert-True ((Read-ReleaseManifest (Join-Path $root 'RELEASE.json')).version -eq 'v1.0.0') "$variant did not change installed release"
    Assert-Preserved $root
}

Set-TestRelease $bundle
$root = New-Installation
Write-TestFile $root 'tools/update_release.ps1' 'unmanaged user script'
Assert-True ((Invoke-DarkRecompUpdate -Root $root -AcceptUpdate) -eq 20) 'Existing unmanaged destination refused'
Assert-True ([IO.File]::ReadAllText((Join-Path $root 'tools/update_release.ps1')) -ceq 'unmanaged user script') 'Unmanaged file preserved'
$root = New-Installation
[IO.Directory]::CreateDirectory((Join-Path $root '.git')) | Out-Null
Assert-True ((Invoke-DarkRecompUpdate -Root $root -AcceptUpdate) -eq 20) 'Source checkout refused'

foreach ($failedPath in @('README.md', 'RELEASE.json')) {
    $root = New-Installation
    $script:FailCopy = $failedPath
    Assert-True ((Invoke-DarkRecompUpdate -Root $root -AcceptUpdate) -eq 20) 'Copy failure reported'
    $script:FailCopy = ''
    Assert-True ((Read-ReleaseManifest (Join-Path $root 'RELEASE.json')).version -eq 'v1.0.0') 'Original manifest restored'
    Assert-True ([IO.File]::ReadAllText((Join-Path $root 'README.md')) -eq 'original readme') 'Original file restored after partial write'
    Assert-True ([IO.File]::ReadAllText((Join-Path $root 'build_native/Release/DarkRecomp.exe')) -eq 'original executable') 'Executable rolled back'
    Assert-True (!(Test-Path -LiteralPath (Join-Path $root 'LaunchWithUpdates.cmd'))) 'New file rolled back'
    Assert-Preserved $root
}

# Non-following tree scan blocks a junction while its outside target is untouched.
$root = New-Installation
$outside = Join-Path $testRoot 'outside-junction-target'
Write-TestFile $outside 'sentinel.txt' 'external target preserved'
$junction = Join-Path $root 'linked-game'
New-Item -ItemType Junction -Path $junction -Target $outside | Out-Null
Assert-True ((Invoke-DarkRecompUpdate -Root $root -AcceptUpdate) -eq 20) 'Junction installation refused'
Assert-True ([IO.File]::ReadAllText((Join-Path $outside 'sentinel.txt')) -eq 'external target preserved') 'Outside junction target untouched'

# Exercise the actual CMD and a separate Windows PowerShell 5.1 process. The
# fixture substitutes network functions before main, keeping validation intact.
$root = New-Installation
$launcherPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'LaunchWithUpdates.cmd'
$updaterPath = Join-Path (Split-Path -Parent $PSScriptRoot) 'tools/update_release.ps1'
$files = [ordered]@{}
foreach ($relative in @('Launch.cmd', 'build_native/Release/DarkRecomp.exe', 'build_native/Release/DarkRecompPreview.exe', 'README.md')) {
    $files[$relative] = [IO.File]::ReadAllText((Join-Path $root $relative))
}
$files['LaunchWithUpdates.cmd'] = [IO.File]::ReadAllText($launcherPath)
Set-TestRelease $bundle
$recordsPath = Join-Path $testRoot 'process-releases.json'
[IO.File]::WriteAllText($recordsPath, ($script:Releases | ConvertTo-Json -Depth 10))
$injection = @"
function Get-OfficialReleases { return ([IO.File]::ReadAllText('$($recordsPath.Replace("'", "''"))') | ConvertFrom-Json) }
function Save-ReleaseAsset([string]`$Uri, [string]`$Path) {
    `$name = [IO.Path]::GetFileName(([Uri]`$Uri).AbsolutePath)
    [IO.File]::Copy((Join-Path '$(([IO.Path]::GetDirectoryName($bundle.Archive)).Replace("'", "''"))' `$name), `$Path)
}
"@
$scriptText = [IO.File]::ReadAllText($updaterPath)
$scriptText = $scriptText.Replace('if (!$LibraryOnly) {', $injection + "`r`n" + 'if (!$LibraryOnly) {')
$files['tools/update_release.ps1'] = $scriptText
foreach ($key in $files.Keys) { Write-TestFile $root $key $files[$key] }
Write-TestFile $root 'RELEASE.json' (New-Manifest 'v1.0.0' $files)
$start = New-Object Diagnostics.ProcessStartInfo
$start.FileName = $env:ComSpec
$start.Arguments = '/d /c ""' + (Join-Path $root 'LaunchWithUpdates.cmd') + '" check"'
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = [Diagnostics.Process]::Start($start)
$process.StandardInput.WriteLine('y')
$process.StandardInput.Close()
$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()
if (!$process.WaitForExit(60000)) { $process.Kill(); throw 'Launcher process timed out.' }
$stdout = $stdoutTask.Result; $stderr = $stderrTask.Result
Assert-True ($process.ExitCode -eq 0) "Actual launcher exited successfully: $stdout $stderr"
Assert-True ($stdout.Contains('UPDATED-LAUNCH')) "Actual launcher installed and launched updated Launch.cmd: $stdout"
Assert-True ((Read-ReleaseManifest (Join-Path $root 'RELEASE.json')).version -eq 'v1.1.0') 'Self-replacement completed in a real PowerShell process'
Assert-Preserved $root

# Offline launcher still forwards quoted game arguments and Launch.cmd's result.
$root = New-Installation
$files['Launch.cmd'] = "@echo off`r`necho ARGUMENTS: %*`r`nexit /b 37`r`n"
$offlineInjection = "function Get-OfficialReleases { throw 'offline fixture' }"
$files['tools/update_release.ps1'] = [IO.File]::ReadAllText($updaterPath).Replace('if (!$LibraryOnly) {', $offlineInjection + "`r`n" + 'if (!$LibraryOnly) {')
foreach ($key in $files.Keys) { Write-TestFile $root $key $files[$key] }
Write-TestFile $root 'RELEASE.json' (New-Manifest 'v1.0.0' $files)
$start.Arguments = '/d /c ""' + (Join-Path $root 'LaunchWithUpdates.cmd') + '" play --game-dir "dump & files!" --fps 120"'
$process = [Diagnostics.Process]::Start($start)
$process.StandardInput.Close()
$stdoutTask = $process.StandardOutput.ReadToEndAsync()
$stderrTask = $process.StandardError.ReadToEndAsync()
if (!$process.WaitForExit(60000)) { $process.Kill(); throw 'Offline launcher process timed out.' }
$stdout = $stdoutTask.Result; $stderr = $stderrTask.Result
Assert-True ($process.ExitCode -eq 37) "Offline launcher preserves Launch.cmd exit code: $stdout $stderr"
Assert-True ($stdout.Contains('Update service unavailable. Starting the installed version.')) 'Offline launcher permits installed game'
Assert-True ($stdout.Contains('ARGUMENTS: play --game-dir "dump & files!" --fps 120')) "Launcher preserves quoted arguments: $stdout"

# A real process from this installation must prevent an update before downloading.
$root = New-Installation
$runningExe = Join-Path $root 'build_native/Release/DarkRecomp.exe'
Remove-Item -LiteralPath $runningExe
Add-Type -TypeDefinition 'class UpdaterRunningFixture { public static void Main() { System.Threading.Thread.Sleep(30000); } }' -OutputAssembly $runningExe -OutputType ConsoleApplication
$gameStart = New-Object Diagnostics.ProcessStartInfo
$gameStart.FileName = $runningExe
$gameStart.UseShellExecute = $false
$gameStart.CreateNoWindow = $true
$gameProcess = [Diagnostics.Process]::Start($gameStart)
try {
    Assert-True ((Invoke-DarkRecompUpdate -Root $root -AcceptUpdate) -eq 20) 'Running local executable blocks update'
} finally { if (!$gameProcess.HasExited) { $gameProcess.Kill(); $gameProcess.WaitForExit() } }

Write-Host "PASS: $script:TestCount updater assertions on PowerShell $($PSVersionTable.PSVersion)."
Write-Host "Test fixtures retained without recursive deletion: $testRoot"
