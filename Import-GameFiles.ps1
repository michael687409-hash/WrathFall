[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Disc', 'Update', 'Dlc')]
    [string] $Kind,

    [Parameter(Mandatory)]
    [string] $PathsFile
)

$ErrorActionPreference = 'Stop'
# Keep the legacy command-line entry point while the launcher owns the import.
$launcher = Join-Path $PSScriptRoot 'WrathFall.exe'
if (!(Test-Path -LiteralPath $launcher -PathType Leaf) -and (Split-Path -Leaf $PSScriptRoot) -eq 'engine') {
    $launcher = Join-Path (Split-Path -Parent $PSScriptRoot) 'WrathFall.exe'
}
if (!(Test-Path -LiteralPath $launcher -PathType Leaf)) { throw 'Build the WrathFall launcher first.' }
$selection = (Resolve-Path -LiteralPath $PathsFile).Path
$process = Start-Process -FilePath $launcher -ArgumentList '--import-worker', $Kind, ('"{0}"' -f $selection) `
    -NoNewWindow -Wait -PassThru
if ($process.ExitCode -ne 0) { throw "Game file import exited with code $($process.ExitCode)." }
