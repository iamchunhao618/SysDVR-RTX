param(
    [string]$PublishDirectory =
        (Join-Path $PSScriptRoot '..\..\Client\bin\Release\net9.0\win-x64\publish'),

    [string]$SdkDirectory =
        'C:\Users\18109\AppData\Local\Temp\SysDVR-RtxVideoResearch-20260816\sdk',

    [string]$OutputDirectory =
        (Join-Path $PSScriptRoot 'results\phase3_gpu_direct'),

    [int]$CombinationTimeoutSeconds = 50,

    [switch]$Include4KHigh,

    [switch]$Fullscreen,

    [switch]$CompatibilitySmoke
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

$combinations = @(
    [pscustomobject]@{ Label = '1440p_medium'; Resolution = 1440; Quality = 2 },
    [pscustomobject]@{ Label = '1440p_high'; Resolution = 1440; Quality = 3 }
)
if ($CompatibilitySmoke) {
    $combinations = @(
        [pscustomobject]@{ Label = '1080p_low'; Resolution = 1080; Quality = 1 },
        [pscustomobject]@{ Label = '2160p_ultra'; Resolution = 2160; Quality = 4 }
    )
}
if ($Include4KHigh) {
    $combinations += [pscustomobject]@{
        Label = '2160p_high'
        Resolution = 2160
        Quality = 3
    }
}

$originalOptions = [System.IO.File]::ReadAllText($optionsPath)
$previousFeatureDirectory = $env:SYSDVR_RTX_VIDEO_FEATURE_DIR
$previousCaptureDirectory = $env:SYSDVR_RTX_CAPTURE_DIR
$env:SYSDVR_RTX_VIDEO_FEATURE_DIR = $featureDirectory
$env:SYSDVR_RTX_CAPTURE_DIR = $null

try {
    foreach ($combination in $combinations) {
        $settings = $originalOptions | ConvertFrom-Json
        $settings.windows_RtxVideo.enabled = $true
        $settings.windows_RtxVideo.outputResolution = $combination.Resolution
        $settings.windows_RtxVideo.quality = $combination.Quality
        if ($null -eq $settings.windows_RtxVideo.presentationBackend) {
            $settings.windows_RtxVideo | Add-Member `
                -NotePropertyName presentationBackend -NotePropertyValue 1
        } else {
            $settings.windows_RtxVideo.presentationBackend = 1
        }
        [System.IO.File]::WriteAllText(
            $optionsPath,
            ($settings | ConvertTo-Json -Depth 10) + [Environment]::NewLine,
            [System.Text.UTF8Encoding]::new($false))

        $stdout = Join-Path $output ("gpu_direct_{0}.log" -f $combination.Label)
        $stderr = Join-Path $output ("gpu_direct_{0}.error.log" -f $combination.Label)
        Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue

        Write-Host "Running Phase 3 GPU direct $($combination.Label)..."
        $arguments = @('usb', '--debug', 'log')
        if ($Fullscreen) {
            $arguments += '--fullscreen'
        }
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
                (Select-String -SimpleMatch -Quiet `
                    -LiteralPath $stdout -Pattern '[RTX VSR timing/interval]')) {
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
            $logText = if (Test-Path -LiteralPath $stdout) {
                Get-Content -Raw -LiteralPath $stdout
            } else {
                ''
            }
            $errorText = if (Test-Path -LiteralPath $stderr) {
                Get-Content -Raw -LiteralPath $stderr
            } else {
                ''
            }
            throw "GPU-direct $($combination.Label) did not complete.`n$logText`n$errorText"
        }

        Select-String -SimpleMatch -LiteralPath $stdout `
            -Pattern '[RTX VSR timing/interval]' |
            Select-Object -Last 1 |
            ForEach-Object Line

        Start-Sleep -Milliseconds 750
    }
}
finally {
    $restoredSettings = $originalOptions | ConvertFrom-Json
    $restoredSettings.windows_RtxVideo.enabled = $false
    $restoredSettings.windows_RtxVideo.outputResolution = 1440
    $restoredSettings.windows_RtxVideo.quality = 2
    if ($null -eq $restoredSettings.windows_RtxVideo.presentationBackend) {
        $restoredSettings.windows_RtxVideo | Add-Member `
            -NotePropertyName presentationBackend -NotePropertyValue 0
    } else {
        $restoredSettings.windows_RtxVideo.presentationBackend = 0
    }
    [System.IO.File]::WriteAllText(
        $optionsPath,
        ($restoredSettings | ConvertTo-Json -Depth 10) + [Environment]::NewLine,
        [System.Text.UTF8Encoding]::new($false))
    $env:SYSDVR_RTX_VIDEO_FEATURE_DIR = $previousFeatureDirectory
    $env:SYSDVR_RTX_CAPTURE_DIR = $previousCaptureDirectory
}

Write-Host "Phase 3 GPU-direct live benchmark complete: $output"
