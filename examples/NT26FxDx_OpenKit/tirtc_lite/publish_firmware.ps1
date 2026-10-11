# Publish the verified normal Lite package. This script never flashes hardware.
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..'))
$buildDirectory = Join-Path $repository 'gccout\tirtc_lite'
$releaseDirectory = Join-Path $repository 'firmware\lite'
$versionMatch = [regex]::Match(
    [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'config\tirtc_config.mk')),
    '(?m)^APP_VERSION\s*:?=\s*(\w+)\s*$')
if (-not $versionMatch.Success) { throw 'Missing Lite APP_VERSION.' }
$packagePath = Join-Path $buildDirectory ('TiRTC_lite_NT26F6D0_' + $versionMatch.Groups[1].Value + '.binpkg')
$binaryPath = Join-Path $buildDirectory 'TiRTC_lite_NT26F6D0.bin'
$binaryInfo = Get-Item -LiteralPath $binaryPath
$binaryWriteTime = $binaryInfo.LastWriteTimeUtc
$binaryLength = $binaryInfo.Length
$timestamp = $binaryWriteTime.AddHours(8).ToString('yyyy_MM_dd_HH_mm_ss', [Globalization.CultureInfo]::InvariantCulture)
$releaseName = $timestamp + '_lite.binpkg'
$releasePath = Join-Path $releaseDirectory $releaseName
$checksumPath = Join-Path $releaseDirectory 'SHA256SUMS.txt'
$sha = [Security.Cryptography.SHA256]::Create()

function Get-BytesHash([byte[]]$Bytes, [int]$Offset = 0, [int]$Count = -1) {
    if ($Count -lt 0) { $Count = $Bytes.Length }
    return [BitConverter]::ToString($sha.ComputeHash($Bytes, $Offset, $Count)).Replace('-', '').ToLowerInvariant()
}

function Assert-ChildPath([string]$Path, [string]$Directory) {
    $absolute = [IO.Path]::GetFullPath($Path)
    $parent = [IO.Path]::GetFullPath($Directory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($parent, [StringComparison]::OrdinalIgnoreCase)) {
        throw ('Path outside expected directory: ' + $absolute)
    }
}

$stagedPackage = $null
$stagedChecksum = $null
try {
    $packageBytes = [IO.File]::ReadAllBytes($packagePath)
    $binaryHash = Get-BytesHash ([IO.File]::ReadAllBytes($binaryPath))
    $magic = [byte[]](49, 49, 49, 0, 0, 0, 0, 0, 98, 112, 107, 103)
    if ($packageBytes.Length -lt 52) { throw 'Incomplete Lite package.' }
    for ($i = 0; $i -lt $magic.Length; ++$i) {
        if ($packageBytes[$i] -ne $magic[$i]) { throw 'Unsupported Lite package format.' }
    }
    if ([Text.Encoding]::ASCII.GetString($packageBytes, 20, 6) -ne 'SHA256' -or
        [BitConverter]::ToUInt32($packageBytes, 16) -ne 4) {
        throw 'Expected the normal four-image SHA256 Lite package.'
    }
    $layout = @(
        @{ Address = 0x803000; Capacity = 0x16000; Source = 'components\basePkg\F6D_D\ap_bootloader.bin' },
        @{ Address = 0x8BE000; Capacity = 0x1D0000; Source = 'components\basePkg\F6D_D\ap_lierda_app.bin' },
        @{ Address = 0x81E000; Capacity = 0xA0000; Source = 'components\basePkg\F6D_D\cp-demo-flash.bin' },
        @{ Address = 0xAA6000; Capacity = 0xCB000; Source = 'gccout\tirtc_lite\TiRTC_lite_NT26F6D0.bin' }
    )
    $offset = 52 + [int][BitConverter]::ToUInt16($packageBytes, 30)
    for ($i = 0; $i -lt $layout.Count; ++$i) {
        if ($offset + 364 -gt $packageBytes.Length) { throw 'Truncated Lite image header.' }
        $address = [BitConverter]::ToUInt32($packageBytes, $offset + 64)
        $capacity = [BitConverter]::ToUInt32($packageBytes, $offset + 68)
        $position = [BitConverter]::ToUInt32($packageBytes, $offset + 72)
        $length = [BitConverter]::ToUInt32($packageBytes, $offset + 76)
        if ($address -ne $layout[$i].Address -or $capacity -ne $layout[$i].Capacity -or
            $position -ne $offset -or $length -eq 0 -or $length -gt $capacity -or
            [long]$offset + 364 + $length -gt $packageBytes.Length) {
            throw 'Lite image does not match the audited F6D_D layout.'
        }
        $expectedHash = [Text.Encoding]::ASCII.GetString($packageBytes, $offset + 80, 64).ToLowerInvariant()
        if ($expectedHash -notmatch '^[0-9a-f]{64}$' -or
            (Get-BytesHash $packageBytes ($offset + 364) ([int]$length)) -ne $expectedHash) {
            throw 'Lite image SHA256 mismatch.'
        }
        $inputHash = if ($i -eq 3) { $binaryHash } else {
            Get-BytesHash ([IO.File]::ReadAllBytes((Join-Path $repository $layout[$i].Source)))
        }
        if ($expectedHash -ne $inputHash -or ($i -eq 3 -and $length -ne $binaryLength)) {
            throw 'Lite package does not match the current build and F6D_D inputs.'
        }
        $offset += 364 + [int]$length
    }
    if ($offset -ne $packageBytes.Length) { throw 'Unexpected trailing Lite package bytes.' }
    $binaryInfo = Get-Item -LiteralPath $binaryPath
    if ($binaryInfo.LastWriteTimeUtc -ne $binaryWriteTime -or $binaryInfo.Length -ne $binaryLength) {
        throw 'Lite build changed during release validation; retry after the build completes.'
    }
    $packageHash = Get-BytesHash $packageBytes
    Assert-ChildPath $releaseDirectory $repository
    [IO.Directory]::CreateDirectory($releaseDirectory) | Out-Null
    if (Test-Path -LiteralPath $checksumPath -PathType Container) { throw 'Lite checksum path is a directory.' }
    if ((Test-Path -LiteralPath $releasePath) -and
        (-not (Test-Path -LiteralPath $releasePath -PathType Leaf) -or
         (Get-BytesHash ([IO.File]::ReadAllBytes($releasePath))) -ne $packageHash)) {
        throw 'The build timestamp already names a different Lite package; rebuild before publishing.'
    }
    $stageId = [Guid]::NewGuid().ToString('N')
    $stagedPackage = Join-Path $releaseDirectory ('.lite-' + $stageId + '.binpkg.tmp')
    $stagedChecksum = Join-Path $releaseDirectory ('.lite-' + $stageId + '.sha.tmp')
    [IO.File]::WriteAllBytes($stagedPackage, $packageBytes)
    if ((Get-BytesHash ([IO.File]::ReadAllBytes($stagedPackage))) -ne $packageHash) { throw 'Staged Lite package verification failed.' }
    [IO.File]::WriteAllText($stagedChecksum, $packageHash + '  ' + $releaseName + [char]10, [Text.Encoding]::ASCII)
    if (-not (Test-Path -LiteralPath $releasePath)) { [IO.File]::Move($stagedPackage, $releasePath) }
    if ([IO.File]::Exists($checksumPath)) { [IO.File]::Replace($stagedChecksum, $checksumPath, [NullString]::Value) }
    else { [IO.File]::Move($stagedChecksum, $checksumPath) }

    # Archive only recognized previous Lite packages after the verified release
    # and its checksum have both been published. Unknown files stay untouched.
    $previous = @(Get-ChildItem -LiteralPath $releaseDirectory -File | Where-Object {
        $_.Name -ne $releaseName -and ($_.Name -match '^\d{4}_\d{2}_\d{2}_\d{2}_\d{2}_\d{2}_lite\.binpkg$' -or $_.Name -eq 'latest.binpkg')
    })
    if ($previous.Count -gt 0) {
        $archiveDirectory = Join-Path $env:TEMP ('lierda-firmware-archive-' + [Guid]::NewGuid().ToString('N') + '\lite')
        Assert-ChildPath $archiveDirectory $env:TEMP
        [IO.Directory]::CreateDirectory($archiveDirectory) | Out-Null
        foreach ($file in $previous) {
            Assert-ChildPath $file.FullName $releaseDirectory
            $destination = Join-Path $archiveDirectory $file.Name
            Assert-ChildPath $destination $archiveDirectory
            Move-Item -LiteralPath $file.FullName -Destination $destination
        }
        Write-Output ('Previous Lite packages archived: ' + $archiveDirectory)
    }
    Write-Output ('Ready: ' + $releasePath)
    Write-Output ('SHA256: ' + $packageHash)
}
finally {
    foreach ($path in @($stagedPackage, $stagedChecksum)) {
        if ($path -and [IO.File]::Exists($path)) {
            Assert-ChildPath $path $releaseDirectory
            Remove-Item -LiteralPath $path
        }
    }
    $sha.Dispose()
}
