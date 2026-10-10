# Offline commit-updater contracts. Git uses tiny local repositories; network,
# compiler and packager I/O are replaced, while source, archive, installation,
# rollback and launcher checks execute production code. No game or UI is run.
[CmdletBinding()]
param()
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSHOME 'Modules/Microsoft.PowerShell.Utility/Microsoft.PowerShell.Utility.psd1') -Force
$project = Split-Path -Parent $PSScriptRoot
. (Join-Path $project 'tools/update_commits.ps1') -InstallRoot $project -LibraryOnly
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$testRoot = Join-Path ([IO.Path]::GetTempPath()) ('DarkRecomp-commit-tests-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($testRoot) | Out-Null
$script:TestCount = 0
$script:FixtureNumber = 0
$script:MainCommit = 'b' * 40
$script:NetworkFailure = $false
$script:BuildFailure = $false
$script:MissingPrerequisite = $false
$script:BuildCalls = 0
$script:FakeOfficialOrigin = $true
$script:CloneRemote = ''
$script:PackageTamper = $false
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
function Write-Manifest([string]$Root, [string]$Version, [string]$Commit, $Names) {
    $hashes = [ordered]@{}
    foreach ($name in $Names) { $hashes[$name] = Get-ReleaseSha256 (Join-Path $Root $name.Replace('/', '\')) }
    Write-TestFile $Root 'RELEASE.json' ([ordered]@{ version = $Version; commit = $Commit; sha256 = $hashes } | ConvertTo-Json -Depth 10)
}
function New-Installation {
    $script:FixtureNumber++
    $root = Join-Path $testRoot ('portable & files!-' + $script:FixtureNumber)
    $files = [ordered]@{
        'Launch.cmd' = "@echo off`r`necho ORIGINAL-LAUNCH ARGUMENTS: %*`r`nexit /b 37`r`n"
        'LaunchWithUpdates.cmd' = [IO.File]::ReadAllText((Join-Path $project 'LaunchWithUpdates.cmd'))
        'tools/update_release.ps1' = [IO.File]::ReadAllText((Join-Path $project 'tools/update_release.ps1'))
        'tools/update_commits.ps1' = [IO.File]::ReadAllText((Join-Path $project 'tools/update_commits.ps1'))
        'build_native/Release/DarkRecomp.exe' = 'original game'
        'build_native/Release/DarkRecompPreview.exe' = 'original preview'
        'README.md' = 'original readme'
    }
    foreach ($name in $files.Keys) { Write-TestFile $root $name $files[$name] }
    Write-Manifest $root 'v0.1.6' ('a' * 40) $files.Keys
    Write-TestFile $root 'Darkness/default.xex' 'owned executable'
    Write-TestFile $root 'Darkness/Content/data.bin' 'owned game data'
    Write-TestFile $root 'Darkness/darkness_switch_tables.toml' 'user analysis file'
    Write-TestFile $root 'saves/player.sav' 'saved progress'
    Write-TestFile $root 'DarkRecomp.settings.ini' 'user settings'
    return $root
}
function Assert-Preserved([string]$Root) {
    foreach ($pair in @(@('Darkness/default.xex', 'owned executable'), @('Darkness/Content/data.bin', 'owned game data'),
        @('Darkness/darkness_switch_tables.toml', 'user analysis file'), @('saves/player.sav', 'saved progress'), @('DarkRecomp.settings.ini', 'user settings'))) {
        Assert-True ([IO.File]::ReadAllText((Join-Path $Root $pair[0])) -ceq $pair[1]) "Preserved $($pair[0])"
    }
}

$script:RealGit = (Get-Item Function:Invoke-CommitGit).ScriptBlock
function Invoke-CommitGit([string]$Directory, [string[]]$Arguments) {
    if ($script:FakeOfficialOrigin -and $Arguments.Count -ge 3 -and $Arguments[0] -ceq 'remote' -and $Arguments[1] -ceq 'get-url') {
        return [pscustomobject]@{ ExitCode = 0; Output = 'https://github.com/portingpete/The-Darkness-Recomp.git' }
    }
    if ($Arguments[0] -ceq 'clone' -and $script:CloneRemote) {
        $Arguments = @($Arguments)
        $Arguments[$Arguments.Length - 2] = $script:CloneRemote
    }
    return (& $script:RealGit $Directory $Arguments)
}
function Get-OfficialMainCommit {
    if ($script:NetworkFailure) { throw 'offline fixture' }
    return $script:MainCommit
}
function Assert-CommitBuildPrerequisites {
    if ($script:MissingPrerequisite) { throw 'Missing compiler fixture. Install Visual Studio 2022 C++ Clang tools or use Launch.cmd.' }
}
function Invoke-SourceCommitBuild([string]$Root, [string]$Log) {
    $script:BuildCalls++
    Write-TestFile $Root 'build_native/Release/DarkRecomp.exe' ('new game ' + $script:MainCommit)
    if ($script:BuildFailure) {
        Write-TestFile $Root 'build_native/Release/new-build.dll' 'partially built DLL'
        throw 'simulated build/test failure'
    }
    Write-TestFile $Root 'build_native/Release/DarkRecompPreview.exe' ('new preview ' + $script:MainCommit)
    Write-TestFile $Root 'build_native/Release/CubeWnd.pc.ru.xcr' 'Russian menu payload'
}
function Invoke-CommitPackage([string]$Root, [string]$Output, [string]$Version, [string]$Commit) {
    [IO.Directory]::CreateDirectory($Output) | Out-Null
    $archive = Join-Path $Output "The-Darkness-Recomp-$Version-windows-x64.zip"
    $files = [ordered]@{
        'Launch.cmd' = "@echo off`r`necho UPDATED-LAUNCH ARGUMENTS: %*`r`nexit /b 37`r`n"
        'LaunchWithUpdates.cmd' = [IO.File]::ReadAllText((Join-Path $project 'LaunchWithUpdates.cmd')) + "`r`nrem changed wrapper length`r`n"
        'tools/update_release.ps1' = 'throw "Replacement release updater must not run during installation."'
        'tools/update_commits.ps1' = 'throw "Replacement commit updater must run only on the next launch."'
        'build_native/Release/DarkRecomp.exe' = 'new packaged game'
        'build_native/Release/DarkRecompPreview.exe' = 'new packaged preview'
        'build_native/Release/CubeWnd.pc.ru.xcr' = 'Russian menu payload'
        'assets/localization/README.md' = 'translation guide'
        'assets/localization/native_menu_ru.json' = '{"encoding":"cp1251"}'
        'README.md' = 'new readme'
    }
    $hashes = [ordered]@{}
    foreach ($name in $files.Keys) {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try { $hashes[$name] = ([BitConverter]::ToString($algorithm.ComputeHash([Text.Encoding]::UTF8.GetBytes($files[$name])))).Replace('-', '').ToLowerInvariant() }
        finally { $algorithm.Dispose() }
    }
    $manifest = [ordered]@{ version = $Version; commit = $Commit; sha256 = $hashes } | ConvertTo-Json -Depth 10
    $files['RELEASE.json'] = $manifest
    if ($script:PackageTamper) { $files['README.md'] = 'tampered package' }
    $zip = [IO.Compression.ZipFile]::Open($archive, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in $files.Keys) {
            $entry = $zip.CreateEntry($name)
            $stream = $entry.Open()
            $bytes = [Text.Encoding]::UTF8.GetBytes($files[$name])
            try { $stream.Write($bytes, 0, $bytes.Length) } finally { $stream.Dispose() }
        }
    } finally { $zip.Dispose() }
    [IO.File]::WriteAllText(($archive + '.sha256'), (Get-ReleaseSha256 $archive) + '  ' + [IO.Path]::GetFileName($archive) + "`n")
    return $archive
}
function Copy-ReleaseFile([string]$Source, [string]$Destination) {
    if ($script:FailCopy -and $Destination.EndsWith($script:FailCopy, [StringComparison]::OrdinalIgnoreCase)) {
        [IO.File]::WriteAllText($Destination, 'partial write')
        throw 'simulated install copy failure'
    }
    [IO.File]::Copy($Source, $Destination, $true)
}
function New-SourceFixture {
    $script:FixtureNumber++
    $root = Join-Path $testRoot ('source & files!-' + $script:FixtureNumber)
    $remote = Join-Path $testRoot ('remote-' + $script:FixtureNumber + '.git')
    [IO.Directory]::CreateDirectory($root) | Out-Null
    $null = Invoke-CheckedCommitGit $testRoot @('init', '--bare', $remote)
    $null = Invoke-CheckedCommitGit $remote @('symbolic-ref', 'HEAD', 'refs/heads/main')
    $null = Invoke-CheckedCommitGit $root @('init', '-b', 'main')
    $null = Invoke-CheckedCommitGit $root @('config', 'user.name', 'Updater Fixture')
    $null = Invoke-CheckedCommitGit $root @('config', 'user.email', 'fixture@example.invalid')
    Write-TestFile $root '.gitignore' "build_native/`nDarkness/*`n!Darkness/darkness_switch_tables.toml`n"
    Write-TestFile $root 'Darkness/darkness_switch_tables.toml' 'source analysis file'
    Write-TestFile $root 'README.md' 'first source commit'
    $null = Invoke-CheckedCommitGit $root @('add', '.')
    $null = Invoke-CheckedCommitGit $root @('commit', '-m', 'First fixture')
    $first = Invoke-CheckedCommitGit $root @('rev-parse', 'HEAD')
    $null = Invoke-CheckedCommitGit $root @('remote', 'add', 'origin', $remote)
    $null = Invoke-CheckedCommitGit $root @('push', '-u', 'origin', 'main')
    $writer = Join-Path $testRoot ('writer-' + $script:FixtureNumber)
    $null = Invoke-CheckedCommitGit $testRoot @('clone', '--', $remote, $writer)
    $null = Invoke-CheckedCommitGit $writer @('config', 'user.name', 'Updater Fixture')
    $null = Invoke-CheckedCommitGit $writer @('config', 'user.email', 'fixture@example.invalid')
    Write-TestFile $writer 'README.md' 'second source commit'
    $null = Invoke-CheckedCommitGit $writer @('add', 'README.md')
    $null = Invoke-CheckedCommitGit $writer @('commit', '-m', 'Second fixture')
    $second = Invoke-CheckedCommitGit $writer @('rev-parse', 'HEAD')
    $null = Invoke-CheckedCommitGit $writer @('push', 'origin', 'main')
    Write-TestFile $root 'build_native/Release/DarkRecomp.exe' 'original source game'
    Write-TestFile $root 'build_native/Release/DarkRecompPreview.exe' 'original source preview'
    Write-SuccessfulBuildCommit $root $first
    return [pscustomobject]@{ Root = $root; Remote = $remote; First = $first; Second = $second }
}

$actualPython = Get-CommitPythonVersion
Assert-True ($actualPython -match '^[0-9]+\.[0-9]+$') 'Production Python argv survives native Windows PowerShell 5.1 quoting'
$numericVersion = Get-CommitPackageVersion ('012345678901' + ('2' * 28))
Assert-True ((Compare-ReleaseVersion $numericVersion $numericVersion) -eq 0) 'Leading-zero numeric commit hash remains a valid package SemVer'
$codecTools = Join-Path $testRoot 'codec-prerequisite-fixture'
foreach ($relative in @('msys/usr/bin/bash.exe', 'msys/usr/bin/make.exe', 'msys/mingw64/bin/gcc.exe',
    'msys/mingw64/bin/libwinpthread-1.dll', 'msys/mingw64/share/licenses/winpthreads/COPYING', 'llvm/bin/llvm-lib.exe')) {
    Write-TestFile $codecTools $relative 'fixture tool file'
}
Assert-CommitCodecPrerequisites (Join-Path $codecTools 'msys') (Join-Path $codecTools 'llvm')
Remove-Item -LiteralPath (Join-Path $codecTools 'llvm/bin/llvm-lib.exe')
Assert-Throws { Assert-CommitCodecPrerequisites (Join-Path $codecTools 'msys') (Join-Path $codecTools 'llvm') } 'Missing standalone llvm-lib blocks fresh audio-codec builds despite Visual Studio Clang availability'

$root = New-Installation
Assert-True ((Invoke-DarkRecompCommitUpdate -Root $root -CheckOnly) -eq 2) 'Check-only finds a different main SHA without builds'
Assert-True ($script:BuildCalls -eq 0) 'Check-only does not build'
Assert-True ((Invoke-DarkRecompCommitUpdate -Root $root -NonInteractive) -eq 2) 'Noninteractive needs acceptance'
$script:NetworkFailure = $true
Assert-True ((Invoke-DarkRecompCommitUpdate -Root $root -AcceptUpdate) -eq 10) 'Offline check allows the installed build'
$script:NetworkFailure = $false
$script:MissingPrerequisite = $true
Assert-True ((Invoke-DarkRecompCommitUpdate -Root $root -AcceptUpdate) -eq 20) 'Missing compiler blocks update before mutation'
$script:MissingPrerequisite = $false
Assert-Preserved $root
$heldLock = Open-DarkRecompUpdateLock $root
try {
    Assert-Throws { Open-DarkRecompUpdateLock $root } 'Shared update lock excludes a second release or commit updater'
    Assert-True ((Invoke-DarkRecompCommitUpdate -Root $root -AcceptUpdate) -eq 20) 'Commit update honors an existing shared installation lock'
} finally { $heldLock.Dispose() }

$fixture = New-SourceFixture
$script:MainCommit = $fixture.Second
$script:FakeOfficialOrigin = $false
Assert-Throws { Assert-OfficialSourceCheckout $fixture.Root } 'Unofficial source origin is rejected'
$script:FakeOfficialOrigin = $true
Write-TestFile $fixture.Root 'README.md' 'unsaved edit'
Assert-True ((Invoke-DarkRecompCommitUpdate -Root $fixture.Root -AcceptUpdate) -eq 20) 'Dirty source is never overwritten'
Assert-True ([IO.File]::ReadAllText((Join-Path $fixture.Root 'README.md')) -ceq 'unsaved edit') 'Dirty source edit preserved'
$null = Invoke-CheckedCommitGit $fixture.Root @('restore', 'README.md')
$script:BuildFailure = $true
Assert-True ((Invoke-DarkRecompCommitUpdate -Root $fixture.Root -AcceptUpdate) -eq 20) 'Source build failure is reported'
Assert-True ((Invoke-CheckedCommitGit $fixture.Root @('rev-parse', 'HEAD')) -ceq $fixture.Second) 'Source fast-forward is retained after failed build'
Assert-True ([IO.File]::ReadAllText((Join-Path $fixture.Root 'build_native/Release/DarkRecomp.exe')) -ceq 'original source game') 'Previous program restored after partial build'
Assert-True (!(Test-Path -LiteralPath (Join-Path $fixture.Root 'build_native/Release/new-build.dll'))) 'New failed-build program removed without recursive cleanup'
Assert-True ((Get-SuccessfulBuildCommit $fixture.Root) -ceq $fixture.First) 'Failed build did not mark latest commit successful'
$script:BuildFailure = $false
Assert-True ((Invoke-DarkRecompCommitUpdate -Root $fixture.Root -AcceptUpdate) -eq 0) 'Source rebuild retries when HEAD is current but successful build is stale'
Assert-True ((Get-SuccessfulBuildCommit $fixture.Root) -ceq $fixture.Second) 'Successful source build records exact main commit'
$calls = $script:BuildCalls
Assert-True ((Invoke-DarkRecompCommitUpdate -Root $fixture.Root -AcceptUpdate) -eq 0 -and $script:BuildCalls -eq $calls) 'Current verified source build is not rebuilt'
Write-TestFile $fixture.Root 'build_native/Release/DarkRecomp.exe' 'program changed outside updater'
Assert-True ((Invoke-DarkRecompCommitUpdate -Root $fixture.Root -CheckOnly) -eq 2) 'Modified program invalidates successful-build stamp'

$divergent = New-SourceFixture
Write-TestFile $divergent.Root 'README.md' 'local divergent commit'
$null = Invoke-CheckedCommitGit $divergent.Root @('add', 'README.md')
$null = Invoke-CheckedCommitGit $divergent.Root @('commit', '-m', 'Local divergent fixture')
$local = Invoke-CheckedCommitGit $divergent.Root @('rev-parse', 'HEAD')
$script:MainCommit = $divergent.Second
Assert-True ((Invoke-DarkRecompCommitUpdate -Root $divergent.Root -AcceptUpdate) -eq 20) 'Divergent local commits are rejected without reset'
Assert-True ((Invoke-CheckedCommitGit $divergent.Root @('rev-parse', 'HEAD')) -ceq $local) 'Divergent commit preserved'

$script:MainCommit = $fixture.Second
$script:CloneRemote = $fixture.Remote
$root = New-Installation
$cache = Join-Path $testRoot ('cache-' + $script:FixtureNumber + '/source')
Assert-True ((Invoke-DarkRecompCommitUpdate -Root $root -CacheSource $cache -AcceptUpdate) -eq 0) 'Portable installation builds latest commit from a retained clone'
Assert-True ((Read-ReleaseManifest (Join-Path $root 'RELEASE.json')).commit -ceq $script:MainCommit) 'Installed manifest identifies exact built commit'
Assert-True ([IO.File]::ReadAllText((Join-Path $cache 'Darkness/Content/data.bin')) -ceq 'owned game data') 'Physical owned game copy reaches build clone'
Assert-True ([IO.File]::ReadAllText((Join-Path $cache 'Darkness/darkness_switch_tables.toml')) -ceq 'source analysis file') 'Pinned source analysis file cannot be replaced by dump copy'
Assert-True (((Get-Item -LiteralPath (Join-Path $cache 'Darkness')).Attributes -band [IO.FileAttributes]::ReparsePoint) -eq 0) 'Build-game copy is a physical directory'
Assert-Preserved $root
$calls = $script:BuildCalls
Assert-True ((Invoke-DarkRecompCommitUpdate -Root $root -AcceptUpdate) -eq 0 -and $script:BuildCalls -eq $calls) 'Current portable main commit is not rebuilt'
Write-TestFile $root 'build_native/Release/DarkRecomp.exe' 'altered installed executable'
Assert-True ((Invoke-DarkRecompCommitUpdate -Root $root -CheckOnly) -eq 2) 'Matching portable main commit still repairs an altered executable'
Write-TestFile $root 'build_native/Release/DarkRecomp.exe' 'new packaged game'
Remove-Item -LiteralPath (Join-Path $root 'build_native/Release/DarkRecompPreview.exe')
Assert-True ((Invoke-DarkRecompCommitUpdate -Root $root -CheckOnly) -eq 2) 'Matching portable main commit still repairs a missing executable'
Assert-Throws { Get-CommitCacheSource (New-Installation) $cache } 'Retained cache cannot be shared with a different installation'

foreach ($failure in @('build', 'checksum', 'copy')) {
    $root = New-Installation
    $cache = Join-Path $testRoot ('failure-cache-' + $script:FixtureNumber + '/source')
    $script:BuildFailure = $failure -ceq 'build'
    $script:PackageTamper = $failure -ceq 'checksum'
    if ($failure -ceq 'copy') { $script:FailCopy = 'README.md' }
    Assert-True ((Invoke-DarkRecompCommitUpdate -Root $root -CacheSource $cache -AcceptUpdate) -eq 20) "Portable $failure failure blocks launch"
    Assert-True ((Read-ReleaseManifest (Join-Path $root 'RELEASE.json')).commit -ceq ('a' * 40)) "$failure preserves previous installed commit"
    Assert-True ([IO.File]::ReadAllText((Join-Path $root 'build_native/Release/DarkRecomp.exe')) -ceq 'original game') "$failure preserves previous executable"
    Assert-Preserved $root
    $script:BuildFailure = $false; $script:PackageTamper = $false; $script:FailCopy = ''
}

$root = New-Installation
$outside = Join-Path $testRoot 'outside-link-target'
Write-TestFile $outside 'sentinel.txt' 'outside preserved'
New-Item -ItemType Junction -Path (Join-Path $root 'linked-game') -Target $outside | Out-Null
Assert-True ((Invoke-DarkRecompCommitUpdate -Root $root -AcceptUpdate) -eq 20) 'Non-following scan blocks linked installations'
Assert-True ([IO.File]::ReadAllText((Join-Path $outside 'sentinel.txt')) -ceq 'outside preserved') 'Junction target untouched'

# Real CMD + Windows PowerShell process, including replacement of the running
# wrapper and both scripts. Only build/network functions are fixture I/O.
$root = New-Installation
$processSource = Join-Path $testRoot 'process-source'
[IO.Directory]::CreateDirectory($processSource) | Out-Null
$version = Get-CommitPackageVersion $script:MainCommit
$processArchive = Invoke-CommitPackage $processSource (Join-Path $testRoot 'process-package') $version $script:MainCommit
$injection = @"
function Get-OfficialMainCommit { return '$($script:MainCommit)' }
function Assert-CommitBuildPrerequisites { }
function Get-CommitCacheSource { return '$($processSource.Replace("'", "''"))' }
function Sync-OfficialCommit { }
function Copy-OwnedBuildGame { }
function Invoke-SourceCommitBuild { }
function Assert-OfficialSourceCheckout { return '$($script:MainCommit)' }
function Invoke-CommitPackage { return '$($processArchive.Replace("'", "''"))' }
"@
$scriptText = [IO.File]::ReadAllText((Join-Path $project 'tools/update_commits.ps1'))
Write-TestFile $root 'tools/update_commits.ps1' ($scriptText.Replace('if (!$LibraryOnly) {', $injection + "`r`n" + 'if (!$LibraryOnly) {'))
$manifest = Read-ReleaseManifest (Join-Path $root 'RELEASE.json')
Write-Manifest $root $manifest.version $manifest.commit $manifest.sha256.PSObject.Properties.Name
$start = New-Object Diagnostics.ProcessStartInfo
$start.FileName = $env:ComSpec
$start.Arguments = '/d /c ""' + (Join-Path $root 'LaunchWithUpdates.cmd') + '" play --game-dir "dump & files!" --fps 120"'
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardInput = $true
$start.RedirectStandardOutput = $true
$start.RedirectStandardError = $true
$process = [Diagnostics.Process]::Start($start)
$process.StandardInput.Close()
$stdoutTask = $process.StandardOutput.ReadToEndAsync(); $stderrTask = $process.StandardError.ReadToEndAsync()
if (!$process.WaitForExit(60000)) { $process.Kill(); throw 'Real commit launcher fixture timed out.' }
$stdout = $stdoutTask.Result; $stderr = $stderrTask.Result
Assert-True ($process.ExitCode -eq 37) "Real CMD forwards Launch.cmd exit status: $stdout $stderr"
Assert-True ($stdout.Contains('UPDATED-LAUNCH ARGUMENTS: play --game-dir "dump & files!" --fps 120')) "Real CMD launches updated wrapper and preserves quoted arguments: $stdout"
Assert-True ((Read-ReleaseManifest (Join-Path $root 'RELEASE.json')).commit -ceq $script:MainCommit) 'Self-replacing commit updater installed requested main SHA'
Assert-Preserved $root

$root = New-Installation
$offline = 'function Get-OfficialMainCommit { throw "offline fixture" }'
Write-TestFile $root 'tools/update_commits.ps1' ($scriptText.Replace('if (!$LibraryOnly) {', $offline + "`r`n" + 'if (!$LibraryOnly) {'))
$manifest = Read-ReleaseManifest (Join-Path $root 'RELEASE.json')
Write-Manifest $root $manifest.version $manifest.commit $manifest.sha256.PSObject.Properties.Name
$start.Arguments = '/d /c ""' + (Join-Path $root 'LaunchWithUpdates.cmd') + '" play --game-dir "dump & files!" --fps 120"'
$process = [Diagnostics.Process]::Start($start)
$process.StandardInput.Close()
$stdoutTask = $process.StandardOutput.ReadToEndAsync(); $stderrTask = $process.StandardError.ReadToEndAsync()
if (!$process.WaitForExit(60000)) { $process.Kill(); throw 'Offline CMD fixture timed out.' }
$stdout = $stdoutTask.Result; $stderr = $stderrTask.Result
Assert-True ($process.ExitCode -eq 37) "Offline commit launcher permits installed game: $stdout $stderr"
Assert-True ($stdout.Contains('Update service unavailable. Starting the installed version.')) 'Offline launcher emits fallback message'
Assert-True ($stdout.Contains('ORIGINAL-LAUNCH ARGUMENTS: play --game-dir "dump & files!" --fps 120')) 'Offline launcher preserves quoted arguments'
Assert-Preserved $root

$root = New-Installation
$blocked = @'
function Get-OfficialMainCommit { return ('b' * 40) }
function Assert-CommitBuildPrerequisites { throw 'Missing compiler fixture' }
'@
Write-TestFile $root 'tools/update_commits.ps1' ($scriptText.Replace('if (!$LibraryOnly) {', $blocked + "`r`n" + 'if (!$LibraryOnly) {'))
$manifest = Read-ReleaseManifest (Join-Path $root 'RELEASE.json')
Write-Manifest $root $manifest.version $manifest.commit $manifest.sha256.PSObject.Properties.Name
$start.Arguments = '/d /c ""' + (Join-Path $root 'LaunchWithUpdates.cmd') + '" play"'
$process = [Diagnostics.Process]::Start($start)
$process.StandardInput.Close()
$stdoutTask = $process.StandardOutput.ReadToEndAsync(); $stderrTask = $process.StandardError.ReadToEndAsync()
if (!$process.WaitForExit(60000)) { $process.Kill(); throw 'Blocked CMD fixture timed out.' }
$stdout = $stdoutTask.Result; $stderr = $stderrTask.Result
Assert-True ($process.ExitCode -eq 20) "Real CMD stops after a hard update failure (exit $($process.ExitCode)): $stdout $stderr"
Assert-True (!$stdout.Contains('ORIGINAL-LAUNCH') -and !$stdout.Contains('UPDATED-LAUNCH')) 'Hard update failure never runs Launch.cmd'
Assert-Preserved $root

Write-Host "PASS: $script:TestCount commit-updater assertions on PowerShell $($PSVersionTable.PSVersion)."
Write-Host "Fixtures and reparse-point recovery evidence retained without recursive deletion: $testRoot"
