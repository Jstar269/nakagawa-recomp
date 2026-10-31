# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors
#requires -Version 7.4
#
# tools/sign_release_binaries.ps1 - Authenticode-sign the project's own Windows binaries in a
# staged release folder, on the maintainer's machine (#1284). docs/RELEASE_SIGNING.md has the
# procedure; the signing certificate and its key never enter the repository, CI or this script's
# arguments: the certificate is chosen by thumbprint from a Windows certificate store (a hardware
# token or a cloud signing provider shows up there too), or by the provider arguments of
# -ExtraSigntoolArgument.
#
# Signed: the two executables in $SignedFiles below, which this project builds. Third-party DLLs keep the
# bytes their upstream shipped (their notices say so). The showcase demo executables under bin/demos are
# not signed: each demo's completion-manifest.json lists the SHA-256 of its executable, and the player
# hides a demo whose file no longer matches. Run it after the binaries are copied into
# the staged folder and BEFORE the SBOMs are generated and the ZIP is written, so that SBOM.*,
# bin/runtime-manifest.json and SHA256SUMS.txt describe the signed bytes. The script refreshes
# bin/runtime-manifest.json itself and writes signing-record.json at the stage root.
#
# The build attestations (SLSA Build L2) cover the UNSIGNED bytes the workflow built. A signature
# changes the file's SHA-256, so `gh attestation verify` passes on the unsigned file, not on the signed
# one. signing-record.json lists both digests per file, and the release notes say so.
#
#   pwsh tools/sign_release_binaries.ps1 -Stage <staged folder> -Thumbprint <sha1> -TimestampUrl <rfc3161 url>
#   pwsh tools/sign_release_binaries.ps1 -Stage <staged folder> -VerifyOnly

[CmdletBinding()]
param(
    # The staged release folder: the one holding bin/ (the folder the ZIP is made from).
    [Parameter(Mandatory)] [string] $Stage,
    # SHA-1 thumbprint of the code-signing certificate in the CurrentUser or LocalMachine "My" store.
    [string] $Thumbprint,
    # An RFC 3161 timestamp service. Required for a release signature, so that it stays valid after the
    # certificate expires. The maintainer picks the service.
    [string] $TimestampUrl,
    # signtool.exe; default: the newest x64 copy under the Windows SDK.
    [string] $SignTool,
    # Extra signtool sign arguments, for a cloud signing provider (for example /dlib and /dmdf).
    [string[]] $ExtraSigntoolArgument = @(),
    # Check that every project file is signed and trusted, and rewrite nothing.
    [switch] $VerifyOnly,
    # Rehearsal with a self-signed certificate: no timestamp, and the trust check is skipped. The record
    # says "test certificate", and the maintainer never publishes a package made this way.
    [switch] $TestCertificate
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

# Paths are relative to the stage, with forward slashes. Both must be there.
$SignedFiles = @('bin/nakagawa_player.exe', 'bin/production_smoke.exe')
$RecordName = 'signing-record.json'

function Fail([string] $Message) { throw "sign_release_binaries: FAIL: $Message" }

function Find-SignTool {
    if ($SignTool) {
        if (-not (Test-Path -LiteralPath $SignTool -PathType Leaf)) { Fail "SIGNTOOL_MISSING: $SignTool" }
        return (Resolve-Path -LiteralPath $SignTool).Path
    }
    $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
    $found = Get-ChildItem -Path $kits -Filter signtool.exe -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.FullName -match '\\x64\\signtool\.exe$' } |
        Sort-Object { [version]($_.FullName -replace '^.*\\bin\\([0-9.]+)\\x64\\.*$', '$1') } -Descending |
        Select-Object -First 1
    if (-not $found) { Fail 'SIGNTOOL_MISSING: install the Windows SDK or pass -SignTool' }
    return $found.FullName
}

function Get-Sha256([string] $Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }

function Get-ProjectFiles([string] $Root) {
    foreach ($required in $SignedFiles) {
        if (-not (Test-Path -LiteralPath (Join-Path $Root $required) -PathType Leaf)) { Fail "SIGN_FILE_MISSING: $required is not in the stage" }
    }
    return @($SignedFiles | ForEach-Object { Get-Item -LiteralPath (Join-Path $Root $_) })
}

function Relative([string] $Root, [string] $Path) { $Path.Substring($Root.Length + 1).Replace('\', '/') }

if (-not (Test-Path -LiteralPath $Stage -PathType Container)) { Fail "SIGN_STAGE_MISSING: $Stage is not a folder" }
$Root = (Resolve-Path -LiteralPath $Stage).Path.TrimEnd('\')
$Files = Get-ProjectFiles $Root

if ($VerifyOnly) {
    foreach ($file in $Files) {
        $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
        $ok = if ($TestCertificate) { $signature.SignerCertificate } else { $signature.Status -eq 'Valid' -and $signature.TimeStamperCertificate }
        if (-not $ok) { Fail "SIGN_VERIFY: $(Relative $Root $file.FullName) is $($signature.Status), or has no timestamp" }
        Write-Output "signed: $(Relative $Root $file.FullName) by $($signature.SignerCertificate.Subject)"
    }
    return
}

if (-not $Thumbprint -and $ExtraSigntoolArgument.Count -eq 0) { Fail 'SIGN_NO_CERTIFICATE: give -Thumbprint, or the provider arguments of -ExtraSigntoolArgument' }
if (-not $TestCertificate -and -not $TimestampUrl) { Fail 'SIGN_NO_TIMESTAMP: a release signature needs -TimestampUrl (RFC 3161)' }
if ($TimestampUrl -and $TimestampUrl -notmatch '^https?://') { Fail "SIGN_TIMESTAMP_URL: $TimestampUrl is not an http(s) URL" }
$tool = Find-SignTool

# Refuse a stage that is already signed: re-signing would drop the unsigned digest from the record.
foreach ($file in $Files) {
    $existing = Get-AuthenticodeSignature -LiteralPath $file.FullName
    if ($existing.Status -ne 'NotSigned') { Fail "SIGN_ALREADY_SIGNED: $(Relative $Root $file.FullName) is $($existing.Status); start from the attested bytes" }
}
if (Test-Path -LiteralPath (Join-Path $Root $RecordName)) { Fail "SIGN_RECORD_EXISTS: $RecordName is already in the stage" }

$records = @()
$certificate = $null
foreach ($file in $Files) {
    $relative = Relative $Root $file.FullName
    $unsigned = Get-Sha256 $file.FullName
    $arguments = @('sign', '/fd', 'sha256')
    if ($Thumbprint) { $arguments += @('/sha1', $Thumbprint) }
    if ($TimestampUrl) { $arguments += @('/tr', $TimestampUrl, '/td', 'sha256') }
    $arguments += $ExtraSigntoolArgument
    $arguments += $file.FullName
    & $tool @arguments
    if ($LASTEXITCODE -ne 0) { Fail "SIGN_FAILED: signtool exited $LASTEXITCODE for $relative" }
    & $tool verify /pa /all $file.FullName 2>&1 | Out-Null
    $trusted = $LASTEXITCODE -eq 0
    if (-not $trusted -and -not $TestCertificate) { Fail "SIGN_VERIFY: signtool verify /pa failed for $relative" }
    $signature = Get-AuthenticodeSignature -LiteralPath $file.FullName
    if (-not $signature.SignerCertificate) { Fail "SIGN_VERIFY: $relative carries no signature after signing" }
    if ($certificate -and $certificate.Thumbprint -ne $signature.SignerCertificate.Thumbprint) { Fail 'SIGN_MIXED_CERTIFICATES: files were signed by different certificates' }
    $certificate = $signature.SignerCertificate
    $records += [ordered]@{
        path            = $relative
        unsigned_sha256 = $unsigned
        signed_sha256   = Get-Sha256 $file.FullName
    }
}

# bin/runtime-manifest.json lists the size and SHA-256 of every .exe and .dll in bin/: refresh it.
$python = if ($env:PYTHON) { $env:PYTHON } else { 'python' }
$tools = $PSScriptRoot
& $python (Join-Path $tools 'stage_runtime_dlls.py') --target (Join-Path $Root 'bin') --write-runtime-manifest-only
if ($LASTEXITCODE -ne 0) { Fail 'SIGN_MANIFEST: refreshing bin/runtime-manifest.json failed' }
& $python (Join-Path $tools 'stage_runtime_dlls.py') --target (Join-Path $Root 'bin') --verify-runtime-manifest
if ($LASTEXITCODE -ne 0) { Fail 'SIGN_MANIFEST: the refreshed runtime manifest does not verify' }

$record = [ordered]@{
    format           = 'nakagawa-signing-record'
    schema           = 1
    test_certificate = [bool]$TestCertificate
    signer_subject   = $certificate.Subject
    signer_thumbprint = $certificate.Thumbprint.ToLowerInvariant()
    digest_algorithm = 'sha256'
    timestamped      = [bool]$TimestampUrl
    files            = $records
}
$json = ($record | ConvertTo-Json -Depth 4) + "`n"
[System.IO.File]::WriteAllText((Join-Path $Root $RecordName), $json, [System.Text.UTF8Encoding]::new($false))
Write-Output "signed $($records.Count) file(s) as $($certificate.Subject); wrote $RecordName and refreshed bin/runtime-manifest.json"
if ($TestCertificate) { Write-Warning 'TEST CERTIFICATE: this package is a rehearsal and is never published.' }
