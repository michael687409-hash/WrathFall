param(
    [string] $OutputDirectory = (Join-Path $PSScriptRoot '../..')
)

$ErrorActionPreference = 'Stop'
$framework = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319'
$compiler = Join-Path $framework 'csc.exe'
if (!(Test-Path -LiteralPath $compiler -PathType Leaf)) {
    throw 'The Windows .NET Framework 4.x compiler is required to build the launcher.'
}

$output = (New-Item -Path $OutputDirectory -ItemType Directory -Force).FullName
$target = Join-Path $output 'WrathFall.exe'
$options = @(
    '/nologo', '/target:winexe', '/platform:x64', '/langversion:5', '/optimize+', '/codepage:65001'
    "/out:$target"
    "/win32manifest:$PSScriptRoot\launcher.manifest"
    "/win32icon:$PSScriptRoot\Launcher.ico"
    "/resource:$PSScriptRoot\LauncherWindow.xaml,LauncherWindow.xaml"
)
$hero = Join-Path $PSScriptRoot 'Hero.jpg'
if (Test-Path -LiteralPath $hero -PathType Leaf) { $options += "/resource:$hero,Hero.jpg" }
foreach ($assembly in @(
    'WPF\PresentationFramework.dll', 'WPF\PresentationCore.dll', 'WPF\WindowsBase.dll', 'System.Xaml.dll'
    'System.Web.Extensions.dll', 'System.Windows.Forms.dll', 'System.Drawing.dll'
    'System.IO.Compression.dll', 'System.IO.Compression.FileSystem.dll'
)) {
    $options += '/reference:' + (Join-Path $framework $assembly)
}
$options += @('Program.cs', 'Project.cs', 'BackgroundJob.cs', 'GameLaunch.cs', 'GameLaunchInputs.cs', 'GameImport.cs') |
    ForEach-Object { Join-Path $PSScriptRoot $_ }
& $compiler @options
if ($LASTEXITCODE -ne 0) { throw "Launcher compilation failed ($LASTEXITCODE)." }
Get-Item -LiteralPath $target

