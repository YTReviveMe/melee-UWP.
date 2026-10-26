param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\..\build-xbox-vendor'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\..\build-xbox-artifacts\packages'),
    [switch]$Sign
)
$ErrorActionPreference = 'Stop'
$build = [IO.Path]::GetFullPath($BuildDirectory)
$output = [IO.Path]::GetFullPath($OutputDirectory)
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$sdk = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin\10.0.26100.0\x64'
$identity = ([xml](Get-Content (Join-Path $PSScriptRoot 'Package.appxmanifest') -Raw)).Package.Identity
$pcVersion = (Get-Content -LiteralPath (Join-Path $root 'VERSION') -TotalCount 1).Trim()
if ($pcVersion -notmatch '^v?(\d+\.\d+\.\d+)$') { throw "Invalid PC version: $pcVersion" }
if ($identity.Version -ne "$($Matches[1]).0") {
    throw "Package version $($identity.Version) does not match PC version $pcVersion"
}
$vclibs = Join-Path ${env:ProgramFiles(x86)} 'Microsoft SDKs\Windows Kits\10\ExtensionSDKs\Microsoft.VCLibs\14.0\Appx\Retail\x64\Microsoft.VCLibs.x64.14.00.appx'
if (-not (Test-Path -LiteralPath $vclibs -PathType Leaf)) { throw "Missing package dependency: $vclibs" }
$stage = Join-Path $build ('package-' + [Guid]::NewGuid().ToString('N'))
$assets = Join-Path $stage 'Assets'
New-Item -ItemType Directory -Path $assets -Force | Out-Null

foreach ($name in @('melee.exe', 'SDL3.dll', 'nod.dll', 'libpng16.dll', 'zlib1.dll')) {
    $source = Join-Path $build $name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "Missing build output: $source" }
    Copy-Item -LiteralPath $source -Destination $stage
}
Copy-Item -LiteralPath (Join-Path $root 'resources') -Destination $stage -Recurse
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Package.appxmanifest') -Destination (Join-Path $stage 'AppxManifest.xml')
Copy-Item -LiteralPath (Join-Path $root 'licenses') -Destination $stage -Recurse
Copy-Item -LiteralPath (Join-Path $root 'extern\aurora\LICENSE') -Destination (Join-Path $stage 'Aurora-LICENSE.txt')
$discImages = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Where-Object {
    $_.Extension -in @('.iso', '.gcm', '.ciso', '.rvz', '.gcz', '.wia')
})
if ($discImages.Count) { throw "Disc image found in package staging: $($discImages[0].FullName)" }

Add-Type -AssemblyName System.Drawing
$icon = [Drawing.Image]::FromFile((Join-Path $PSScriptRoot 'Assets\Icon.png'))
try {
    foreach ($spec in @(@('StoreLogo.png', 50, 50), @('Square44x44Logo.png', 44, 44),
                        @('Square150x150Logo.png', 150, 150), @('SplashScreen.png', 620, 300))) {
        $width = [int]$spec[1]
        $height = [int]$spec[2]
        $bitmap = [Drawing.Bitmap]::new($width, $height)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.Clear([Drawing.Color]::FromArgb(56, 43, 69))
            $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $size = [Math]::Min($width, $height)
            $graphics.DrawImage($icon, [Drawing.Rectangle]::new(
                [int](($width - $size) / 2), [int](($height - $size) / 2), $size, $size))
            $bitmap.Save((Join-Path $assets $spec[0]), [Drawing.Imaging.ImageFormat]::Png)
        } finally { $graphics.Dispose(); $bitmap.Dispose() }
    }
} finally { $icon.Dispose() }

New-Item -ItemType Directory -Path $output -Force | Out-Null
$package = Join-Path $output ("$($identity.Name)_$($identity.Version)-$($identity.ProcessorArchitecture).appx")
& (Join-Path $sdk 'makeappx.exe') pack /d $stage /p $package /o
if ($LASTEXITCODE -ne 0) { throw 'MakeAppx failed' }
Copy-Item -LiteralPath $vclibs -Destination $output -Force
if ($Sign) {
    $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object {
        $_.Subject -eq $identity.Publisher -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date).AddDays(30)
    } | Sort-Object NotAfter -Descending | Select-Object -First 1
    if (-not $cert) {
        $cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject $identity.Publisher `
            -FriendlyName 'Melee UWP Xbox Dev Mode' -CertStoreLocation Cert:\CurrentUser\My `
            -KeyExportPolicy NonExportable -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(2)
    }
    & (Join-Path $sdk 'signtool.exe') sign /fd SHA256 /sha1 $cert.Thumbprint /s My $package
    if ($LASTEXITCODE -ne 0) { throw 'SignTool failed' }
    & (Join-Path $sdk 'signtool.exe') verify /pa $package
    if ($LASTEXITCODE -ne 0) { throw 'Package signature verification failed' }
    Export-Certificate -Cert $cert -FilePath (Join-Path $output "$($identity.Name).cer") -Force | Out-Null
}
$stagePath = [IO.Path]::GetFullPath($stage)
$buildPrefix = $build.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if (-not $stagePath.StartsWith($buildPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Staging folder is outside the build directory: $stagePath"
}
Remove-Item -LiteralPath $stagePath -Recurse -Force
Write-Output "Xbox package: $package"
