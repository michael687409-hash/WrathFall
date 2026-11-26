[CmdletBinding()]
param(
    [Parameter(Mandatory)]
    [ValidateSet('Disc', 'Update', 'Dlc')]
    [string] $Kind,

    [Parameter(Mandatory)]
    [string] $PathsFile
)

$ErrorActionPreference = 'Stop'
$local = Join-Path $PSScriptRoot 'local'
$game = Join-Path $local 'game'
$update = Join-Path $local 'update'
$dlc = Join-Path $local 'dlc'
$rex = Join-Path $PSScriptRoot '.tools\rexglue-0.11.0\win-amd64\bin\rexglue.exe'
. (Join-Path $PSScriptRoot 'ProjectInputs.ps1')
$paths = @(Get-Content -LiteralPath $PathsFile -Raw | ConvertFrom-Json)
if ($paths.Count -eq 0) { throw 'Select at least one file.' }

function Expand-Container([string] $Source, [string] $Destination) {
    & $rex vfs dump $Source $Destination
    if ($LASTEXITCODE -ne 0) { throw "ReXGlue could not unpack $Source." }
}

function Expand-ZipPackage([string] $Archive, [string] $Pattern, [string] $Destination) {
    $zip = [IO.Compression.ZipFile]::OpenRead($Archive)
    try {
        $packages = @($zip.Entries | Where-Object { $_.FullName -match $Pattern })
        if ($packages.Count -ne 1) { throw "Expected one matching package in $Archive." }
        $entry = $packages[0]
        $file = Join-Path $Destination $entry.Name
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $file, $false)
        if ((Get-Item -LiteralPath $file).Length -ne $entry.Length) { throw "Incomplete package: $Archive" }
        $file
    } finally { $zip.Dispose() }
}

function Confirm-Dlc([string] $Path) {
    $stream = [IO.File]::OpenRead($Path)
    try {
        $header = New-Object byte[] 0x364
        for ($offset = 0; $offset -lt $header.Length; $offset += $read) {
            $read = $stream.Read($header, $offset, $header.Length - $offset)
            if (!$read) { throw "Incomplete DLC header: $Path" }
        }
        if ([Text.Encoding]::ASCII.GetString($header, 0, 4) -notin 'LIVE','PIRS','CON ' -or
            [BitConverter]::ToString($header, 0x344, 4) -ne '00-00-00-02' -or
            [BitConverter]::ToString($header, 0x360, 4) -ne '43-43-08-17') {
            throw "Expected Asura's Wrath Xbox 360 DLC: $Path"
        }
    } finally { $stream.Dispose() }
}

if ($Kind -ne 'Dlc' -and $paths.Count -ne 1) { throw 'Select one disc or update file at a time.' }
Add-Type -AssemblyName System.IO.Compression.FileSystem
$staging = Join-Path $local ('import-' + [Guid]::NewGuid().ToString('N'))
$lock = Open-ProjectLock (Join-Path $PSScriptRoot 'out')

try {
    New-Item -ItemType Directory -Path $staging -Force | Out-Null
    foreach ($path in $paths) {
        $file = Get-Item -LiteralPath $path
        if ($file.PSIsContainer) { throw "Expected a file: $path" }
        $inputPath = $file.FullName
        switch ($Kind) {
            'Disc' {
                if (Confirm-File (Join-Path $game 'default.xex') $discHash) {
                    'The supported base game is already staged.'
                    break
                }
                if (Test-Path -LiteralPath $game) { throw "Move the existing game folder before importing: $game" }
                if ($file.Extension -ieq '.7z') {
                    $entries = @(& tar.exe -tf $inputPath)
                    if ($LASTEXITCODE -ne 0 -or $entries.Count -ne 1 -or $entries[0] -notmatch '^[^/\\:]+\.iso$') {
                        throw 'Select a 7z archive containing one Xbox 360 disc ISO.'
                    }
                    & tar.exe -xf $inputPath -C $staging -- $entries[0]
                    if ($LASTEXITCODE -ne 0) { throw 'Disc archive extraction failed.' }
                    $inputPath = Join-Path $staging $entries[0]
                } elseif ($file.Extension -ine '.iso') {
                    throw 'Select an Xbox 360 ISO or a 7z archive containing the ISO.'
                }
                $expanded = Join-Path $staging 'disc'
                Expand-Container $inputPath $expanded
                if (!(Confirm-File (Join-Path $expanded 'default.xex') $discHash)) {
                    throw 'This project targets the disc revision with media ID 11DFADD6.'
                }
                [IO.Directory]::Move($expanded, $game)
                'Verified and imported the Xbox 360 disc.'
            }
            'Update' {
                if (Confirm-File (Join-Path $update 'default.xexp') $patchHash) {
                    'The matching TU1 patch is already staged.'
                    break
                }
                if (Test-Path -LiteralPath $update) { throw "Move the existing update folder before importing: $update" }
                $expanded = Join-Path $staging 'update'
                if ($file.Extension -ieq '.zip') {
                    $inputPath = Expand-ZipPackage $inputPath '^(Cache/)?TU_[A-Z0-9_.]+$' $staging
                }
                if ($file.Extension -ieq '.xexp') {
                    New-Item -ItemType Directory -Path $expanded | Out-Null
                    Copy-Item -LiteralPath $inputPath -Destination (Join-Path $expanded 'default.xexp')
                } else {
                    Expand-Container $inputPath $expanded
                }
                if (!(Confirm-File (Join-Path $expanded 'default.xexp') $patchHash)) {
                    throw 'The update does not match this disc revision. Use TU1 for media ID 11DFADD6.'
                }
                [IO.Directory]::Move($expanded, $update)
                'Verified and imported Asura''s Wrath TU1.'
            }
            'Dlc' {
                if ($file.Extension -ieq '.zip') {
                    $inputPath = Expand-ZipPackage $inputPath '^(43430817/00000002/)?[A-Fa-f0-9]{42}$' $staging
                }
                $name = [IO.Path]::GetFileName($inputPath).ToUpperInvariant()
                if ($name -notmatch '^[A-F0-9]{42}$') { throw 'Use the original 42-character Xbox content package filename.' }
                Confirm-Dlc $inputPath
                New-Item -ItemType Directory -Path $dlc -Force | Out-Null
                $destination = Join-Path $dlc $name
                if (Test-Path -LiteralPath $destination) {
                    $incomingHash = Get-ProjectFileHash $inputPath
                    if (!(Confirm-File $destination $incomingHash)) { throw "A different package already uses $name." }
                    "Already imported $name."
                    break
                }
                $pending = Join-Path $staging ($name + '.ready')
                Copy-Item -LiteralPath $inputPath -Destination $pending
                [IO.File]::Move($pending, $destination)
                "Imported $name."
            }
        }
    }
    Install-Patch $game $update
} finally {
    $lock.Dispose()
    $owned = Get-Item -LiteralPath $staging -ErrorAction SilentlyContinue
    if ($owned -and $owned.FullName -eq $staging -and !$owned.LinkType) {
        Remove-Item -LiteralPath $owned.FullName -Force -Recurse
    }
}

