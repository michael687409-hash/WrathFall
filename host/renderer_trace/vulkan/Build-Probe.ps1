param(
    [Parameter(Mandatory)][string] $VulkanInclude,
    [Parameter(Mandatory)][string] $OutputDirectory,
    [string] $ProjectRoot = (Join-Path $PSScriptRoot '../../..'),
    [string] $Dxc,
    [string] $GlslangValidator
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path -LiteralPath $ProjectRoot).Path
$VulkanInclude = (Resolve-Path -LiteralPath $VulkanInclude).Path
$renderer = Join-Path $ProjectRoot 'host/renderer_trace'
$vendor = Join-Path $ProjectRoot '.tools/renderer-deps'
$fsr = Join-Path $vendor 'fsr-2.4.0/FidelityFX-SDK-2.4.0/Kits/FidelityFX'
$streamline = Join-Path $vendor 'streamline-2.14.16'
foreach ($header in @("$VulkanInclude/vulkan/vulkan.h", "$fsr/api/include/vk/ffx_api_vk.h",
                       "$streamline/include/sl_helpers_vk.h", "$renderer/temporal_d3d12.h")) {
    if (!(Test-Path -LiteralPath $header -PathType Leaf)) {
        throw "Missing pinned header $header"
    }
}
$compiler = (Get-Command cl.exe -ErrorAction Stop).Source
if ($Dxc) { $Dxc = (Get-Command $Dxc -ErrorAction Stop).Source }
if ($GlslangValidator) { $GlslangValidator = (Get-Command $GlslangValidator -ErrorAction Stop).Source }
$null = New-Item -ItemType Directory -Path $OutputDirectory -Force
$output = (Resolve-Path -LiteralPath $OutputDirectory).Path
Push-Location $output
try {
    $compileArgs = @(
        '/nologo', '/std:c++20', '/EHsc', '/W4', '/WX', '/O2', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN'
        '/I', $VulkanInclude, '/I', "$fsr/api/include", '/I', "$fsr/upscalers/include"
        '/external:I', "$streamline/include", '/external:W0', '/I', $renderer
        "$PSScriptRoot/temporal_probe.cpp", "$PSScriptRoot/fsr_vulkan.cpp"
        "$PSScriptRoot/dlss_vulkan.cpp", '/Fe:temporal_probe.exe'
    )
    & $compiler @compileArgs
    if ($LASTEXITCODE -ne 0) { throw "Probe compilation failed ($LASTEXITCODE)" }
    if ($GlslangValidator) {
        & $GlslangValidator -V --target-env vulkan1.2 '-DMOTION=1' -o "$output/live-motion.spv" "$PSScriptRoot/live_temporal.comp"
        if ($LASTEXITCODE -ne 0) { throw "Vulkan motion SPIR-V compilation failed ($LASTEXITCODE)" }
    }
    if ($Dxc) {
        foreach ($name in @('depth_motion', 'fsr3_reactive')) {
            $shaderArgs = @(
                '-spirv', '-fspv-target-env=vulkan1.2', '-fvk-use-dx-layout'
                '-fvk-b-shift', 0, 0, '-fvk-t-shift', 1, 0, '-fvk-u-shift', 4, 0, '-fvk-s-shift', 6, 0
                '-T', 'cs_6_0', '-E', 'CSMain', '-Fo', "$output/$name.spv"
                '-Fc', "$output/$name.spvasm", "$renderer/$name.hlsl"
            )
            & $Dxc @shaderArgs
            if ($LASTEXITCODE -ne 0) { throw "SPIR-V compilation for $name failed ($LASTEXITCODE)" }
        }
    }
} finally {
    Pop-Location
}
