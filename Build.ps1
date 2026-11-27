[CmdletBinding()]
param(
    [ValidateSet('Codegen', 'Build', 'Run', 'PrepareShaders', 'Achievements')]
    [string] $Stage = 'Build',

    [ValidateRange(1, 32)]
    [int] $Jobs = 8,

    [ValidateSet('d3d12', 'vulkan')]
    [string] $Renderer = 'd3d12',

    [ValidateRange(1, 3)]
    [int] $ResolutionScale = 1,

    [ValidatePattern('^[1-9][0-9]{0,4}x[1-9][0-9]{0,4}$')]
    [string] $OutputResolution,

    [ValidatePattern('^\\\\\.\\DISPLAY[1-9][0-9]*$')]
    [string] $DisplayMonitor,

    [ValidateSet('none', 'fxaa', 'fxaa_extreme')]
    [string] $AntiAliasing = 'none',

    [ValidateSet('Default', 'J', 'K', 'L', 'M')]
    [string] $DlssPreset,

    [ValidateSet('native', 'dlss', 'fsr3')]
    [string] $Upscaler = 'native',

    [ValidateRange(-1, 5)]
    [int] $TextureFiltering = 3,

    [ValidateSet(0, 30, 60, 120, 144)]
    [int] $FrameRate = 30,

    [ValidateRange(24, 240)]
    [int] $DisplayRefreshRate = 60,

    [ValidateSet('windowed', 'borderless', 'fullscreen')]
    [string] $DisplayMode = 'windowed',

    [switch] $Fullscreen,
    [switch] $DisableVSync,
    [switch] $SkipGameplayCollector
)

$ErrorActionPreference = 'Stop'
if ($DlssPreset -and !$PSBoundParameters.ContainsKey('Upscaler')) {
    $Upscaler = 'dlss'
}
$Upscaler = $Upscaler.ToLowerInvariant()
if ($OutputResolution) {
    $width, $height = $OutputResolution -split 'x'
    if ([int]$width -gt 16384 -or [int]$height -gt 16384) {
        throw 'Output resolution must be within 16384 × 16384.'
    }
    if ($Upscaler -ne 'native' -and $OutputResolution -ne '2560x1440') {
        throw 'The current temporal renderer requires 2560 × 1440 output.'
    }
}
if ($Fullscreen -and !$PSBoundParameters.ContainsKey('DisplayMode')) {
    $DisplayMode = 'borderless'
}
if ($Stage -eq 'Run' -and $ResolutionScale -eq 1 -and !$PSBoundParameters.ContainsKey('AntiAliasing')) {
    $AntiAliasing = 'fxaa_extreme'
}
$sdk = Join-Path $PSScriptRoot '.tools\rexglue-0.11.0\win-amd64'
$game = Join-Path $PSScriptRoot 'local\game'
$update = Join-Path $PSScriptRoot 'local\update'
$out = Join-Path $PSScriptRoot 'out'
$manifest = Join-Path $PSScriptRoot 'asuras_wrath_manifest.toml'
. (Join-Path $PSScriptRoot 'ProjectInputs.ps1')

function Invoke-Program([string] $Program, [string[]] $Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program exited with code $LASTEXITCODE." }
}

function Watch-GameplayCollector([Diagnostics.Process] $GameProcess, [string] $LogDirectory,
                                 [string] $GraphicsApi, [string] $Provider) {
    $collectorExe = Join-Path $out 'build\win-amd64\Release\asura_gameplay_collector.exe'
    if (!(Test-Path -LiteralPath $collectorExe -PathType Leaf)) {
        throw 'Use Build PC version to install the collector.'
    }

    do {
        $GameProcess.Refresh()
        if ($GameProcess.HasExited) { return }
        $window = $GameProcess.MainWindowHandle
    } while ($window -eq [IntPtr]::Zero -and !$GameProcess.WaitForExit(100))
    if ($window -eq [IntPtr]::Zero) { return }

    [void][IO.Directory]::CreateDirectory($LogDirectory)
    $stamp = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
    $log = Join-Path $LogDirectory "gameplay-collector-$stamp-$($GameProcess.Id).log"
    $errorLog = [IO.Path]::ChangeExtension($log, '.err.log')
    $arguments = @(('"{0}"' -f $PSScriptRoot), $GameProcess.Id, $window.ToInt64(), $GraphicsApi, $Provider)
    $collector = Start-Process -FilePath $collectorExe -ArgumentList $arguments -WindowStyle Hidden -PassThru `
        -RedirectStandardOutput $log -RedirectStandardError $errorLog
    Write-Output "Collector log: $log"

    try {
        $active = $false
        while (!$GameProcess.WaitForExit(250)) {
            if ($collector.HasExited) {
                Write-Output "Gameplay collector stopped. See $errorLog"
                return
            }
            if (!$active -and (Get-Item -LiteralPath $log).Length -gt 0) {
                $active = $true
                Write-Output 'Gameplay collector active. Press L in the game to mark an issue.'
            }
        }
        # Keep the redirected logs open while the collector saves pending marks.
        if ($collector.WaitForExit(10000)) { $collector.WaitForExit() }
    } finally {
        $collector.Dispose()
    }
}

$lock = Open-ProjectLock $out
Push-Location -LiteralPath $PSScriptRoot
try {
    Install-Patch $game $update
    if (!(Confirm-File (Join-Path $game 'default.xex') $discHash) -or
        !(Confirm-File (Join-Path $update 'default.xexp') $patchHash) -or
        !(Confirm-File (Join-Path $game 'default.xexp') $patchHash)) {
        throw 'Expected the verified base disc and TU1 pair for media ID 11DFADD6. Import or verify them in the launcher.'
    }
    if ($Stage -eq 'Codegen') {
        $rexglue = Join-Path $sdk 'bin\rexglue.exe'
        Invoke-Program $rexglue @('codegen', $manifest, '--ignore-stamp')
        return
    }
    if ($Stage -eq 'Achievements') {
        $rexglue = Join-Path $sdk 'bin\rexglue.exe'
        Invoke-Program $rexglue @('init', 'achievements', (Join-Path $game 'default.xex'),
            (Join-Path $out 'achievements'))
        return
    }

    if ($Stage -in 'Run', 'PrepareShaders') {
        $prepare = $Stage -eq 'PrepareShaders'
        $vulkanTemporal = !$prepare -and $Renderer -eq 'vulkan' -and $Upscaler -ne 'native'
        # The Vulkan and D3D12 shader archives use the same filename with incompatible versions.
        $cacheRoot = Join-Path $out 'cache'
        if ($Renderer -eq 'vulkan') { $cacheRoot = Join-Path $cacheRoot 'vulkan' }
        $exe = Join-Path $out 'build\win-amd64\Release\asuras_wrath.exe'
        if (!(Test-Path -LiteralPath $exe -PathType Leaf)) { throw "Build the executable first. Missing $exe." }
        if ($Upscaler -ne 'native' -and (!$prepare -or $Renderer -eq 'd3d12')) {
            $upscalerName = if ($Upscaler -eq 'fsr3') { 'FSR 3' } else { 'DLSS' }
            if ($ResolutionScale -ne 1) { throw "$upscalerName requires 720p internal resolution." }
            if ($vulkanTemporal -and $OutputResolution -ne '2560x1440') {
                throw 'Experimental Vulkan upscaling requires 2560 × 1440 output.'
            }
            if ($vulkanTemporal) {
                $temporalDll = Join-Path $out 'vulkan-upscalers\rexgpu-xenos-vulkan.dll'
                $temporalGame = Join-Path $out 'vulkan-upscalers\game'
            } else {
                $temporalDll = Join-Path $PSScriptRoot 'runtime\upscalers\rexgpu-xenos-d3d12.dll'
                if (!(Test-Path -LiteralPath $temporalDll -PathType Leaf)) {
                    $temporalDll = Join-Path $out 'dlss-runtime\rexgpu-xenos-d3d12.dll'
                }
                $temporalGame = Join-Path $out 'dlss-runtime\game'
            }
            if ($Upscaler -eq 'fsr3') {
                $vendorRoot = Join-Path $PSScriptRoot '.tools\renderer-deps\fsr-2.4.0\FidelityFX-SDK-2.4.0\Kits\FidelityFX\signedbin'
                $backend = if ($vulkanTemporal) { 'vk' } else { 'dx12' }
                $vendorDlls = "amd_fidelityfx_loader_$backend.dll", "amd_fidelityfx_upscaler_$backend.dll"
            } else {
                $vendorRoot = Join-Path $PSScriptRoot '.tools\renderer-deps\streamline-2.14.16\bin\x64'
                $vendorDlls = 'sl.interposer.dll', 'sl.common.dll', 'sl.pcl.dll', 'sl.dlss.dll', 'nvngx_dlss.dll'
            }
            $missingVendor = $vendorDlls | Where-Object {
                !(Test-Path -LiteralPath (Join-Path $vendorRoot $_) -PathType Leaf)
            }
            if (!(Test-Path -LiteralPath $temporalDll -PathType Leaf) -or
                $missingVendor) {
                throw "The $upscalerName renderer or vendor runtime is missing."
            }
            $null = New-Item -ItemType Directory -Path $temporalGame -Force
            $moduleName = Split-Path -Leaf $temporalDll
            Get-ChildItem -LiteralPath (Split-Path -Parent $exe) -Filter '*.dll' -File |
                Where-Object Name -ne $moduleName |
                Copy-Item -Destination $temporalGame -Force
            Copy-Item -LiteralPath $exe, $temporalDll -Destination $temporalGame -Force
            $exe = Join-Path $temporalGame 'asuras_wrath.exe'
        }
        $logRoot = Join-Path $out 'logs'
        if ($prepare) {
            $cachePattern = if ($Renderer -eq 'vulkan') { '43430817.*.vk.xpso' }
                elseif ($Upscaler -ne 'native') { '43430817.rtv.d3d12.xpso' } else { '43430817.*.d3d12.xpso' }
            $recorded = Get-ChildItem -LiteralPath (Join-Path $cacheRoot 'shaders\shareable') -File `
                -Filter $cachePattern `
                -ErrorAction SilentlyContinue | Where-Object Length -GT 32
            if (!$recorded) { throw 'No recorded shaders for this renderer are available to precompile.' }
            $userRoot = Join-Path $out 'shader-preparation\user'
            $logRoot = Join-Path $out 'logs\shader-preparation'
            Write-Output 'Precompiling recorded shaders...'
        } else {
            $userRoot = Initialize-PlayerData $PSScriptRoot
            Write-Output "Using the save folder at $userRoot"
        }
        $arguments = @(
            ('"--title.game_root={0}"' -f $game),
            ('"--title.update_root={0}"' -f $update),
            ('"--app.user_root={0}"' -f $userRoot),
            ('"--app.log_root={0}"' -f $logRoot),
            ('"--app.cache_root={0}"' -f $cacheRoot),
            "--gpu.driver=$Renderer",
            "--gpu.resolution_scale_x=$ResolutionScale",
            "--gpu.resolution_scale_y=$ResolutionScale",
            "--ui.fullscreen=$(($DisplayMode -ne 'windowed').ToString().ToLowerInvariant())",
            "--ui.fullscreen_exclusive=$(($DisplayMode -eq 'fullscreen').ToString().ToLowerInvariant())",
            # This single-player title has one local profile, in guest slot 0.
            '--hid.shared_pads=true',
            "--gpu.vsync=$((!$DisableVSync.IsPresent).ToString().ToLowerInvariant())",
            "--gpu.anisotropic_override=$TextureFiltering",
            "--gpu.xenos.swap_post_effect=$AntiAliasing",
            '--gpu.xenos.present_letterbox=true',
            '--gpu.xenos.store_shaders=true',
            '--gpu.xenos.async_shader_compilation=false'
        )
        if ($Renderer -eq 'd3d12' -and $Upscaler -ne 'native') { $arguments += '--gpu.d3d12.render_target_path=rtv' }
        if ($Renderer -eq 'vulkan') { $arguments += '--gpu.vulkan.render_target_path=fsi' }
        if (!$prepare -and $OutputResolution) {
            $arguments += "--ui.resolution=$OutputResolution",
                '--ui.video_mode_width=1280', '--ui.video_mode_height=720'
            if (!$DisplayMonitor) { $arguments += '--ui.monitor=1' }
        }
        if ($prepare) {
            $arguments += @('--ui.driver=none', '--log.level=info')
            if ($Renderer -eq 'd3d12') {
                # Wait for the preload workers before reporting completion.
                $arguments += '--gpu.d3d12.pipeline_creation_threads=0'
            } elseif ($Renderer -eq 'vulkan') {
                $arguments += '--gpu.vulkan.pipeline_creation_threads=0'
            }
        }
        $previousRate = $env:ASURAS_WRATH_FRAME_RATE
        $previousMonitor = $env:ASURAS_WRATH_DISPLAY_MONITOR
        $previousPreparation = $env:ASURAS_WRATH_PREPARE_SHADERS
        $previousGraphicsOverride = $env:ASURAS_WRATH_GRAPHICS_OVERRIDE
        $upscalerEnvironment = @{}
        $upscalerLog = $null
        try {
            $env:ASURAS_WRATH_FRAME_RATE = if ($prepare) { '30' } else { [string] $FrameRate }
            $env:ASURAS_WRATH_DISPLAY_MONITOR = if (!$prepare -and $DisplayMonitor) { $DisplayMonitor } else { $null }
            $env:ASURAS_WRATH_PREPARE_SHADERS = if ($prepare) { '1' } else { $null }
            $env:ASURAS_WRATH_GRAPHICS_OVERRIDE = if (!$prepare) {
                . (Join-Path $PSScriptRoot 'MotionBlur.ps1')
                $temporal = $Upscaler -ne 'native'
                $override = if ($temporal) { 'blur-off-temporal' } else { 'motion-blur-off' }
                New-MotionBlurOverride $game (Join-Path $out "cache\$override") -KeepVelocityPass:$temporal `
                    -DisableFrameRateSmoothing:($FrameRate -ne 30)
            } else { $null }
            if (!$prepare) {
                $arguments += "--gpu.frame_limit=$FrameRate"
            }
            if (!$prepare -and $FrameRate -eq 0) {
                $arguments += "--ui.video_mode_refresh_rate=$DisplayRefreshRate"
            } elseif (!$prepare -and $FrameRate -gt 60) {
                $arguments += "--ui.video_mode_refresh_rate=$FrameRate"
            }
            if (!$prepare -and $Upscaler -ne 'native') {
                $upscalerLog = Join-Path $logRoot ($Renderer + '-' + $Upscaler + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
                $null = New-Item -ItemType Directory -Path $upscalerLog
                $model = if ($Upscaler -eq 'dlss') {
                    if (!$DlssPreset) { 'K' }
                    elseif ($DlssPreset -eq 'Default') { 'Default' }
                    else { $DlssPreset.ToUpperInvariant() }
                }
                $upscalerOptions = @{
                    ASURA_LIVE_UPSCALER = $Upscaler
                    ASURA_UPSCALER_REPORT_DIR = $logRoot
                    ASURA_LIVE_DLSS_QA = $upscalerLog
                    ASURA_LIVE_DLSS_PRESENT = '1'
                    ASURA_LIVE_DLSS_RUNTIME = $vendorRoot
                    ASURA_DLSS_PRESET = $model
                    ASURA_LIVE_DLSS_DIAGNOSTICS = '0'
                    ASURA_REMOVE_MOTION_BLUR = '1'
                }
                if ($vulkanTemporal) {
                    $upscalerOptions.WRATHFALL_VK_TEMPORAL = $Upscaler
                    $upscalerOptions.WRATHFALL_VK_SCENE_TRACE = Join-Path $upscalerLog 'vulkan-scene.tsv'
                    $upscalerOptions.WRATHFALL_VK_DLSS_PROBE_RUNTIME = if ($Upscaler -eq 'dlss') { $vendorRoot } else { $null }
                    $upscalerOptions.WRATHFALL_VK_FSR_PROBE_RUNTIME = if ($Upscaler -eq 'fsr3') { $vendorRoot } else { $null }
                }
                foreach ($name in $upscalerOptions.Keys) {
                    $upscalerEnvironment[$name] = [Environment]::GetEnvironmentVariable($name)
                    [Environment]::SetEnvironmentVariable($name, $upscalerOptions[$name])
                }
                $modelText = if ($model) { ", preset $model" } else { '' }
                Write-Output "$upscalerName on $Renderer, 1280x720 to 2560x1440$modelText. Log folder $upscalerLog"
            }
            $start = @{ FilePath=$exe; ArgumentList=$arguments; PassThru=$true }
            if ($prepare) { $start.WindowStyle = 'Hidden' }
            $process = Start-Process @start
        } finally {
            $env:ASURAS_WRATH_FRAME_RATE = $previousRate
            $env:ASURAS_WRATH_DISPLAY_MONITOR = $previousMonitor
            $env:ASURAS_WRATH_PREPARE_SHADERS = $previousPreparation
            $env:ASURAS_WRATH_GRAPHICS_OVERRIDE = $previousGraphicsOverride
            foreach ($name in $upscalerEnvironment.Keys) {
                [Environment]::SetEnvironmentVariable($name, $upscalerEnvironment[$name])
            }
        }
        if (!$prepare -and $SkipGameplayCollector) {
            Write-Output 'Gameplay collector disabled.'
        } elseif (!$prepare) {
            try { Watch-GameplayCollector $process $logRoot $Renderer $Upscaler }
            catch { Write-Output "Gameplay collector unavailable. $($_.Exception.Message)" }
        }
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "$Stage exited with code $($process.ExitCode). See $logRoot." }
        if ($upscalerLog -and $vulkanTemporal) {
            $trace = Join-Path $upscalerLog 'vulkan-scene.tsv'
            if (!(Test-Path -LiteralPath $trace)) { throw "Experimental $upscalerName did not start. See $upscalerLog." }
            $failure = Select-String -LiteralPath $trace -Pattern '^(dlss-(instance|context)-error|fsr-context-error|temporal-init-error|upscale-error)\t' | Select-Object -Last 1
            if ($failure) { throw "Experimental $upscalerName reported an error. See $upscalerLog." }
        } elseif ($upscalerLog) {
            $upscalerError = Join-Path $upscalerLog 'live-temporal-error.txt'
            $upscalerTrace = Join-Path $upscalerLog 'live-temporal.tsv'
            if (Test-Path -LiteralPath $upscalerError) {
                throw "$upscalerName could not start. $(Get-Content -LiteralPath $upscalerError -Raw)"
            }
            if (!(Test-Path -LiteralPath $upscalerTrace)) { throw "The $upscalerName renderer did not start. See $logRoot." }
            $failure = Select-String -LiteralPath $upscalerTrace -Pattern '^disabled\t' | Select-Object -Last 1
            if ($failure) { throw "$upscalerName stopped during play. $($failure.Line.Split("`t", 2)[1])" }
        }
        if ($prepare) {
            $variant = if ($Renderer -eq 'vulkan') { 'vulkan-fsi' }
                elseif ($Upscaler -eq 'native') { $Renderer } else { 'd3d12-rtv' }
            $prepared = Join-Path $out "shader-preparation\$variant.prepared"
            [void][IO.Directory]::CreateDirectory((Split-Path -Parent $prepared))
            [IO.File]::WriteAllText($prepared, [DateTime]::UtcNow.ToString('o'))
            Write-Output 'Recorded shader precompilation finished.'
        }
        return
    }

    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (!(Test-Path -LiteralPath $vswhere -PathType Leaf)) { throw 'Install Visual Studio Build Tools with the Desktop development with C++ workload.' }
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (!$vs) { throw 'No Visual Studio installation with x64 C++ Build Tools was found.' }

    $cmakeTools = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
    $cmake = Join-Path $cmakeTools 'CMake\bin\cmake.exe'
    $ninja = Join-Path $cmakeTools 'Ninja\ninja.exe'
    if (!(Test-Path -LiteralPath $cmake -PathType Leaf) -or !(Test-Path -LiteralPath $ninja -PathType Leaf)) {
        throw 'Add C++ CMake tools for Windows to your Visual Studio 2022 installation.'
    }
    if (!(Test-Path -LiteralPath (Join-Path $PSScriptRoot 'generated\default\sources.cmake') -PathType Leaf)) {
        Write-Output 'Generating the PC source from your verified Xbox 360 game and update.'
        Invoke-Program (Join-Path $sdk 'bin\rexglue.exe') @('codegen', $manifest)
    }
    $clang = Join-Path $PSScriptRoot '.tools\llvm-20.1.8\bin'
    $savedPath = $env:PATH
    try {
        & (Join-Path $vs 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation
        $env:PATH = "$clang;$env:PATH"
        Invoke-Program $cmake @('--preset', 'win-amd64', "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_PREFIX_PATH=$sdk")
        $targets = @('asuras_runtime')
        $checks = Test-Path -LiteralPath (Join-Path $PSScriptRoot 'accuracy\guest_layout.cpp') -PathType Leaf
        if ($checks) { $targets += 'guest_layout_check', 'prompt_controller_check' }
        Invoke-Program $cmake (@('--build', '--preset', 'win-amd64-release', '--parallel', "$Jobs", '--target') + $targets)
        if ($checks) {
            Invoke-Program (Join-Path $out 'build\win-amd64\Release\guest_layout_check.exe') @()
            Invoke-Program (Join-Path $out 'build\win-amd64\Release\prompt_controller_check.exe') @()
        }
    } finally {
        $env:PATH = $savedPath
    }
} finally {
    Pop-Location
    $lock.Dispose()
}


