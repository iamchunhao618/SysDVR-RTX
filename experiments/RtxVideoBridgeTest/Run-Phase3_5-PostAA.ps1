param(
    [string]$PublishDirectory =
        (Join-Path $PSScriptRoot '..\..\Client\bin\Release\net9.0\win-x64\publish'),
    [string]$SdkDirectory =
        'C:\Users\18109\AppData\Local\Temp\SysDVR-RtxVideoResearch-20260816\sdk',
    [string]$OutputDirectory =
        (Join-Path $PSScriptRoot 'results\phase3_5_post_aa'),
    [int]$CombinationTimeoutSeconds = 50,
    [string[]]$OnlyLabel = @(),
    [switch]$Include4KHigh,
    [switch]$Fullscreen
)

$ErrorActionPreference = 'Stop'

$publish = [System.IO.Path]::GetFullPath($PublishDirectory)
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
$client = Join-Path $publish 'SysDVR-Client.exe'
$optionsPath = Join-Path $publish 'options.json'
$featureDirectory = Join-Path $SdkDirectory 'bin\Windows\x64\dev'

if (-not (Test-Path -LiteralPath $client)) {
    throw "Published SysDVR client was not found: $client"
}
if (-not (Test-Path -LiteralPath $optionsPath)) {
    throw "Published SysDVR options were not found: $optionsPath"
}
if (-not (Test-Path -LiteralPath (Join-Path $featureDirectory 'nvngx_vsr.dll'))) {
    throw "Official NVIDIA RTX Video SDK feature DLL was not found: $featureDirectory"
}

New-Item -ItemType Directory -Force -Path $output | Out-Null

$combinations = foreach ($quality in @(2, 3)) {
    foreach ($aa in @(0, 1, 2)) {
        $qualityName = if ($quality -eq 2) { 'medium' } else { 'high' }
        $aaName = @('off', 'fxaa', 'smaa1x')[$aa]
        [pscustomobject]@{
            Label = "1440p_${qualityName}_${aaName}"
            Resolution = 1440
            Quality = $quality
            Aa = $aa
        }
    }
}
if ($Include4KHigh) {
    foreach ($aa in @(0, 1, 2)) {
        $combinations += [pscustomobject]@{
            Label = "2160p_high_$(@('off', 'fxaa', 'smaa1x')[$aa])"
            Resolution = 2160
            Quality = 3
            Aa = $aa
        }
    }
}
if ($OnlyLabel.Count -ne 0) {
    $combinations = @($combinations | Where-Object { $_.Label -in $OnlyLabel })
    if ($combinations.Count -eq 0) {
        throw 'No Phase 3.5 combination matched -OnlyLabel.'
    }
}

$originalOptions = [System.IO.File]::ReadAllText($optionsPath)
$previousFeatureDirectory = $env:SYSDVR_RTX_VIDEO_FEATURE_DIR
$previousCaptureDirectory = $env:SYSDVR_RTX_CAPTURE_DIR
$env:SYSDVR_RTX_VIDEO_FEATURE_DIR = $featureDirectory

try {
    foreach ($combination in $combinations) {
        $settings = $originalOptions | ConvertFrom-Json
        $settings.windows_RtxVideo.enabled = $true
        $settings.windows_RtxVideo.outputResolution = $combination.Resolution
        $settings.windows_RtxVideo.quality = $combination.Quality
        foreach ($entry in @(
            @{ Name = 'presentationBackend'; Value = 1 },
            @{ Name = 'postProcessAa'; Value = $combination.Aa }
        )) {
            if ($null -eq $settings.windows_RtxVideo.($entry.Name)) {
                $settings.windows_RtxVideo | Add-Member `
                    -NotePropertyName $entry.Name -NotePropertyValue $entry.Value
            } else {
                $settings.windows_RtxVideo.($entry.Name) = $entry.Value
            }
        }
        [System.IO.File]::WriteAllText(
            $optionsPath,
            ($settings | ConvertTo-Json -Depth 10) + [Environment]::NewLine,
            [System.Text.UTF8Encoding]::new($false))

        $capture = Join-Path $output 'captures'
        $env:SYSDVR_RTX_CAPTURE_DIR = $capture
        $stdout = Join-Path $output ("{0}.log" -f $combination.Label)
        $stderr = Join-Path $output ("{0}.error.log" -f $combination.Label)
        Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue

        Write-Host "Running Phase 3.5 $($combination.Label)..."
        $arguments = @('usb', '--debug', 'log')
        if ($Fullscreen) { $arguments += '--fullscreen' }
        $process = Start-Process `
            -FilePath $client `
            -WorkingDirectory $publish `
            -ArgumentList $arguments `
            -RedirectStandardOutput $stdout `
            -RedirectStandardError $stderr `
            -PassThru

        $deadline = [DateTime]::UtcNow.AddSeconds($CombinationTimeoutSeconds)
        $completed = $false
        while ([DateTime]::UtcNow -lt $deadline -and -not $process.HasExited) {
            if ((Test-Path -LiteralPath $stdout) -and
                (Select-String -SimpleMatch -Quiet -LiteralPath $stdout `
                    -Pattern '[RTX VSR timing/interval]')) {
                $completed = $true
                break
            }
            Start-Sleep -Milliseconds 250
        }

        if (-not $process.HasExited) {
            $null = $process.CloseMainWindow()
            if (-not $process.WaitForExit(5000)) {
                Stop-Process -Id $process.Id
                $process.WaitForExit()
            }
        }
        if (-not $completed) {
            Write-Warning "No 120-frame interval completed for $($combination.Label)."
        }
    }
}
finally {
    [System.IO.File]::WriteAllText(
        $optionsPath, $originalOptions, [System.Text.UTF8Encoding]::new($false))
    $env:SYSDVR_RTX_VIDEO_FEATURE_DIR = $previousFeatureDirectory
    $env:SYSDVR_RTX_CAPTURE_DIR = $previousCaptureDirectory
}

Write-Host "Phase 3.5 logs and captures: $output"
