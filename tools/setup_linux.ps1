# Repeatable setup of the complete Windows build in an existing Ubuntu WSL2.
[CmdletBinding()]
param(
    [string]$Distro,
    [string]$InstallDir,
    [string]$SourceRoot,
    [string]$GameDirectory,
    [string]$CrtDirectory,
    [string]$UmuArchive,
    [string]$ProtonArchive,
    [switch]$Check,
    [switch]$Play,
    [switch]$SkipDependencies,
    [switch]$SkipDownload
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function ConvertTo-NativeArgument {
    param([AllowEmptyString()][string]$Value)
    # Windows CRT quoting, including quotes and trailing backslashes. Never
    # send a source path through cmd.exe or a Linux shell command string.
    # WSL parses unquoted option names before forwarding its Linux argv.
    if ($Value -ne '' -and $Value -notmatch '[\s"]') { return $Value }
    $quoted = New-Object System.Text.StringBuilder
    [void]$quoted.Append('"')
    $slashes = 0
    foreach ($character in $Value.ToCharArray()) {
        if ($character -eq [char]92) { $slashes++; continue }
        if ($character -eq [char]34) {
            [void]$quoted.Append(('\' * (2 * $slashes + 1)))
        } else {
            [void]$quoted.Append(('\' * $slashes))
        }
        [void]$quoted.Append($character)
        $slashes = 0
    }
    [void]$quoted.Append(('\' * (2 * $slashes)))
    [void]$quoted.Append('"')
    return $quoted.ToString()
}

function Invoke-SetupProcess {
    param([string]$Executable, [string[]]$Arguments, [switch]$Capture, [switch]$UnicodeOutput)
    $start = New-Object System.Diagnostics.ProcessStartInfo
    $start.FileName = $Executable
    $start.Arguments = (($Arguments | ForEach-Object { ConvertTo-NativeArgument $_ }) -join ' ')
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    # WSL's output can disappear when inherited through a double-clicked batch
    # process. Relay stdout explicitly, and drain stderr asynchronously so a
    # verbose installer cannot block on a full pipe.
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = if ($UnicodeOutput) {
        [System.Text.Encoding]::Unicode
    } else { New-Object System.Text.UTF8Encoding($false) }
    $start.StandardErrorEncoding = New-Object System.Text.UTF8Encoding($false)
    $process = New-Object System.Diagnostics.Process
    $process.StartInfo = $start
    try {
        [void]$process.Start()
        $errorTask = $process.StandardError.ReadToEndAsync()
        $output = ''
        if ($Capture) { $output = $process.StandardOutput.ReadToEnd() } else {
            while ($null -ne ($line = $process.StandardOutput.ReadLine())) { Write-Host $line }
        }
        $process.WaitForExit()
        $errorOutput = $errorTask.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0) {
            throw ('{0} failed (exit {1}). {2} {3}' -f [IO.Path]::GetFileName($Executable), $process.ExitCode, $output.Trim(), $errorOutput.Trim())
        }
        if ($errorOutput.Trim()) { Write-Host $errorOutput.TrimEnd("`r", "`n") }
        if ($Capture) { return $output.TrimEnd("`r", "`n") }
    } finally { $process.Dispose() }
}

function Invoke-SetupWsl {
    param([string[]]$Arguments, [switch]$Capture, [switch]$UnicodeOutput)
    Invoke-SetupProcess -Executable $script:SetupWslExecutable -Arguments $Arguments -Capture:$Capture -UnicodeOutput:$UnicodeOutput
}

function ConvertTo-SetupLinuxPath {
    param([string]$Value, [string]$Distribution, [switch]$AllowLinux)
    if ($AllowLinux -and $Value.StartsWith('/')) { return $Value }
    $resolved = (Resolve-Path -LiteralPath $Value).ProviderPath
    return Invoke-SetupWsl -Arguments @('-d', $Distribution, '--exec', 'wslpath', '-a', '-u', $resolved) -Capture
}

function Find-SetupCrt {
    param([string]$Root, [string]$Requested)
    $required = @('msvcp140.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
    if ($Requested) {
        $found = (Resolve-Path -LiteralPath $Requested).ProviderPath
        foreach ($name in $required) {
            if (-not (Test-Path -LiteralPath (Join-Path $found $name) -PathType Leaf)) { throw "Missing CRT file: $found\$name" }
        }
        return $found
    }
    $binary = Join-Path $Root 'build_native\Release'
    if (@($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $binary $_) -PathType Leaf) }).Count -eq 0) { return '' }
    $cache = Join-Path $Root 'build_native\CMakeCache.txt'
    if (Test-Path -LiteralPath $cache -PathType Leaf) {
        $instance = @(Get-Content -LiteralPath $cache | Where-Object { $_ -match '^CMAKE_GENERATOR_INSTANCE:INTERNAL=' })
        if ($instance.Count -eq 1) {
            $redist = Join-Path ($instance[0] -replace '^CMAKE_GENERATOR_INSTANCE:INTERNAL=', '') 'VC\Redist\MSVC'
            if (Test-Path -LiteralPath $redist -PathType Container) {
                $versions = @(Get-ChildItem -LiteralPath $redist -Directory |
                    Where-Object { $_.Name -match '^\d+(?:\.\d+){1,3}$' } |
                    Sort-Object { [version]$_.Name } -Descending)
                foreach ($version in $versions) {
                    $x64 = Join-Path $version.FullName 'x64'
                    if (-not (Test-Path -LiteralPath $x64 -PathType Container)) { continue }
                    foreach ($directory in @(Get-ChildItem -LiteralPath $x64 -Directory -Filter 'Microsoft.VC*.CRT')) {
                        if (@($required | Where-Object { -not (Test-Path -LiteralPath (Join-Path $directory.FullName $_) -PathType Leaf) }).Count -eq 0) {
                            return $directory.FullName
                        }
                    }
                }
            }
        }
    }
    throw 'The release needs its Visual C++ DLLs. Extract the complete release, or pass -CrtDirectory with the matching Visual Studio x64 CRT directory.'
}

function Read-SetupConfiguration {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $null }
    $configuration = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($configuration.schema -ne 1 -or -not ($configuration.distro -is [string]) -or
        -not ($configuration.install_dir -is [string]) -or -not $configuration.distro -or
        -not $configuration.install_dir.StartsWith('/')) {
        throw "Invalid Linux setup configuration: $Path"
    }
    return $configuration
}

function Ensure-SetupDependencies {
    param([string]$Distribution, [switch]$ReadOnly, [switch]$SkipInstall)
    $missing = $false
    $pythonVersionText = ''
    try {
        $pythonVersionText = Invoke-SetupWsl -Arguments @('-d', $Distribution, '--exec', 'python3', '--version') -Capture
    } catch { $missing = $true }
    if ($pythonVersionText -match 'Python (\d+\.\d+(?:\.\d+)?)' -and [version]$Matches[1] -lt [version]'3.10') {
        throw 'UMU requires Python 3.10 or newer. Use Ubuntu 22.04 or newer (24.04 was tested), or upgrade the distro Python before rerunning setup.'
    }
    try {
        $probe = 'import ctypes; names=["libGL.so.1","libEGL.so.1","libasound.so.2","libpulse.so.0","libvulkan.so.1"]; missing=[]' + "`n" +
            'for name in names:' + "`n" + ' try: ctypes.CDLL(name)' + "`n" + ' except OSError: missing.append(name)' + "`n" +
            'print(" ".join(missing)); raise SystemExit(bool(missing))'
        Invoke-SetupWsl -Arguments @('-d', $Distribution, '--exec', 'python3', '-c', $probe) -Capture | Out-Null
    } catch { $missing = $true }
    if (-not $missing) { return }
    if ($ReadOnly -or $SkipInstall) {
        throw 'Ubuntu needs Python 3 and graphics/audio libraries. Run SetupLinux.cmd without -Check or -SkipDependencies to install them.'
    }
    # cat is available even when Python has not been installed yet.
    $release = Invoke-SetupWsl -Arguments @('-d', $Distribution, '--exec', 'cat', '/etc/os-release') -Capture
    if ($release -notmatch '(?m)^ID=ubuntu$') { throw 'Automatic dependency installation supports Ubuntu WSL. Install Python 3 and the missing graphics/audio libraries yourself, then use -SkipDependencies.' }
    if ($release -notmatch '(?m)^VERSION_ID="?(\d+)\.' -or [int]$Matches[1] -lt 22) {
        throw 'Automatic dependency installation requires Ubuntu 22.04 or newer so its Python meets UMU requirements. Ubuntu 24.04 was tested.'
    }
    $alsa = if ($release -match '(?m)^VERSION_ID="?(24|25|26)\.') { 'libasound2t64' } else { 'libasound2' }
    Write-Host 'Installing Ubuntu Python, graphics and audio dependencies...'
    Invoke-SetupWsl -Arguments @('-d', $Distribution, '-u', 'root', '--exec', 'apt-get', 'update')
    Invoke-SetupWsl -Arguments (@('-d', $Distribution, '-u', 'root', '--exec', 'apt-get', 'install', '-y', '--no-install-recommends') +
        @('python3', 'ca-certificates', 'libgl1', 'libegl1', 'libglx-mesa0', 'libgl1-mesa-dri', 'libvulkan1', 'libpulse0', $alsa))
    Ensure-SetupDependencies -Distribution $Distribution -ReadOnly
}

function Start-LinuxSetup {
    if ($Play -and $Check) { throw 'Use -Play or -Check, not both.' }
    $root = if ($SourceRoot) { (Resolve-Path -LiteralPath $SourceRoot).ProviderPath } else { Split-Path -Parent $PSScriptRoot }
    $configPath = Join-Path $root 'build_native\linux-setup.json'
    $saved = Read-SetupConfiguration $configPath
    $distribution = if ($Distro) { $Distro } elseif ($saved) { $saved.distro } else { 'Ubuntu' }
    $wsl = Get-Command wsl.exe -ErrorAction SilentlyContinue
    if (-not $wsl) { throw 'WSL2 is required. Install Ubuntu with wsl --install -d Ubuntu from an administrator terminal, restart Windows if requested, and rerun SetupLinux.cmd.' }
    $script:SetupWslExecutable = $wsl.Source
    $distributions = @( (Invoke-SetupWsl -Arguments @('--list', '--quiet') -Capture -UnicodeOutput) -split "`r?`n" | Where-Object { $_ })
    if ($distribution -notin $distributions) {
        throw "WSL distribution '$distribution' is not installed. Run wsl --install -d Ubuntu, complete Ubuntu's first-run setup, then rerun. Installed: $($distributions -join ', ')."
    }
    $kernel = Invoke-SetupWsl -Arguments @('-d', $distribution, '--exec', 'uname', '-r') -Capture
    if ($kernel -notmatch 'WSL2') { throw "'$distribution' must use WSL2. Run wsl --set-version $distribution 2, then rerun." }
    $linuxUid = Invoke-SetupWsl -Arguments @('-d', $distribution, '--exec', 'id', '-u') -Capture
    if ($linuxUid -eq '0') { throw 'Complete Ubuntu first-run setup and configure its default non-root user before installing Proton.' }
    $target = if ($InstallDir) { $InstallDir } elseif ($saved) { $saved.install_dir } else {
        $linuxUserHome = Invoke-SetupWsl -Arguments @('-d', $distribution, '--exec', 'printenv', 'HOME') -Capture
        if (-not $linuxUserHome.StartsWith('/')) { throw 'Ubuntu must have a configured non-root user and absolute home directory.' }
        "$($linuxUserHome.TrimEnd('/'))/.local/share/darkrecomp"
    }
    if ($Play) {
        if (-not $saved -and -not $InstallDir) { throw 'Run SetupLinux.cmd first to create the Linux installation.' }
        Invoke-SetupWsl -Arguments @('-d', $distribution, '--exec', 'bash', '--noprofile', '--norc', "$($target.TrimEnd('/'))/play-linux.sh")
        return
    }
    $game = if ($GameDirectory) { $GameDirectory } else { Join-Path $root 'Darkness' }
    if (-not (Test-Path -LiteralPath (Join-Path $game 'default.xex') -PathType Leaf) -or
        -not (Test-Path -LiteralPath (Join-Path $game 'Content') -PathType Container) -or
        -not (Test-Path -LiteralPath (Join-Path $game 'System') -PathType Container)) {
        throw 'Copy your own complete supported game dump into Darkness before running setup, or pass -GameDirectory.'
    }
    $crt = Find-SetupCrt -Root $root -Requested $CrtDirectory
    Ensure-SetupDependencies -Distribution $distribution -ReadOnly:$Check -SkipInstall:$SkipDependencies
    $linuxRoot = ConvertTo-SetupLinuxPath -Value $root -Distribution $distribution
    $linuxGame = ConvertTo-SetupLinuxPath -Value $game -Distribution $distribution
    $arguments = @('-d', $distribution, '--exec', 'python3', "$linuxRoot/tools/setup_linux.py", '--source', $linuxRoot,
        '--game-dir', $linuxGame, '--install-dir', $target)
    if ($crt) { $arguments += @('--crt-dir', (ConvertTo-SetupLinuxPath -Value $crt -Distribution $distribution)) }
    if ($UmuArchive) { $arguments += @('--umu-archive', (ConvertTo-SetupLinuxPath -Value $UmuArchive -Distribution $distribution -AllowLinux)) }
    if ($ProtonArchive) { $arguments += @('--proton-archive', (ConvertTo-SetupLinuxPath -Value $ProtonArchive -Distribution $distribution -AllowLinux)) }
    if ($Check) { $arguments += '--check' }
    if ($SkipDownload) { $arguments += '--skip-download' }
    Write-Host "Checking the complete release and game for Linux setup in $target..."
    Invoke-SetupWsl -Arguments $arguments
    if (-not $Check) {
        $configuration = @{ schema = 1; distro = $distribution; install_dir = $target } | ConvertTo-Json
        $configDirectory = Split-Path -Parent $configPath
        [void](New-Item -ItemType Directory -Force -Path $configDirectory)
        $temporary = Join-Path $configDirectory ('linux-setup-' + [guid]::NewGuid().ToString('N') + '.tmp')
        [IO.File]::WriteAllText($temporary, $configuration + "`n", (New-Object System.Text.UTF8Encoding($false)))
        Move-Item -LiteralPath $temporary -Destination $configPath -Force
        Write-Host 'Setup complete. Double-click PlayLinux.cmd to play this Linux copy.'
    }
}

# Dot-sourcing exposes helpers for tests without running WSL or installing files.
if ($MyInvocation.InvocationName -ne '.') {
    try { Start-LinuxSetup } catch { Write-Error $_ -ErrorAction Continue; exit 1 }
}
