param(
    [string]$RemotePath = "host0:/nakagawa_psp_oracle.prx",
    [int]$WaitSeconds = 16,
    [string]$PspshExe = ""
)

if (-not $PspshExe) {
    if (Get-Command pspsh.exe -ErrorAction SilentlyContinue) {
        $PspshExe = "pspsh.exe"
    } elseif ($env:PSPLINK_DIR -and (Test-Path (Join-Path $env:PSPLINK_DIR "pspsh.exe"))) {
        $PspshExe = Join-Path $env:PSPLINK_DIR "pspsh.exe"
    } else {
        $userProfile = $env:USERPROFILE
        $PspshExe = Join-Path $userProfile "Documents\PSPHacks\psplinkusb-windows\pspsh.exe"
    }
}

& {
    Start-Sleep -Seconds 2
    Write-Output "ldstart $RemotePath"
    Start-Sleep -Seconds $WaitSeconds
} | & $PspshExe
