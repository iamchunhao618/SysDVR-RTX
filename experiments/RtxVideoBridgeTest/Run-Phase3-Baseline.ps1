param(
    [string]$PublishDirectory =
        (Join-Path $PSScriptRoot '..\..\Client\bin\Release\net9.0\win-x64\publish'),

    [string]$OutputDirectory =
        (Join-Path $PSScriptRoot 'results\phase3_baseline'),

    [int]$CombinationTimeoutSeconds = 50
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
    [pscustomobject]@{ Label = '1440p_medium'; Quality = 2 },
    [pscustomobject]@{ Label = '1440p_high'; Quality = 3 }
)

$originalOptions = [System.IO.File]::ReadAllText($optionsPath)
$previousCaptureDirectory = $env:SYSDVR_RTX_CAPTURE_DIR
$env:SYSDVR_RTX_CAPTURE_DIR = $null

try {
    foreach ($combination in $combinations) {
        $settings = $originalOptions | ConvertFrom-Json
        $settings.windows_RtxVideo.enabled = $true
        $settings.windows_RtxVideo.outputResolution = 1440
        $settings.windows_RtxVideo.quality = $combination.Quality
        [System.IO.File]::WriteAllText(
            $optionsPath,
            ($settings | ConvertTo-Json -Depth 10) + [Environment]::NewLine,
            [System.Text.UTF8Encoding]::new($false))

        $stdout = Join-Path $output ("baseline_{0}.log" -f $combination.Label)
        $stderr = Join-Path $output ("baseline_{0}.error.log" -f $combination.Label)
        Remove-Item -LiteralPath $stdout, $stderr -Force -ErrorAction SilentlyContinue

        Write-Host "Running Phase 3 baseline $($combination.Label)..."
        $process = Start-Process `
            -FilePath $client `
            -WorkingDirectory $publish `
            -ArgumentList @('usb', '--debug', 'log') `
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
            $errorText = if (Test-Path -LiteralPath $stderr) {
                Get-Content -Raw -LiteralPath $stderr
            } else {
                ''
            }
            throw "Baseline $($combination.Label) did not complete. $errorText"
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
    [System.IO.File]::WriteAllText(
        $optionsPath,
        ($restoredSettings | ConvertTo-Json -Depth 10) + [Environment]::NewLine,
        [System.Text.UTF8Encoding]::new($false))
    $env:SYSDVR_RTX_CAPTURE_DIR = $previousCaptureDirectory
}

Write-Host "Phase 3 CPU-readback baseline complete: $output"
