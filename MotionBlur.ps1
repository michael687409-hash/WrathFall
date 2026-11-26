function Read-CoalescedInt([IO.BinaryReader] $Reader) {
    [Net.IPAddress]::NetworkToHostOrder($Reader.ReadInt32())
}

function Read-CoalescedString([IO.BinaryReader] $Reader) {
    $length = Read-CoalescedInt $Reader
    if ($length -eq 0) { return '' }
    $size = if ($length -lt 0) { -2L * $length } else { $length }
    if ($size -gt ($Reader.BaseStream.Length - $Reader.BaseStream.Position)) {
        throw 'Unexpected end of the Xbox 360 configuration.'
    }
    $encoding = if ($length -lt 0) { [Text.Encoding]::Unicode } else { [Text.Encoding]::ASCII }
    $encoding.GetString($Reader.ReadBytes([int] $size)).TrimEnd([char] 0)
}

function Write-MotionBlurConfig([string] $Source, [string] $Destination,
                                [switch] $KeepVelocityPass, [switch] $DisableFrameRateSmoothing) {
    $bytes = [IO.File]::ReadAllBytes($Source)
    $sourceStream = [IO.MemoryStream]::new($bytes, $false)
    $output = [IO.MemoryStream]::new()
    $reader = [IO.BinaryReader]::new($sourceStream)
    $writer = [IO.BinaryWriter]::new($output)
    try {
        $remaining = 0
        $fields = @{}
        $files = Read-CoalescedInt $reader
        for ($f = 0; $f -lt $files; ++$f) {
            $name = Read-CoalescedString $reader
            $sections = Read-CoalescedInt $reader
            for ($s = 0; $s -lt $sections; ++$s) {
                $section = Read-CoalescedString $reader
                $count = Read-CoalescedInt $reader
                for ($k = 0; $k -lt $count; ++$k) {
                    $key = Read-CoalescedString $reader
                    $start = [int] $sourceStream.Position
                    $value = Read-CoalescedString $reader
                    $blur = $section -eq 'SystemSettings' -and $key -in 'MotionBlur', 'AllowRadialBlur'
                    $smoothing = $DisableFrameRateSmoothing -and $section -eq 'Engine.GameEngine' -and
                        $key -eq 'bSmoothFrameRate'
                    if ($name -like '*\Xbox360-BCEngine.ini' -and ($blur -or $smoothing)) {
                        if ($value -notin 'True', 'False' -or $fields.ContainsKey($key)) {
                            throw "Unexpected $key setting in $Source"
                        }
                        $fields[$key] = $true
                        if ($KeepVelocityPass -and $key -eq 'MotionBlur') { continue }
                        $output.Write($bytes, $remaining, $start - $remaining)
                        # UE3 FString uses a big-endian negative character count for UTF-16LE.
                        $writer.Write([Net.IPAddress]::HostToNetworkOrder([int] -6))
                        $writer.Write([Text.Encoding]::Unicode.GetBytes('False' + [char] 0))
                        $remaining = [int] $sourceStream.Position
                    }
                }
            }
        }
        $expected = if ($DisableFrameRateSmoothing) { 3 } else { 2 }
        if ($sourceStream.Position -ne $bytes.Length -or $fields.Count -ne $expected) {
            throw "Could not identify the Xbox 360 graphics settings in $Source"
        }
        $output.Write($bytes, $remaining, $bytes.Length - $remaining)
        [IO.File]::WriteAllBytes($Destination, $output.ToArray())
    } finally {
        $reader.Dispose()
        $writer.Dispose()
    }
}

function New-MotionBlurOverride([string] $Game, [string] $Destination,
                                [switch] $KeepVelocityPass, [switch] $DisableFrameRateSmoothing) {
    $configs = @(Get-ChildItem -LiteralPath (Join-Path $Game 'BCGame\CookedXbox360') -File -Filter 'Coalesced_*.bin')
    if (!$configs) { throw 'The game is missing its compiled graphics configuration.' }
    [void][IO.Directory]::CreateDirectory($Destination)
    foreach ($config in $configs) {
        Write-MotionBlurConfig $config.FullName (Join-Path $Destination $config.Name) `
            -KeepVelocityPass:$KeepVelocityPass -DisableFrameRateSmoothing:$DisableFrameRateSmoothing
    }
    $Destination
}
