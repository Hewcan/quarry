<#
.SYNOPSIS
  Builds the Windows packages of Quarry from the tree `cmake --install` laid out (H10 phase 2).

.DESCRIPTION
  - Inno Setup installer: Quarry-<version>-x64-setup.exe
  - MSIX package:         Quarry-<version>-x64.msix

  Nothing here uses a real signing certificate. With -TestSign the MSIX is signed with a throwaway
  self-signed certificate created for this run and deleted at the end; only its public part
  (Quarry-test.cer) is kept, so a tester can trust it and install the package. Without it the MSIX is
  left unsigned, which Windows refuses to install.

.PARAMETER TreeDir
  The installed tree (the directory holding bin\, LICENSE, THIRD_PARTY_LICENSES.txt).
.PARAMETER OutDir
  Where the packages go (default: installers next to the repository's build directory).
.PARAMETER Version
  x.y.z; default: the version of project(quarry ...) in CMakeLists.txt.
.PARAMETER TestSign
  Sign the MSIX with a throwaway certificate and keep its .cer.
.PARAMETER SkipInno
  Do not build the Inno Setup installer.
.PARAMETER SkipMsix
  Do not build the MSIX package.
.PARAMETER BudgetMB
  Size budget of the installer (docs/PERFORMANCE.md: 45 MB). Over it, the script fails.
#>
param(
    [Parameter(Mandatory = $true)][string]$TreeDir,
    [string]$OutDir,
    [string]$Version,
    [switch]$TestSign,
    [switch]$SkipInno,
    [switch]$SkipMsix,
    [int]$BudgetMB = 45
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$TreeDir = (Resolve-Path $TreeDir).Path
if (-not (Test-Path (Join-Path $TreeDir 'bin\quarry.exe'))) { throw "No bin\quarry.exe in $TreeDir" }
if (-not $OutDir) { $OutDir = Join-Path $repo 'build\installers' }
New-Item -ItemType Directory -Force $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

if (-not $Version) {
    $m = Select-String -Path (Join-Path $repo 'CMakeLists.txt') -Pattern 'project\(quarry VERSION (\d+\.\d+\.\d+)'
    if (-not $m) { throw 'Could not read the version from CMakeLists.txt' }
    $Version = $m.Matches[0].Groups[1].Value
}
Write-Host "Quarry $Version from $TreeDir"

function Find-Tool([string]$exe, [string[]]$globs) {
    $cmd = Get-Command $exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    foreach ($g in $globs) {
        $hit = Get-ChildItem $g -ErrorAction SilentlyContinue | Sort-Object FullName | Select-Object -Last 1
        if ($hit) { return $hit.FullName }
    }
    throw "$exe not found"
}

# ---- Inno Setup --------------------------------------------------------------------------------
if (-not $SkipInno) {
    $iscc = Find-Tool 'ISCC.exe' @(
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "$env:ProgramFiles\Inno Setup 6\ISCC.exe")
    Write-Host "`n== Inno Setup ($iscc)"
    & $iscc /Q "/DAppVersion=$Version" "/DSourceDir=$TreeDir" "/DOutputDir=$OutDir" (Join-Path $PSScriptRoot 'quarry.iss')
    if ($LASTEXITCODE) { throw "ISCC failed: $LASTEXITCODE" }
    $setup = Join-Path $OutDir "Quarry-$Version-x64-setup.exe"
    $mb = (Get-Item $setup).Length / 1MB
    Write-Host ("  {0}  {1:N1} MB (budget {2} MB)" -f (Split-Path $setup -Leaf), $mb, $BudgetMB)
    if ($mb -gt $BudgetMB) { throw ("The installer is {0:N1} MB, over the {1} MB budget" -f $mb, $BudgetMB) }
}

# ---- MSIX ---------------------------------------------------------------------------------------
if (-not $SkipMsix) {
    $makeappx = Find-Tool 'makeappx.exe' @("${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\makeappx.exe")
    Write-Host "`n== MSIX ($makeappx)"
    # The subject of the certificate that signs the package must equal the publisher of the manifest.
    $publisher = 'CN=Quarry Test'
    $stage = Join-Path ([IO.Path]::GetTempPath()) ("quarry-msix-" + [Guid]::NewGuid().ToString('N'))
    try {
        Copy-Item $TreeDir $stage -Recurse
        Copy-Item (Join-Path $PSScriptRoot 'msix-assets') (Join-Path $stage 'Assets') -Recurse
        $manifest = Get-Content (Join-Path $PSScriptRoot 'AppxManifest.xml.in') -Raw
        $manifest = $manifest.Replace('@VERSION4@', "$Version.0").Replace('@PUBLISHER@', $publisher)
        # UTF-8 without BOM, as makeappx expects.
        [IO.File]::WriteAllText((Join-Path $stage 'AppxManifest.xml'), $manifest, (New-Object Text.UTF8Encoding $false))

        $msix = Join-Path $OutDir "Quarry-$Version-x64.msix"
        if (Test-Path $msix) { Remove-Item $msix }
        & $makeappx pack /d $stage /p $msix /o | Out-Null
        if ($LASTEXITCODE) { throw "makeappx pack failed: $LASTEXITCODE" }

        if ($TestSign) {
            $signtool = Find-Tool 'signtool.exe' @("${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe")
            $cert = New-SelfSignedCertificate -Type Custom -Subject $publisher -KeyUsage DigitalSignature `
                -FriendlyName 'Quarry test signing (temporary)' -CertStoreLocation 'Cert:\CurrentUser\My' `
                -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3', '2.5.29.19={text}') -NotAfter (Get-Date).AddDays(30)
            $pfx = Join-Path ([IO.Path]::GetTempPath()) ("quarry-test-" + [Guid]::NewGuid().ToString('N') + '.pfx')
            $password = [Guid]::NewGuid().ToString('N')
            try {
                Export-PfxCertificate -Cert $cert -FilePath $pfx -Password (ConvertTo-SecureString $password -AsPlainText -Force) | Out-Null
                Export-Certificate -Cert $cert -FilePath (Join-Path $OutDir 'Quarry-test.cer') | Out-Null
                & $signtool sign /fd SHA256 /f $pfx /p $password $msix | Out-Null
                if ($LASTEXITCODE) { throw "signtool failed: $LASTEXITCODE" }
            } finally {
                # The private key never outlives this run.
                Remove-Item $pfx -ErrorAction SilentlyContinue
                Remove-Item "Cert:\CurrentUser\My\$($cert.Thumbprint)" -ErrorAction SilentlyContinue
            }
            Write-Host '  signed with a throwaway certificate; Quarry-test.cer is its public part'
        } else {
            Write-Host '  unsigned (Windows will not install it)'
        }
        Write-Host ("  {0}  {1:N1} MB" -f (Split-Path $msix -Leaf), ((Get-Item $msix).Length / 1MB))

        # The package must unpack back to what went in: a malformed manifest fails here.
        $check = Join-Path ([IO.Path]::GetTempPath()) ("quarry-msix-check-" + [Guid]::NewGuid().ToString('N'))
        try {
            & $makeappx unpack /p $msix /d $check /o | Out-Null
            if ($LASTEXITCODE) { throw "makeappx unpack failed: $LASTEXITCODE" }
            if (-not (Test-Path (Join-Path $check 'bin\quarry.exe'))) { throw 'bin\quarry.exe missing from the MSIX' }
            if (-not (Test-Path (Join-Path $check 'AppxManifest.xml'))) { throw 'AppxManifest.xml missing from the MSIX' }
        } finally { Remove-Item $check -Recurse -Force -ErrorAction SilentlyContinue }
    } finally { Remove-Item $stage -Recurse -Force -ErrorAction SilentlyContinue }
}

Write-Host "`nPackages in $OutDir"
Get-ChildItem $OutDir | Where-Object { $_.Name -like 'Quarry-*' } | ForEach-Object { Write-Host ("  {0}  {1:N1} MB" -f $_.Name, ($_.Length / 1MB)) }
