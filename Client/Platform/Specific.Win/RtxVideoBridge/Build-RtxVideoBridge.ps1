param(
    [Parameter(Mandatory = $false)]
    [string]$SdkDirectory = $env:RTX_VIDEO_SDK_DIR,

    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'

if ([string]::IsNullOrWhiteSpace($SdkDirectory)) {
    throw 'Pass -SdkDirectory or set RTX_VIDEO_SDK_DIR to the official NVIDIA RTX Video SDK directory.'
}

$bridgeDirectory = $PSScriptRoot
$repositoryRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $bridgeDirectory '..\..\..\..'))
$nativeOutputDirectory = Join-Path $repositoryRoot 'Client\Platform\Resources\win-x64\native'
$buildDirectory = Join-Path $bridgeDirectory 'build'

$cmakeCommand = Get-Command cmake -ErrorAction SilentlyContinue
if ($null -ne $cmakeCommand) {
    $cmakePath = $cmakeCommand.Source
} else {
    $cmakePath = Join-Path ${env:ProgramFiles(x86)} `
        'Microsoft Visual Studio\2022\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
}
if (-not (Test-Path -LiteralPath $cmakePath)) {
    throw 'CMake was not found in PATH or the Visual Studio 2022 Build Tools installation.'
}

& $cmakePath -S $bridgeDirectory -B $buildDirectory -G 'Visual Studio 17 2022' -A x64 `
    "-DRTX_VIDEO_SDK_DIR=$SdkDirectory" `
    "-DSYSDVR_NATIVE_OUTPUT_DIR=$nativeOutputDirectory"
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed with exit code $LASTEXITCODE."
}

& $cmakePath --build $buildDirectory --config $Configuration
if ($LASTEXITCODE -ne 0) {
    throw "Native bridge build failed with exit code $LASTEXITCODE."
}

$bridgeDll = Join-Path $buildDirectory "$Configuration\RtxVideoBridge.dll"
if (-not (Test-Path -LiteralPath $bridgeDll)) {
    throw "Native bridge output was not found: $bridgeDll"
}

Write-Host "Built: $bridgeDll"
Write-Host "Copied to: $nativeOutputDirectory\RtxVideoBridge.dll"
Write-Host 'The NVIDIA SDK and nvngx_vsr.dll were not copied into the repository.'
