# Builds both apps and collects the release files in .\release
#   powershell -ExecutionPolicy Bypass -File build_release.ps1 [-Version 1.0.0] [-VersionCode 1]
param([string]$Version = '1.0.0', [int]$VersionCode = 1)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

Write-Host '== Windows ==' -ForegroundColor Cyan
cmd /c "windows\build.cmd"
if ($LASTEXITCODE -ne 0) { throw 'Windows build failed' }

Write-Host '== Android ==' -ForegroundColor Cyan
$env:VERSION_NAME = $Version
$env:VERSION_CODE = "$VersionCode"
# Prefer Git Bash: C:\Windows\System32\bash.exe is WSL, which can't run the Windows SDK tools.
$bash = "$env:ProgramFiles\Git\bin\bash.exe"
if (-not (Test-Path $bash)) { $bash = (Get-Command bash -ErrorAction Stop).Source }
& $bash android/build_apk.sh
if ($LASTEXITCODE -ne 0) { throw 'Android build failed' }

Write-Host '== Release ==' -ForegroundColor Cyan
New-Item -ItemType Directory -Force release | Out-Null
Copy-Item windows\bin\PhoneUnlockSetup.exe release\PhoneUnlockSetup.exe -Force
Copy-Item android\build\PhoneUnlock.apk release\PhoneUnlock.apk -Force
Get-FileHash release\PhoneUnlockSetup.exe, release\PhoneUnlock.apk -Algorithm SHA256 |
    ForEach-Object { '{0}  {1}' -f $_.Hash.ToLower(), (Split-Path $_.Path -Leaf) } |
    Set-Content release\SHA256SUMS.txt -Encoding ascii
Get-ChildItem release | Format-Table Name, Length -AutoSize
