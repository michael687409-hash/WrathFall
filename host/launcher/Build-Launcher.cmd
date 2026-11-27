@echo off
setlocal
set "framework=%SystemRoot%\Microsoft.NET\Framework64\v4.0.30319"
if not exist "%framework%\csc.exe" (
    echo The Windows .NET Framework 4.x compiler is required to build the launcher.
    exit /b 1
)
set "output=%~1"
if not defined output set "output=%~dp0..\.."
if not exist "%output%" mkdir "%output%"
if not exist "%output%" exit /b 1
set "hero="
if exist "%~dp0Hero.jpg" set hero="/resource:%~dp0Hero.jpg,Hero.jpg"
"%framework%\csc.exe" /nologo /target:winexe /platform:x64 /langversion:5 /optimize+ /codepage:65001 ^
    "/out:%output%\WrathFall.exe" "/win32manifest:%~dp0launcher.manifest" "/win32icon:%~dp0Launcher.ico" ^
    "/resource:%~dp0LauncherWindow.xaml,LauncherWindow.xaml" %hero% ^
    "/reference:%framework%\WPF\PresentationFramework.dll" "/reference:%framework%\WPF\PresentationCore.dll" ^
    "/reference:%framework%\WPF\WindowsBase.dll" "/reference:%framework%\System.Xaml.dll" ^
    "/reference:%framework%\System.Web.Extensions.dll" "/reference:%framework%\System.Windows.Forms.dll" ^
    "/reference:%framework%\System.Drawing.dll" "/reference:%framework%\System.IO.Compression.dll" ^
    "/reference:%framework%\System.IO.Compression.FileSystem.dll" ^
    "%~dp0Program.cs" "%~dp0Project.cs" "%~dp0BackgroundJob.cs" "%~dp0GameLaunch.cs" ^
    "%~dp0GameLaunchInputs.cs" "%~dp0GameImport.cs" "%~dp0LaunchCache.cs" "%~dp0ProjectTools.cs"
exit /b %errorlevel%
