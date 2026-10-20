# Daytona USA for the original Xbox from your own ROM set, on Windows, in one
# command (from the project folder):
#
#   powershell -ExecutionPolicy Bypass -File platform\xboxog\build.ps1 C:\path\to\daytona.zip
#
# Give your MAME ROM set as a .zip, a .7z or a folder; a clone set and its
# parent may be given together. First this installs what the build needs
# and does not have yet, with winget (setup.ps1 --tools-only: Git, CMake,
# Ninja, Python 3 and the Visual Studio 2022 Build Tools with their Clang
# tools; Windows may ask for permission), then it runs build.py. RXDK is not
# installed here: install it first.
#
# The result is in platform\xboxog\dist: the "Daytona USA" folder to copy to
# the Xbox, and Daytona USA.iso.

param([Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)][string[]]$Rom)

$ErrorActionPreference = 'Stop'
$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')

# RXDK first: the tools take a while to install, and without RXDK the build
# cannot finish anyway.
$getRxdk = "Install the current RXDK with its extension for Visual Studio " +
    "(https://github.com/Team-Resurgent/RXDK-VS20XX/releases/latest) or for VS Code " +
    "(https://github.com/Team-Resurgent/RXDK-VSCode/releases/latest), then run this again."
$rxdk = if ($env:RXDK) { $env:RXDK } else { 'C:\ProgramData\RXDK' }
$cli = if ($env:RXDK_CLI) { $env:RXDK_CLI } else { Join-Path $rxdk 'engine\Rxdk.Cli.exe' }
if (-not (Test-Path $cli) -and -not (Get-Command rxdk -ErrorAction SilentlyContinue)) {
    Write-Error "RXDK is not installed (no $cli). $getRxdk"
}
# v1.3.8 or newer: older ones' DirectSound corrupts the stack. (The archived
# Team-Resurgent/RXDK installer's 1.4.0-1.4.2 are older still, whatever their
# numbers say.)
$versionFile = Join-Path $rxdk 'sdk\VERSION'
if (Test-Path $versionFile) {
    $text = (Get-Content $versionFile -Raw).Trim().TrimStart('v')
    $version = $null
    if ([version]::TryParse($text, [ref]$version)) {
        if ($version -lt [version]'1.3.8' -or ($version -ge [version]'1.4.0' -and $version -le [version]'1.4.2')) {
            Write-Error "RXDK $text is too old. $getRxdk"
        }
    }
}

# The build tools (only what is missing), then this window's PATH as a new
# terminal's would be, so they are found without opening one.
& (Join-Path $root 'setup.ps1') --tools-only
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' +
            [Environment]::GetEnvironmentVariable('Path', 'User')

# Prefer the py launcher: a bare "python" can be the Microsoft Store stub.
$python = Get-Command py -ErrorAction SilentlyContinue
if (-not $python) { $python = Get-Command python -ErrorAction SilentlyContinue }
if (-not $python) { Write-Error "Python is not on PATH yet; open a new terminal and run this again." }

& $python.Source (Join-Path $PSScriptRoot 'build.py') --rom @Rom
exit $LASTEXITCODE
