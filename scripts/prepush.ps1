<#
.SYNOPSIS
  Local checks to run before `git push`, so the CI does not find what a machine can find first.

.DESCRIPTION
  Same order as the quick CI: clang-format 18 over the whole repository (as the CI does), build,
  ctest without tst_controls, and clang-tidy on the files scripts/ci-tidy-files.sh picks for the
  commits not yet on origin/main. Stops at the first failure.

  tst_controls (the QML suite) is not run here: it hangs under ctest on some machines. Run its files
  one by one when you touch QML (docs/SETUP.md).

.PARAMETER Preset
  CMake preset to build and test (default windows-debug).
.PARAMETER SkipTests
  Skip ctest.
.PARAMETER SkipTidy
  Skip clang-tidy.
.PARAMETER Base
  Base of the clang-tidy file list (default origin/main).
#>
param(
    [string]$Preset = 'windows-debug',
    [switch]$SkipTests,
    [switch]$SkipTidy,
    [string]$Base = 'origin/main'
)

$ErrorActionPreference = 'Stop'
Set-Location (Split-Path $PSScriptRoot -Parent)

function Find-Tool([string]$name) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    # pip puts the Python tools here when --user is used
    $scripts = Get-ChildItem "$env:APPDATA\Python" -Directory -ErrorAction SilentlyContinue |
        ForEach-Object { Join-Path $_.FullName 'Scripts' }
    foreach ($dir in $scripts) {
        $candidate = Join-Path $dir "$name.exe"
        if (Test-Path $candidate) { return $candidate }
    }
    throw "$name not found. Install it with: python -m pip install --user $name==18.1.8"
}

function Step([string]$title) { Write-Host "`n== $title" -ForegroundColor Cyan }

# MSVC environment (as CLAUDE.md), and VSLANG so ninja records the header dependencies.
if (-not $env:VSCMD_VER) {
    $vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    & "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
    Set-Location (Split-Path $PSScriptRoot -Parent)
}
$env:VSLANG = '1033'
if (-not $env:VCPKG_ROOT) { $env:VCPKG_ROOT = 'C:\vcpkg' }

Step 'clang-format (whole repository)'
$clangFormat = Find-Tool 'clang-format'
$files = @(git ls-files '*.h' '*.cpp')
$bad = @()
for ($i = 0; $i -lt $files.Count; $i += 60) {
    $chunk = $files[$i..([Math]::Min($i + 59, $files.Count - 1))]
    $out = & $clangFormat --dry-run --Werror @chunk 2>&1
    if ($LASTEXITCODE) {
        $bad += $out | Where-Object { $_ -match 'error: code should be clang-formatted' } |
            ForEach-Object { ($_ -split ':')[0..1] -join ':' } | Select-Object -Unique
    }
}
if ($bad) { $bad | ForEach-Object { Write-Host "  needs clang-format: $_" -ForegroundColor Red }; exit 1 }
Write-Host "  $($files.Count) files formatted"

Step "build ($Preset)"
cmake --build --preset $Preset -- -j 2
if ($LASTEXITCODE) { exit $LASTEXITCODE }

if (-not $SkipTests) {
    Step 'ctest (without tst_controls)'
    ctest --preset $Preset -j 1 --timeout 120 -E '^tst_controls$'
    if ($LASTEXITCODE) { exit $LASTEXITCODE }
}

if (-not $SkipTidy) {
    Step "clang-tidy (files changed since $Base)"
    $tidy = Find-Tool 'clang-tidy'
    # Git's bash: a plain `bash` on PATH can be the WSL one.
    $bash = Join-Path (Split-Path (Split-Path (Get-Command git).Source)) 'bin\bash.exe'
    if (-not (Test-Path $bash)) { $bash = 'bash' }
    $list = & $bash scripts/ci-tidy-files.sh $Base HEAD false
    if (-not $list) {
        Write-Host '  no C++ file to analyse'
    } else {
        $list = @($list)
        Write-Host "  $($list.Count) file(s)"
        $extra = @()
        if ($env:OS -eq 'Windows_NT') { $extra = @('--extra-arg=-D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH') }
        & $tidy -p "build/$Preset" --quiet @extra @list
        if ($LASTEXITCODE) { exit $LASTEXITCODE }
    }
    Write-Host "  (the Windows clang-tidy does not see everything the Linux one does)"
}

Write-Host "`nAll local checks passed. Push once per logical step; the CI decides its level (docs/SETUP.md)." -ForegroundColor Green
