param(
  [Parameter(Mandatory=$false)] [string] $Path,
  [Parameter(Mandatory=$false)] [string] $Url
)

if (-not $Path -and -not $Url) {
  Write-Error "Provide either -Path <file> or -Url <download-url>"
  exit 2
}
$cleanup = $null
if ($Url) {
  $tmp = [System.IO.Path]::GetTempFileName()
  $tmpZip = "$tmp.bin"
  Invoke-WebRequest -Uri $Url -OutFile $tmpZip -UseBasicParsing -ErrorAction Stop
  $Path = $tmpZip
  $cleanup = $tmpZip
}
try {
  $hash = Get-FileHash -Path $Path -Algorithm SHA256
  Write-Output $hash.Hash.ToLower()
} finally {
  if ($cleanup -and (Test-Path $cleanup)) { Remove-Item $cleanup -Force }
}
