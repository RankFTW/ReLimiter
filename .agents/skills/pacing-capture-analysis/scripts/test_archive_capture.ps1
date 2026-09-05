#requires -Version 5.1
[CmdletBinding()]
param([string]$RepresentativeScheduler)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$archiver = Join-Path $PSScriptRoot 'archive_capture.ps1'
$root = Join-Path ([IO.Path]::GetTempPath()) ('relimiter-archive-test-' + [Guid]::NewGuid().ToString('N'))
$processName = 'ReLimiterArchiveFixture.game'
$prefixes = @('frames', 'timeline', 'gpu_observations', 'presents', 'present_associations', 'pcl')
$script:assertions = 0

function Assert-True([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw "Assertion failed: $Message" }
    ++$script:assertions
}

function Assert-Fails([scriptblock]$Action, [string]$Pattern) {
    $message = $null
    try { & $Action | Out-Null } catch { $message = $_.Exception.Message }
    Assert-True ($null -ne $message -and $message -like $Pattern) "Expected '$Pattern', got '$message'"
}

function New-Fixture([string]$Name) {
    $path = Join-Path $root $Name
    New-Item -ItemType Directory -Path $path | Out-Null
    foreach ($name in @('relimiter.ini', 'reshade.ini', 'ReShade.log',
            "relimiter_$processName.log", 'relimiter.addon64', 'relimiter.addon32')) {
        [IO.File]::WriteAllText((Join-Path $path $name), "fixture=$Name`r`n")
    }
    return $path
}

function Add-Capture([string]$Directory, [string]$Id, [string[]]$Streams = $prefixes) {
    $suffix = if ($Id -eq 'legacy') { $processName } else { "${processName}_$Id" }
    foreach ($prefix in $Streams) {
        [IO.File]::WriteAllText((Join-Path $Directory "relimiter_${prefix}_$suffix.csv"),
            "frame_id,timestamp_ms`r`n1,2`r`n")
    }
}

function Read-Manifest([string]$Archive) {
    return Get-Content -LiteralPath (Join-Path $Archive 'manifest.json') -Raw | ConvertFrom-Json
}

function Assert-ArchiveHashes([string]$Source, [string]$Archive) {
    $hashes = Get-Content -LiteralPath (Join-Path $Archive 'hashes.json') -Raw | ConvertFrom-Json
    foreach ($entry in $hashes) {
        $sourceHash = (Get-FileHash -LiteralPath (Join-Path $Source $entry.name) -Algorithm SHA256).Hash
        $copyHash = (Get-FileHash -LiteralPath (Join-Path $Archive $entry.name) -Algorithm SHA256).Hash
        Assert-True ($sourceHash -eq $entry.sha256 -and $copyHash -eq $entry.sha256) "Hash: $($entry.name)"
    }
}

try {
    New-Item -ItemType Directory -Path $root | Out-Null
    $arguments = @{
        ProcessName = $processName; ApplicationSlug = 'fixture'; Variant = 'test'
        Architecture = 'x64'; DestinationRoot = (Join-Path $root 'archives')
    }

    $legacy = New-Fixture 'legacy'
    Add-Capture $legacy 'legacy'
    $archive = & $archiver @arguments -SourceDirectory $legacy -Strict
    $manifest = Read-Manifest $archive
    Assert-True ($manifest.schema_version -eq 2) 'Manifest v2'
    Assert-True ($manifest.capture_groups.Count -eq 1 -and $manifest.capture_groups[0].capture_id -eq 'legacy') 'Legacy fallback'
    Assert-True ($null -eq $manifest.capture_groups[0].session_filetime) 'Legacy session remains unknown'
    Assert-True ($manifest.files.Count -eq 11) 'Six streams plus five shared inputs'
    Assert-ArchiveHashes $legacy $archive

    # FILETIMEs differ below double precision; the older session has a larger
    # epoch and later mtime. Neither epoch alone nor lexical/double/mtime order works.
    $mixed = New-Fixture 'mixed'
    $older = '111_134330000000000000_capture_99'
    $epoch9 = '222_134330000000000001_capture_9'
    $newest = '222_134330000000000001_capture_11'
    foreach ($id in @('legacy', $older, $epoch9, $newest)) { Add-Capture $mixed $id }
    Get-ChildItem -LiteralPath $mixed -File | Where-Object { $_.Name -like "*${older}.csv" } |
        ForEach-Object { $_.LastWriteTimeUtc = [datetime]'2035-01-01T00:00:00Z' }
    # A near-match process name must not be included (the real name contains '.').
    [IO.File]::WriteAllText((Join-Path $mixed 'relimiter_frames_ReLimiterArchiveFixtureXgame_999_999999999999999999_capture_1.csv'), 'unrelated')
    $archive = & $archiver @arguments -SourceDirectory $mixed -Strict
    $manifest = Read-Manifest $archive
    Assert-True ($manifest.capture_groups.Count -eq 1 -and $manifest.capture_groups[0].capture_id -eq $newest) 'Numeric newest identity, not stream mtime'
    Assert-True ($manifest.capture_groups[0].session_filetime -ceq '134330000000000001') 'Exact FILETIME string'
    Assert-True ($manifest.capture_selection.excluded_ids.Count -eq 3) 'Excluded groups recorded'
    Assert-True (@($manifest.files | Where-Object { $_.capture_id -eq $newest }).Count -eq 6) 'All six streams from selected identity'
    Assert-ArchiveHashes $mixed $archive

    $allArchive = & $archiver @arguments -SourceDirectory $mixed -CaptureSelection All -Strict
    $manifest = Read-Manifest $allArchive
    Assert-True ($manifest.capture_groups.Count -eq 4 -and $manifest.files.Count -eq 29) 'All keeps four distinct groups and shared inputs once'
    Assert-True ($manifest.capture_selection.excluded_ids.Count -eq 0) 'All excludes no groups'
    Assert-True (@($manifest.files | Where-Object { $null -eq $_.capture_id }).Count -eq 5) 'Shared snapshots have no epoch identity'
    Assert-ArchiveHashes $mixed $allArchive
    $again = & $archiver @arguments -SourceDirectory $mixed -CaptureSelection All -Strict
    Assert-True ($again -ne $allArchive -and (Test-Path -LiteralPath $allArchive)) 'Repeated archival never overwrites'
    $exact = & $archiver @arguments -SourceDirectory $mixed -CaptureId $older -Strict
    $manifest = Read-Manifest $exact
    Assert-True ($manifest.capture_groups[0].capture_id -eq $older -and $manifest.capture_selection.mode -eq 'Exact') 'Exact older group'
    $exactLegacy = & $archiver @arguments -SourceDirectory $mixed -CaptureId legacy -Strict
    Assert-True ((Read-Manifest $exactLegacy).capture_groups[0].capture_id -eq 'legacy') 'Exact legacy in mixed directory'
    Assert-Fails { & $archiver @arguments -SourceDirectory $mixed -CaptureId legacy -CaptureSelection All } '*cannot be combined*'
    Assert-Fails { & $archiver @arguments -SourceDirectory $mixed -CaptureId '999_999_capture_1' } '*CaptureId not found*'

    $incomplete = New-Fixture 'incomplete'
    Add-Capture $incomplete 'legacy'
    Add-Capture $incomplete $older
    Add-Capture $incomplete $newest @('frames', 'timeline', 'gpu_observations', 'presents', 'present_associations')
    Assert-Fails { & $archiver @arguments -SourceDirectory $incomplete -Strict } "*Strict archive is missing inputs:*pcl*${newest}*"
    Assert-Fails { & $archiver @arguments -SourceDirectory $incomplete -CaptureSelection All -Strict } "*Strict archive is missing inputs:*pcl*${newest}*"
    $partialStreams = & $archiver @arguments -SourceDirectory $incomplete
    $manifest = Read-Manifest $partialStreams
    $missing = @($manifest.files | Where-Object { -not $_.present })
    Assert-True ($missing.Count -eq 1 -and $missing[0].capture_id -eq $newest -and
        $missing[0].expected_name -eq "relimiter_pcl_${processName}_$newest.csv" -and
        $null -eq $missing[0].path) 'Missing companion is explicit, not borrowed'
    $partialAll = & $archiver @arguments -SourceDirectory $incomplete -CaptureSelection All
    Assert-True (@((Read-Manifest $partialAll).files | Where-Object { -not $_.present }).Count -eq 1) 'Non-strict All preserves missing per-epoch entry'

    $orphan = '222_134330000000000001_capture_13'
    Add-Capture $incomplete $orphan @('pcl')
    Assert-Fails { & $archiver @arguments -SourceDirectory $incomplete } "*Required scheduler capture is unavailable*${orphan}*"
    Assert-Fails { & $archiver @arguments -SourceDirectory $incomplete -CaptureSelection All } "*Required scheduler capture is unavailable*${orphan}*"
    Add-Capture $incomplete '333_134330000000000002_capture_1'
    $afterOrphan = & $archiver @arguments -SourceDirectory $incomplete -Strict
    Assert-True ((Read-Manifest $afterOrphan).capture_groups[0].capture_id -eq '333_134330000000000002_capture_1') 'Newest may exclude older incomplete groups'
    Assert-Fails { & $archiver @arguments -SourceDirectory $incomplete -CaptureSelection All -Strict } "*Required scheduler capture is unavailable*${orphan}*"

    $tie = New-Fixture 'tie'
    Add-Capture $tie '1_100_capture_1'
    Add-Capture $tie '2_100_capture_9'
    Assert-Fails { & $archiver @arguments -SourceDirectory $tie -Strict } '*ambiguous across PIDs*'
    $tieArchive = & $archiver @arguments -SourceDirectory $tie -CaptureSelection All -Strict
    Assert-True ((Read-Manifest $tieArchive).capture_groups.Count -eq 2) 'All preserves tied PIDs'

    $headerOnly = New-Fixture 'header-only'
    Add-Capture $headerOnly $newest
    [IO.File]::WriteAllText((Join-Path $headerOnly "relimiter_pcl_${processName}_$newest.csv"), "frame_id,timestamp_ms`r`n")
    $headerArchive = & $archiver @arguments -SourceDirectory $headerOnly -Strict
    $pcl = (Read-Manifest $headerArchive).files | Where-Object { $_.kind -eq 'relimiter_native_pcl' }
    Assert-True ($pcl.present -and $pcl.row_count -eq 0) 'Header-only stream is present with zero rows'

    $arguments.Architecture = 'Win32'
    $win32 = & $archiver @arguments -SourceDirectory $legacy -Strict
    Assert-True (@((Read-Manifest $win32).files | Where-Object { $_.kind -eq 'relimiter_addon_win32' }).Count -eq 1) 'Win32 artifact selection'
    Remove-Item -LiteralPath (Join-Path $legacy 'relimiter.addon32')
    Assert-Fails { & $archiver @arguments -SourceDirectory $legacy } '*Required ReLimiter addon artifact*addon32*'
    $arguments.Architecture = 'x64'
    Remove-Item -LiteralPath (Join-Path $legacy 'reshade.ini')
    Assert-Fails { & $archiver @arguments -SourceDirectory $legacy -Strict } '*Strict archive is missing inputs*reshade.ini*'

    $arguments.DestinationRoot = $mixed
    Assert-Fails { & $archiver @arguments -SourceDirectory $mixed } '*must not overlap*'
    $arguments.DestinationRoot = Join-Path $root 'archives'
    $arguments.ProcessName = (Get-Process -Id $PID).ProcessName
    Assert-Fails { & $archiver @arguments -SourceDirectory $mixed } '*still running*'
    $arguments.ProcessName = $processName

    # Invalid header fails during copying: no .partial or final directory survives.
    $malformed = New-Fixture 'malformed'
    Add-Capture $malformed 'legacy'
    [IO.File]::WriteAllText((Join-Path $malformed "relimiter_frames_$processName.csv"), '"unterminated')
    $beforeDirectories = @(Get-ChildItem -LiteralPath $arguments.DestinationRoot -Directory).Count
    Assert-Fails { & $archiver @arguments -SourceDirectory $malformed } '*Malformed quoted CSV header*'
    Assert-True (@(Get-ChildItem -LiteralPath $arguments.DestinationRoot -Directory).Count -eq $beforeDirectories) 'Copy failure publishes nothing and cleans partial'

    if ($RepresentativeScheduler) {
        $representativePath = (Resolve-Path -LiteralPath $RepresentativeScheduler).Path
        $originalHash = (Get-FileHash -LiteralPath $representativePath -Algorithm SHA256).Hash
        $representative = New-Fixture 'representative'
        Add-Capture $representative $newest
        Copy-Item -LiteralPath $representativePath -Destination (Join-Path $representative "relimiter_frames_${processName}_$newest.csv")
        $representativeArchive = & $archiver @arguments -SourceDirectory $representative -Strict
        Assert-ArchiveHashes $representative $representativeArchive
        Assert-True ((Get-FileHash -LiteralPath $representativePath -Algorithm SHA256).Hash -eq $originalHash) 'Representative raw input unchanged'
        Write-Output 'Representative scheduler copy/hash check passed.'
    }
    Write-Output "PASS: $script:assertions assertions (temporary fixtures only)."
}
finally {
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}