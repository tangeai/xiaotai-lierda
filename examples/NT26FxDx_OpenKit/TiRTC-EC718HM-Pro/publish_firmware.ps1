# Build and publish the normal HM Pro package. This script never flashes hardware.
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$buildRoot = Join-Path $repository 'gccout'
$releaseDirectory = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\firmware\hm-pro'))
$checksumPath = Join-Path $releaseDirectory 'SHA256SUMS.txt'
$versionMatches = [regex]::Matches(
    [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'boards\f9d_d\config.mk')),
    '(?m)^APP_VERSION[ \t]*:=[ \t]*(pro_f9d_d_\w+_sdk170)[ \t]*\r?$')
if ($versionMatches.Count -ne 1) { throw 'Missing or ambiguous HM Pro release APP_VERSION.' }
$version = $versionMatches[0].Groups[1].Value

$layout = @(
    @{ Address = 0x803000; Capacity = 0x20000; Source = 'ap_bootloader.bin'; Hash = '6743229009df330455c6bb9ecd3cf6324c386ee8b733d0d557cb08ab1e3385d1' },
    @{ Address = 0x8CD000; Capacity = 0x1DC000; Source = 'ap_lierda_app.bin'; Hash = '9292a1d1b7c33168dab10bc0bb66c27dd53a1204889a2feaf325cc31e20eecb1' },
    @{ Address = 0x82D000; Capacity = 0xA0000; Source = 'cp-demo-flash.bin'; Hash = '64e88a6b97954749403c4b1a6fed765552a67e033f1cc9e6ee883914aef7ba2b' },
    @{ Address = 0xAC1000; Capacity = 0x2B4000 }
)

$sha = [Security.Cryptography.SHA256]::Create()
function Get-BytesHash([byte[]]$Bytes, [int]$Offset = 0, [int]$Count = -1) {
    if ($Count -lt 0) { $Count = $Bytes.Length }
    [BitConverter]::ToString($sha.ComputeHash($Bytes, $Offset, $Count)).Replace('-', '').ToLowerInvariant()
}

function Assert-RegularPath([string]$Path) {
    $entry = Get-Item -LiteralPath $Path -Force -ErrorAction SilentlyContinue
    if ($entry -and ($entry.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        throw ('Refusing a linked build or release path: ' + $Path)
    }
}

# A fresh output directory keeps advanced make flags and object files from
# build_app.bat out of the release. Keep the path short for the vendor tools.
$buildDirectory = Join-Path $buildRoot ('hmrel-' + [Guid]::NewGuid().ToString('N').Substring(0, 12))
$buildCreated = $false
$stagedPackage = $null
$stagedChecksum = $null
$publishLock = $null
try {
    Assert-RegularPath $buildRoot
    if (Test-Path -LiteralPath $buildDirectory) { throw 'HM Pro temporary build directory already exists.' }
    [IO.Directory]::CreateDirectory($buildDirectory) | Out-Null
    $buildCreated = $true

    $inheritedFlags = @('MAKEFLAGS', 'GNUMAKEFLAGS', 'MAKEFILES', 'MFLAGS', 'MAKEOVERRIDES',
                        'CFLAGS', 'CFLAGS_CPU', 'CFLAGS_OPTION', 'CFLAGS_OPTIMIZE', 'CFLAGS_ANALYSIS',
                        'CFLAGS_WARNING', 'CFLAGS_INC', 'CFLAGS_DEFS', 'LDFLAGS')
    foreach ($name in $inheritedFlags) {
        [Environment]::SetEnvironmentVariable($name, $null, 'Process')
    }
    $makeArgs = @(('BUILDDIR=' + $buildDirectory.Replace('\', '/')),
                  'TIRTC_CALL_MEDIA_DIAGNOSTICS=n', 'TIRTC_VIDEO_PERF_DIAGNOSTICS=n',
                  'TIRTC_F9_CALL_UDP_RCVBUF_BYTES=0', 'TIRTC_AUDIO_AEC_EN=y',
                  'TIRTC_AUDIO_AEC_DELAY=10', 'TIRTC_AUDIO_FREE_GUARD_EN=y')
    & (Join-Path $PSScriptRoot 'build_app.bat') @makeArgs
    if ($LASTEXITCODE -ne 0) { throw 'HM Pro compilation failed.' }

    $binaryPath = Join-Path $buildDirectory 'TiRTC_pro_NT26F9D0.bin'
    $packagePath = Join-Path $buildDirectory ('TiRTC_pro_NT26F9D0_' + $version + '.binpkg')
    $binary = Get-Item -LiteralPath $binaryPath
    $binaryBytes = [IO.File]::ReadAllBytes($binaryPath)
    if ($binaryBytes.Length -lt 24 -or [BitConverter]::ToUInt32($binaryBytes, 0) -ne 0x30707061 -or
        [BitConverter]::ToUInt32($binaryBytes, 16) -ne 0x575000 -or
        [BitConverter]::ToUInt32($binaryBytes, 20) -ne 0x262000 -or
        -not [Text.Encoding]::ASCII.GetString($binaryBytes).Contains("V1.7.0_appV$version" + [char]0)) {
        throw 'Application is not the HM Pro 1.7.0 release build.'
    }

    $packageBytes = [IO.File]::ReadAllBytes($packagePath)
    $magic = '111' + ([char]0).ToString() * 5 + 'bpkg'
    if ($packageBytes.Length -lt 52 -or [Text.Encoding]::ASCII.GetString($packageBytes, 0, 12) -ne $magic -or
        [Text.Encoding]::ASCII.GetString($packageBytes, 20, 6) -ne 'SHA256' -or
        [BitConverter]::ToUInt32($packageBytes, 16) -ne 4) {
        throw 'Expected the normal four-image SHA256 F9D_D package.'
    }
    $offset = 52 + [int][BitConverter]::ToUInt16($packageBytes, 30)
    for ($i = 0; $i -lt $layout.Count; ++$i) {
        $image = $layout[$i]
        if ($offset + 364 -gt $packageBytes.Length) { throw 'Truncated F9D_D image header.' }
        $address = [BitConverter]::ToUInt32($packageBytes, $offset + 64)
        $capacity = [BitConverter]::ToUInt32($packageBytes, $offset + 68)
        $position = [BitConverter]::ToUInt32($packageBytes, $offset + 72)
        $length = [BitConverter]::ToUInt32($packageBytes, $offset + 76)
        if ($address -ne $image.Address -or $capacity -ne $image.Capacity -or
            $position -ne $offset -or $length -eq 0 -or $length -gt $capacity -or
            [long]$offset + 364 + $length -gt $packageBytes.Length) {
            throw 'Package does not match the F9D_D 1.7.0 flash layout.'
        }
        $expectedHash = [Text.Encoding]::ASCII.GetString($packageBytes, $offset + 80, 64).ToLowerInvariant()
        if ((Get-BytesHash $packageBytes ($offset + 364) ([int]$length)) -ne $expectedHash) {
            throw 'F9D_D image SHA256 mismatch.'
        }
        if ($i -eq 3) {
            if ($expectedHash -ne (Get-BytesHash $binaryBytes)) {
                throw 'Packaged application differs from the current release build.'
            }
        }
        else {
            $inputFile = Join-Path $repository ('components\basePkg\F9D_D\' + $image.Source)
            if ($expectedHash -ne $image.Hash -or (Get-BytesHash ([IO.File]::ReadAllBytes($inputFile))) -ne $image.Hash) {
                throw ('Package and SDK must use the pinned F9D_D 1.7.0 image: ' + $image.Source)
            }
        }
        $offset += 364 + [int]$length
    }
    if ($offset -ne $packageBytes.Length) { throw 'Unexpected trailing F9D_D package bytes.' }

    $timestamp = $binary.LastWriteTimeUtc.AddHours(8).ToString('yyyy_MM_dd_HH_mm_ss', [Globalization.CultureInfo]::InvariantCulture)
    $releaseName = $timestamp + '_hm_pro.binpkg'
    $releasePath = Join-Path $releaseDirectory $releaseName
    $packageHash = Get-BytesHash $packageBytes
    $publishLock = [IO.File]::Open((Join-Path $buildRoot '.hm-pro-publish.lock'),
                                  [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    Assert-RegularPath (Split-Path -Parent $releaseDirectory)
    Assert-RegularPath $releaseDirectory
    [IO.Directory]::CreateDirectory($releaseDirectory) | Out-Null
    Assert-RegularPath $checksumPath
    Assert-RegularPath $releasePath
    if ((Test-Path -LiteralPath $releasePath) -and
        (-not [IO.File]::Exists($releasePath) -or (Get-BytesHash ([IO.File]::ReadAllBytes($releasePath))) -ne $packageHash)) {
        throw 'The build timestamp already names a different HM Pro package.'
    }

    $manifest = ''
    $alreadyListed = $false
    if (Test-Path -LiteralPath $checksumPath) {
        $manifest = [IO.File]::ReadAllText($checksumPath, [Text.Encoding]::ASCII)
        if (-not $manifest) { throw 'Refusing to replace an empty SHA256SUMS.txt.' }
        foreach ($line in $manifest.TrimEnd([char]13, [char]10).Split([char]10)) {
            $entry = [regex]::Match($line.TrimEnd([char]13), '^([0-9a-f]{64})  (\d{4}(?:_\d{2}){5}_hm_pro\.binpkg)$')
            if (-not $entry.Success) { throw 'SHA256SUMS.txt contains an entry outside the HM Pro release format.' }
            if ($entry.Groups[2].Value -eq $releaseName) {
                if ($alreadyListed -or $entry.Groups[1].Value -ne $packageHash) {
                    throw 'SHA256SUMS.txt has a conflicting entry for this package.'
                }
                $alreadyListed = $true
            }
        }
    }

    $stageId = [Guid]::NewGuid().ToString('N')
    $publishedPackage = $false
    if (-not [IO.File]::Exists($releasePath)) {
        $stagedPackage = Join-Path $releaseDirectory ('.hm-pro-' + $stageId + '.binpkg.tmp')
        [IO.File]::WriteAllBytes($stagedPackage, $packageBytes)
        if ((Get-BytesHash ([IO.File]::ReadAllBytes($stagedPackage))) -ne $packageHash) {
            throw 'Staged HM Pro package verification failed.'
        }
        [IO.File]::Move($stagedPackage, $releasePath)
        $publishedPackage = $true
    }
    try {
        if (-not $alreadyListed) {
            $stagedChecksum = Join-Path $releaseDirectory ('.hm-pro-' + $stageId + '.sha.tmp')
            if ($manifest -and -not $manifest.EndsWith([char]10)) { $manifest += [char]10 }
            $manifest += "$packageHash  $releaseName" + [char]10
            [IO.File]::WriteAllText($stagedChecksum, $manifest, [Text.Encoding]::ASCII)
            if ([IO.File]::Exists($checksumPath)) { [IO.File]::Replace($stagedChecksum, $checksumPath, [NullString]::Value) }
            else { [IO.File]::Move($stagedChecksum, $checksumPath) }
        }
    }
    catch {
        if ($publishedPackage) { [IO.File]::Delete($releasePath) }
        throw
    }

    Write-Output ('Ready: ' + $releasePath)
    Write-Output ('SHA256: ' + $packageHash)
}
finally {
    foreach ($path in @($stagedPackage, $stagedChecksum)) {
        if ($path) { [IO.File]::Delete($path) }
    }
    if ($publishLock) { $publishLock.Dispose() }
    if ($buildCreated) {
        $resolvedBuild = (Resolve-Path -LiteralPath $buildDirectory).ProviderPath
        $resolvedRoot = (Resolve-Path -LiteralPath $buildRoot).ProviderPath
        if ((Split-Path -Parent $resolvedBuild) -ne $resolvedRoot -or
            (Split-Path -Leaf $resolvedBuild) -cnotmatch '^hmrel-[0-9a-f]{12}$') {
            throw ('Refusing to remove unexpected release build path: ' + $resolvedBuild)
        }
        Assert-RegularPath $resolvedBuild
        Remove-Item -LiteralPath $resolvedBuild -Recurse -Force
    }
    $sha.Dispose()
}
