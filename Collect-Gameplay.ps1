$ErrorActionPreference = 'Stop'
$bin = Join-Path $PSScriptRoot 'out\build\win-amd64\Release'
$collector = Join-Path $bin 'asura_gameplay_collector.exe'
$gamePaths = @(
    (Join-Path $bin 'asuras_wrath.exe'),
    (Join-Path $PSScriptRoot 'out\dlss-runtime\game\asuras_wrath.exe')
)
if (!(Test-Path -LiteralPath $collector -PathType Leaf)) {
    Write-Error 'Use Build PC version in the launcher first.'
    exit 1
}
$game = Get-Process asuras_wrath -ErrorAction SilentlyContinue |
    Where-Object { $_.Path -in $gamePaths -and $_.MainWindowHandle -ne 0 } |
    Select-Object -First 1
if (!$game) {
    Write-Error 'Start the game from this launcher before opening the collector.'
    exit 1
}
& $collector $PSScriptRoot $game.Id $game.MainWindowHandle.ToInt64()
exit $LASTEXITCODE
