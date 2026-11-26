$discHash = 'C7E324A807A4407239B8FBFABE4377C0623889DAA772EADAC42C8B8F86782479'
$patchHash = '918CDEE90389F27DEE0BFE1BDFD758BC52CC4148666DCAE1E35A1C0297F2005C'

function Get-ProjectFileHash([string] $Path) {
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try {
        $file = [IO.File]::OpenRead($Path)
        try { [BitConverter]::ToString($algorithm.ComputeHash($file)).Replace('-', '') }
        finally { $file.Dispose() }
    } finally { $algorithm.Dispose() }
}
function Confirm-File([string] $Path, [string] $Hash) {
    (Test-Path -LiteralPath $Path -PathType Leaf) -and
        (Get-ProjectFileHash $Path) -eq $Hash
}

function Initialize-PlayerData([string] $ProjectRoot) {
    $destination = Join-Path $ProjectRoot 'userdata'
    if (Test-Path -LiteralPath $destination -PathType Container) { return $destination }

    $legacy = Join-Path $ProjectRoot 'out\user'
    if (!(Test-Path -LiteralPath $legacy -PathType Container)) {
        [void][IO.Directory]::CreateDirectory($destination)
        return $destination
    }

    # Publish the migrated profile only after the whole copy has been verified.
    $pending = "$destination.pending.$([Guid]::NewGuid().ToString('N'))"
    Copy-Item -LiteralPath $legacy -Destination $pending -Recurse -Force -ErrorAction Stop
    foreach ($file in Get-ChildItem -LiteralPath $legacy -Recurse -Force -File) {
        $relative = $file.FullName.Substring($legacy.Length + 1)
        if (!(Confirm-File (Join-Path $pending $relative) (Get-ProjectFileHash $file.FullName))) {
            throw "Could not verify the copied player file $relative. The original is in $legacy."
        }
    }
    [IO.Directory]::Move($pending, $destination)
    return $destination
}

function Install-Patch([string] $Game, [string] $Update) {
    $source = Join-Path $Update 'default.xexp'
    $target = Join-Path $Game 'default.xexp'
    if (!(Confirm-File (Join-Path $Game 'default.xex') $discHash) -or !(Confirm-File $source $patchHash)) {
        return
    }
    if (Test-Path -LiteralPath $target) {
        if (!(Confirm-File $target $patchHash)) { throw "An unexpected patch already exists at $target." }
        return
    }
    $pending = Join-Path $Game ('default.xexp.' + [Guid]::NewGuid().ToString('N'))
    try {
        Copy-Item -LiteralPath $source -Destination $pending
        [IO.File]::Move($pending, $target)
        'TU1 is ready beside the base executable.'
    } finally {
        if (Test-Path -LiteralPath $pending -PathType Leaf) { Remove-Item -LiteralPath $pending }
    }
}

function Open-ProjectLock([string] $OutputDirectory) {
    New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
    try {
        [IO.File]::Open((Join-Path $OutputDirectory '.active'), 'OpenOrCreate', 'ReadWrite', 'None')
    } catch [IO.IOException] {
        throw 'Another import, build, or game session is using this folder.'
    }
}

