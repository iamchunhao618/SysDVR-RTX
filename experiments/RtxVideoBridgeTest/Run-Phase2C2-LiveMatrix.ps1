param(
    [string]$PublishDirectory =
        (Join-Path $PSScriptRoot '..\..\Client\bin\Release\net9.0\win-x64\publish'),

    [string]$OutputDirectory =
        (Join-Path $PSScriptRoot 'results\phase2c2_live'),

    [int]$CombinationTimeoutSeconds = 40,

    [string[]]$Labels = @(),

    [switch]$CaptureOnly
)

$ErrorActionPreference = 'Stop'

$publish = [System.IO.Path]::GetFullPath($PublishDirectory)
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
$client = Join-Path $publish 'SysDVR-Client.exe'
$optionsPath = Join-Path $publish 'options.json'

if (-not (Test-Path -LiteralPath $client)) {
    throw "Published SysDVR client was not found: $client"
}
if (-not (Test-Path -LiteralPath $optionsPath)) {
    throw "Published SysDVR options were not found: $optionsPath"
}

New-Item -ItemType Directory -Force -Path $output | Out-Null

$combinations = @(
    [pscustomobject]@{ Label = 'vanilla_720p'; Enabled = $false; Resolution = 1440; Quality = 2 },
    [pscustomobject]@{ Label = '1080p_medium'; Enabled = $true; Resolution = 1080; Quality = 2 },
    [pscustomobject]@{ Label = '1080p_high'; Enabled = $true; Resolution = 1080; Quality = 3 },
    [pscustomobject]@{ Label = '1440p_medium'; Enabled = $true; Resolution = 1440; Quality = 2 },
    [pscustomobject]@{ Label = '1440p_high'; Enabled = $true; Resolution = 1440; Quality = 3 },
    [pscustomobject]@{ Label = '1440p_ultra'; Enabled = $true; Resolution = 1440; Quality = 4 },
    [pscustomobject]@{ Label = '2160p_medium'; Enabled = $true; Resolution = 2160; Quality = 2 },
    [pscustomobject]@{ Label = '2160p_high'; Enabled = $true; Resolution = 2160; Quality = 3 },
    [pscustomobject]@{ Label = '2160p_ultra'; Enabled = $true; Resolution = 2160; Quality = 4 }
)
if ($Labels.Count -ne 0) {
    $combinations = @($combinations | Where-Object Label -in $Labels)
    if ($combinations.Count -ne $Labels.Count) {
        throw 'One or more requested live-matrix labels are unknown.'
    }
}

function Set-RtxOptions {
    param(
        [bool]$Enabled,
        [int]$Resolution,
        [int]$Quality
    )

    $settings = Get-Content -Raw -Encoding utf8 -LiteralPath $optionsPath |
        ConvertFrom-Json
    $settings.windows_RtxVideo.enabled = $Enabled
    if ($null -eq $settings.windows_RtxVideo.outputResolution) {
        $settings.windows_RtxVideo |
            Add-Member -NotePropertyName outputResolution -NotePropertyValue $Resolution
    } else {
        $settings.windows_RtxVideo.outputResolution = $Resolution
    }
    $settings.windows_RtxVideo.quality = $Quality
    $json = $settings | ConvertTo-Json -Depth 10
    [System.IO.File]::WriteAllText(
        $optionsPath,
        $json + [Environment]::NewLine,
        [System.Text.UTF8Encoding]::new($false))
}

$previousCaptureDirectory = $env:SYSDVR_RTX_CAPTURE_DIR
$env:SYSDVR_RTX_CAPTURE_DIR = $output

try {
    foreach ($combination in $combinations) {
        Set-RtxOptions `
            -Enabled $combination.Enabled `
            -Resolution $combination.Resolution `
            -Quality $combination.Quality

        $stdout = Join-Path $output ("live_{0}.log" -f $combination.Label)
        $stderr = Join-Path $output ("live_{0}.error.log" -f $combination.Label)
        Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue

        Write-Host "Running $($combination.Label)..."
        $process = Start-Process `
            -FilePath $client `
            -WorkingDirectory $publish `
            -ArgumentList @('usb', '--debug', 'log') `
            -RedirectStandardOutput $stdout `
            -RedirectStandardError $stderr `
            -PassThru

        $successPattern = if ($CaptureOnly -or -not $combination.Enabled) {
            '[RTX VSR comparison] Captured'
        } else {
            '[RTX VSR timing/interval]'
        }
        $deadline = [DateTime]::UtcNow.AddSeconds($CombinationTimeoutSeconds)
        $completed = $false
        while ([DateTime]::UtcNow -lt $deadline -and -not $process.HasExited) {
            if ((Test-Path -LiteralPath $stdout) -and
                (Select-String -SimpleMatch -Quiet `
                    -LiteralPath $stdout -Pattern $successPattern)) {
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
            $errorText = if (Test-Path -LiteralPath $stderr) {
                Get-Content -Raw -LiteralPath $stderr
            } else {
                ''
            }
            throw "Live combination $($combination.Label) did not complete. $errorText"
        }

        Start-Sleep -Milliseconds 750
    }
}
finally {
    Set-RtxOptions -Enabled $false -Resolution 1440 -Quality 2
    $env:SYSDVR_RTX_CAPTURE_DIR = $previousCaptureDirectory
}

Write-Host "Live Phase 2C-2 matrix complete: $output"
