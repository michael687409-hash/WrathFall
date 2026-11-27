[CmdletBinding()]
param([Parameter(Mandatory)][string] $PathsFile)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'ProjectInputs.ps1')
$selection = ConvertFrom-Json -InputObject (Get-Content -LiteralPath $PathsFile -Raw -Encoding UTF8)
$paths = @($selection)
if ($paths.Count -ne 1 -or $paths[0] -isnot [string]) {
    throw 'Select one NVIDIA Streamline 2.14.16 x64 ZIP.'
}
$stream = [IO.File]::OpenRead($paths[0])
try {
    $expected = '32041E51DB0CAB709A271B72A352281B690C2ABBD669A827809401A5DD23956B'
    if ((Get-FileHash -InputStream $stream -Algorithm SHA256).Hash -ne $expected) {
        throw 'This ZIP does not match the official NVIDIA Streamline 2.14.16 x64 release. Download it from the Graphics page.'
    }
    $stream.Position = 0
    Add-Type -AssemblyName System.IO.Compression
    $archive = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Read, $true)
    try {
        $root = Join-Path $PSScriptRoot '.tools\renderer-deps\streamline-2.14.16'
        $files = @(
            'license.txt', '3rd-party-licenses.md',
            'bin/x64/nvngx_dlss.license.txt', 'bin/x64/sl.interposer.dll', 'bin/x64/sl.common.dll',
            'bin/x64/sl.pcl.dll', 'bin/x64/sl.dlss.dll', 'bin/x64/nvngx_dlss.dll'
        )
        $lock = Open-ProjectLock (Join-Path $PSScriptRoot 'out')
        try {
            foreach ($name in $files) {
                $entry = $archive.GetEntry($name)
                if (!$entry) { throw "The NVIDIA archive is missing $name." }
                $target = Join-Path $root $name
                [void][IO.Directory]::CreateDirectory((Split-Path -Parent $target))
                $pending = $target + '.' + [guid]::NewGuid().ToString('N') + '.tmp'
                try {
                    $entryStream = $entry.Open()
                    try {
                        $output = [IO.File]::Create($pending)
                        try { $entryStream.CopyTo($output) } finally { $output.Dispose() }
                    } finally { $entryStream.Dispose() }
                    if ([IO.File]::Exists($target)) { [IO.File]::Replace($pending, $target, $null) }
                    else { [IO.File]::Move($pending, $target) }
                } finally {
                    if ([IO.File]::Exists($pending)) { [IO.File]::Delete($pending) }
                }
            }
        } finally { $lock.Dispose() }
    } finally { $archive.Dispose() }
} finally { $stream.Dispose() }
Write-Output 'NVIDIA DLSS runtime installed.'

