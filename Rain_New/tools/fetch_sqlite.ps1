# Download the SQLite amalgamation into third_party\ (Windows PowerShell 5+).
# If the URL ever returns 404, take the current "sqlite-amalgamation-*.zip" link from https://sqlite.org/download.html
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$ver = '3450100'; $year = '2024'
$root = Split-Path -Parent $PSScriptRoot
$zip = Join-Path $env:TEMP "sqlite-$ver.zip"
$dir = Join-Path $env:TEMP "sqlite-$ver"
Invoke-WebRequest "https://www.sqlite.org/$year/sqlite-amalgamation-$ver.zip" -OutFile $zip
Expand-Archive $zip -DestinationPath $dir -Force
Copy-Item (Join-Path $dir "sqlite-amalgamation-$ver\sqlite3.c") (Join-Path $root 'third_party') -Force
Copy-Item (Join-Path $dir "sqlite-amalgamation-$ver\sqlite3.h") (Join-Path $root 'third_party') -Force
Write-Host "SQLite $ver ready in third_party\"
