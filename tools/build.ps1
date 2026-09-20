param([switch]$Diagnostic, [switch]$ReleasePackage, [int]$Jobs = 4)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path -Parent $PSScriptRoot
$generatorRoot = Join-Path $projectRoot 'refs/UnleashedRecomp/tools/XenonRecomp'
$generatorBuild = Join-Path $generatorRoot 'build'
$nativeBuild = Join-Path $projectRoot 'build_native'
function Invoke-Checked([string]$Executable, [string[]]$Arguments) {
    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Executable failed with exit code $LASTEXITCODE" }
}
Invoke-Checked python @((Join-Path $PSScriptRoot 'setup_generator.py'))
Invoke-Checked cmake @('-S', $generatorRoot, '-B', $generatorBuild, '-G', 'Visual Studio 17 2022', '-A', 'x64')
Invoke-Checked cmake @('--build', $generatorBuild, '--config', 'Release', '--target', 'XenonRecomp', '--parallel', "$Jobs")
$generateArgs = @((Join-Path $PSScriptRoot 'recompile.py'), '--output', (Join-Path $nativeBuild 'generated'),
    '--generator', (Join-Path $generatorBuild 'XenonRecomp/Release/XenonRecomp.exe'))
if ($Diagnostic) { $generateArgs += '--allow-incomplete' }
Invoke-Checked python $generateArgs
$codecArgs = @((Join-Path $PSScriptRoot 'build_xma_codec.py'))
if ($ReleasePackage) { $codecArgs += '--rebuild' }
Invoke-Checked python $codecArgs
$allowIncomplete = if ($Diagnostic) { 'ON' } else { 'OFF' }
Invoke-Checked cmake @('-S', $projectRoot, '-B', $nativeBuild, '-G', 'Visual Studio 17 2022', '-A', 'x64', '-T', 'ClangCL', '-DBUILD_TESTING=ON', "-DDARK_ALLOW_INCOMPLETE=$allowIncomplete")
Invoke-Checked cmake @('--build', $nativeBuild, '--config', 'Release', '--parallel', "$Jobs")
Invoke-Checked ctest @('--test-dir', $nativeBuild, '-C', 'Release', '--output-on-failure', '--no-tests=error')
