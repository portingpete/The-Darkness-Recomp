param([string]$PythonExecutable)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '..\tools\setup_linux.ps1')

function Assert-SetupTest([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
}

if (-not $PythonExecutable) { $PythonExecutable = (Get-Command python.exe).Source }
$values = @('', 'plain', 'spaces & punctuation! $()', 'quote"inside', 'ends\', 'path with spaces\',
    'several\\"quotes', "line`nbreak", ('Unicode-' + [char]0xf1))
$probe = 'import json,sys; print(json.dumps(sys.argv[1:],ensure_ascii=True))'
$received = Invoke-SetupProcess -Executable $PythonExecutable -Arguments (@('-c', $probe) + $values) -Capture | ConvertFrom-Json
Assert-SetupTest ($received.Count -eq $values.Count) 'Windows argument count changed.'
for ($index = 0; $index -lt $values.Count; $index++) {
    Assert-SetupTest ($received[$index] -ceq $values[$index]) "Windows argv changed at $index."
}
$failed = $false
try { Invoke-SetupProcess -Executable $PythonExecutable -Arguments @('-c', 'import sys; sys.stderr.write("setup failure detail\n" + "x"*131072); raise SystemExit(37)') -Capture } catch {
    $failed = $_.Exception.Message -match 'exit 37' -and $_.Exception.Message -match 'setup failure detail'
}
Assert-SetupTest $failed 'Nonzero child status or stderr diagnostics were lost.'
$relayed = @(& { Invoke-SetupProcess -Executable $PythonExecutable -Arguments @('-c', 'print("visible setup progress")') } 6>&1)
Assert-SetupTest (($relayed | ForEach-Object { $_.ToString() }) -contains 'visible setup progress') 'Installer progress was not relayed to the Windows console.'

$fixture = Join-Path ([IO.Path]::GetTempPath()) ('dark-linux-setup-' + [guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $fixture)
# No recursive cleanup: remove only the exact files/directories made below.
$binary = Join-Path $fixture 'build_native\Release'
[void](New-Item -ItemType Directory -Path $binary)
$crtFiles = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
$configurationPath = Join-Path $fixture 'linux-setup.json'
$cachePath = Join-Path $fixture 'build_native\CMakeCache.txt'
$visualStudio = Join-Path $fixture 'Visual Studio'
$redistRoot = Join-Path $visualStudio 'VC\Redist\MSVC'
$redist = Join-Path $redistRoot '14.44.35112\x64\Microsoft.VC143.CRT'
try {
    foreach ($file in $crtFiles) { [IO.File]::WriteAllText((Join-Path $binary $file), 'fixture') }
    Assert-SetupTest ((Find-SetupCrt -Root $fixture -Requested '') -ceq '') 'Bundled CRT was not preferred.'
    Assert-SetupTest ((Find-SetupCrt -Root $fixture -Requested $binary) -ceq $binary) 'Explicit CRT was not selected.'
    Remove-Item -LiteralPath (Join-Path $binary 'msvcp140.dll')
    $failed = $false
    try { Find-SetupCrt -Root $fixture -Requested $binary | Out-Null } catch { $failed = $_.Exception.Message -match 'msvcp140' }
    Assert-SetupTest $failed 'Incomplete explicit CRT was accepted.'
    [void](New-Item -ItemType Directory -Path $redist)
    [void](New-Item -ItemType Directory -Path (Join-Path $redistRoot 'v143'))
    foreach ($file in $crtFiles) { [IO.File]::WriteAllText((Join-Path $redist $file), 'fixture CRT') }
    [IO.File]::WriteAllText($cachePath, 'CMAKE_GENERATOR_INSTANCE:INTERNAL=' + $visualStudio)
    Assert-SetupTest ((Find-SetupCrt -Root $fixture -Requested '') -ceq $redist) 'CRT discovery failed alongside the v143 alias directory.'
    Assert-SetupTest ($null -eq (Read-SetupConfiguration $configurationPath)) 'Missing config did not stay absent.'
    [IO.File]::WriteAllText($configurationPath, '{"schema":1,"distro":"Ubuntu","install_dir":"/home/user/a space & quote''s"}')
    $saved = Read-SetupConfiguration $configurationPath
    Assert-SetupTest ($saved.install_dir -ceq "/home/user/a space & quote's") 'Install path was rewritten.'
    $unicodePath = '/home/user/' + [char]0xf1 + ' game'
    $unicodeConfiguration = @{ schema = 1; distro = 'Ubuntu'; install_dir = $unicodePath } | ConvertTo-Json
    [IO.File]::WriteAllText($configurationPath, $unicodeConfiguration, (New-Object System.Text.UTF8Encoding($false)))
    Assert-SetupTest ((Read-SetupConfiguration $configurationPath).install_dir -ceq $unicodePath) 'UTF-8 setup config changed a Unicode install path.'
    [IO.File]::WriteAllText($configurationPath, '{"schema":99,"distro":"Ubuntu","install_dir":"/home/user"}')
    $failed = $false
    try { Read-SetupConfiguration $configurationPath | Out-Null } catch { $failed = $true }
    Assert-SetupTest $failed 'Unknown config schema was accepted.'
    [IO.File]::WriteAllText($configurationPath, '{"schema":1,"distro":"Ubuntu","install_dir":"C:\\unsafe"}')
    $failed = $false
    try { Read-SetupConfiguration $configurationPath | Out-Null } catch { $failed = $true }
    Assert-SetupTest $failed 'Windows install path was accepted as Linux.'
} finally {
    foreach ($file in $crtFiles) {
        $path = Join-Path $binary $file
        if (Test-Path -LiteralPath $path -PathType Leaf) { Remove-Item -LiteralPath $path }
    }
    if (Test-Path -LiteralPath $configurationPath -PathType Leaf) { Remove-Item -LiteralPath $configurationPath }
    if (Test-Path -LiteralPath $cachePath -PathType Leaf) { Remove-Item -LiteralPath $cachePath }
    if (Test-Path -LiteralPath $redist -PathType Container) {
        foreach ($file in $crtFiles) { Remove-Item -LiteralPath (Join-Path $redist $file) }
        Remove-Item -LiteralPath $redist
        Remove-Item -LiteralPath (Split-Path -Parent $redist)
        Remove-Item -LiteralPath (Join-Path $redistRoot '14.44.35112')
        Remove-Item -LiteralPath (Join-Path $redistRoot 'v143')
        Remove-Item -LiteralPath $redistRoot
        Remove-Item -LiteralPath (Join-Path $visualStudio 'VC\Redist')
        Remove-Item -LiteralPath (Join-Path $visualStudio 'VC')
        Remove-Item -LiteralPath $visualStudio
    }
    Remove-Item -LiteralPath $binary
    Remove-Item -LiteralPath (Join-Path $fixture 'build_native')
    Remove-Item -LiteralPath $fixture
}
$originalWslHelper = (Get-Item Function:\Invoke-SetupWsl).ScriptBlock
try {
    function Invoke-SetupWsl {
        param([string[]]$Arguments, [switch]$Capture, [switch]$UnicodeOutput)
        if ($Arguments[-1] -eq '--version') { return 'Python 3.8.10' }
        throw 'Unexpected WSL operation while rejecting an old Python.'
    }
    $failed = $false
    try { Ensure-SetupDependencies -Distribution Ubuntu } catch {
        $failed = $_.Exception.Message -match 'Python 3.10 or newer'
    }
    Assert-SetupTest $failed 'Unsupported Python was accepted or triggered package installation.'
} finally { Set-Item Function:\Invoke-SetupWsl -Value $originalWslHelper }
Write-Host 'Windows setup helpers passed: native argv, exit status, CRT selection, config validation, Python requirement.'
