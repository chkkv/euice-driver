# Builds, packages and test-signs the ARM64 USBXpress-compatible driver.
#
# Requirements:
#   * Visual Studio 2022 (17.x) with the "Desktop development with C++" workload
#   * Windows Driver Kit 10.0.22621 (or newer) with the ARM64 toolset
#   * An ARM64 test machine (or VM) with test signing enabled
#
# Usage (elevated PowerShell):
#   .\build.ps1                 # build + stamp + makecat
#   .\build.ps1 -Sign           # additionally create and install a test cert
#
param(
    [switch]$Sign,
    [string]$Configuration = "Release"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$proj = Join-Path $PSScriptRoot "SiUsbXpArm64.vcxproj"
$out  = Join-Path $root "artifacts\ARM64\$Configuration"

# Locate msbuild through vswhere.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) { throw "vswhere.exe not found; install Visual Studio 2022." }
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild `
                      -find "MSBuild\**\Bin\MSBuild.exe" | Select-Object -First 1
if (-not $msbuild) { throw "MSBuild not found." }

Write-Host "==> Building $proj ($Configuration|ARM64)"
& $msbuild $proj /p:Configuration=$Configuration /p:Platform=ARM64 /m /v:minimal
if ($LASTEXITCODE -ne 0) { throw "Build failed." }

# INF verification + catalog generation.
$inf2cat = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter inf2cat.exe |
           Where-Object { $_.FullName -match '\\x86\\' } | Select-Object -First 1
if ($inf2cat) {
    Write-Host "==> inf2cat"
    & $inf2cat.FullName /driver:"$out" /os:10_ARM64,10_GE_ARM64
}

if ($Sign) {
    $cert = Get-ChildItem Cert:\LocalMachine\My |
            Where-Object { $_.Subject -like "CN=SiUsbXpArm64 Test*" } | Select-Object -First 1
    if (-not $cert) {
        Write-Host "==> Creating test certificate"
        $cert = New-SelfSignedCertificate -Type CodeSigningCert `
                    -Subject "CN=SiUsbXpArm64 Test" `
                    -CertStoreLocation Cert:\LocalMachine\My `
                    -HashAlgorithm SHA256
        Export-Certificate -Cert $cert -FilePath (Join-Path $out "SiUsbXpArm64Test.cer") | Out-Null
        Import-Certificate -FilePath (Join-Path $out "SiUsbXpArm64Test.cer") `
                           -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
    }
    $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe |
                Where-Object { $_.FullName -match '\\x64\\' } | Select-Object -First 1
    Write-Host "==> Signing"
    & $signtool.FullName sign /fd SHA256 /a /s My /n "SiUsbXpArm64 Test" `
        (Join-Path $out "SiUsbXpArm64.sys")
    & $signtool.FullName sign /fd SHA256 /a /s My /n "SiUsbXpArm64 Test" `
        (Join-Path $out "SiUsbXpArm64.cat")
}

Write-Host ""
Write-Host "Output: $out"
Write-Host "Install on the ARM64 target (elevated):"
Write-Host "  bcdedit /set testsigning on ; reboot"
Write-Host "  pnputil /add-driver $out\SiUsbXpArm64.inf /install"
