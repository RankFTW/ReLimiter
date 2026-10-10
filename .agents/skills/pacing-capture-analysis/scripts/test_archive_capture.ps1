#requires -Version 5.1
[CmdletBinding()]
param([string]$RepresentativePacing)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$archiver = Join-Path $PSScriptRoot 'archive_capture.ps1'
$root = Join-Path ([IO.Path]::GetTempPath()) ('relimiter-archive-test-' + [Guid]::NewGuid().ToString('N'))
$processName = 'ReLimiterArchiveFixture.game'
$prefixes = @('pacing', 'pacing_events')
# Families removed in Phase 1: present in old game folders, never archived.
$legacyPrefixes = @('frames', 'timeline', 'gpu_observations', 'presents', 'present_associations', 'provenance', 'pcl')
$sharedInputCount = 5
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
    foreach ($prefix in $Streams) {
        [IO.File]::WriteAllText((Join-Path $Directory "relimiter_${prefix}_${processName}_$Id.csv"),
            "row_kind,sequence`r`n1,1`r`n")
    }
}

# An empty Id writes the fixed process-based names of the oldest builds.
function Add-LegacyFiles([string]$Directory, [string]$Id) {
    $suffix = if ($Id) { "${processName}_$Id" } else { $processName }
    foreach ($prefix in $legacyPrefixes) {
        [IO.File]::WriteAllText((Join-Path $Directory "relimiter_${prefix}_$suffix.csv"),
            "frame_id,timestamp_us`r`n1,2`r`n")
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

    # One pacing group beside legacy files of the same, a newer and a fixed-name capture.
    $basic = New-Fixture 'basic'
    [IO.File]::WriteAllText((Join-Path $basic 'relimiter.ini'),
        "[FrameLimiter]`r`ngpu_readiness_observer=true`r`nready_present_mode=active`r`n")
    $single = '100_134330000000000000_capture_1'
    Add-Capture $basic $single
    Add-LegacyFiles $basic $single
    Add-LegacyFiles $basic '100_134330000000000005_capture_2'
    Add-LegacyFiles $basic ''
    $archive = & $archiver @arguments -SourceDirectory $basic -Strict
    $manifest = Read-Manifest $archive
    Assert-True ($manifest.schema_version -eq 3) 'Manifest v3'
    Assert-True ($manifest.runtime.gpu_readiness_observer -eq $true) 'Observer setting snapshot retained'
    Assert-True ($manifest.runtime.PSObject.Properties.Name -notcontains 'ready_present_mode') 'Deleted feature settings are not recorded'
    Assert-True ($manifest.capture_groups.Count -eq 1 -and $manifest.capture_groups[0].capture_id -eq $single) 'Legacy-only newer group is not a capture group'
    Assert-True ($manifest.capture_groups[0].PSObject.Properties.Name -notcontains 'naming') 'Every group uses session/epoch naming'
    Assert-True ($manifest.capture_groups[0].session_filetime -ceq '134330000000000000') 'Exact FILETIME string'
    Assert-True (@($manifest.capture_selection.discovered_ids).Count -eq 1) 'Only pacing streams are discovered'
    Assert-True ($manifest.files.Count -eq 2 + $sharedInputCount) 'Two pacing streams and five shared inputs'
    Assert-True (@($manifest.files | Where-Object { $_.kind -in @('relimiter_pacing', 'relimiter_pacing_events') -and
        $_.present -and $_.capture_id -eq $single }).Count -eq 2) 'Pacing rows and events are distinct streams'
    Assert-True (@(Get-ChildItem -LiteralPath $archive -Filter 'relimiter_*.csv' |
        Where-Object { $_.Name -notlike 'relimiter_pacing_*' }).Count -eq 0) 'No legacy family file is archived'
    Assert-ArchiveHashes $basic $archive

    # FILETIMEs differ below double precision; the older session has a larger
    # epoch and later mtime. Neither epoch alone nor lexical/double/mtime order works.
    $mixed = New-Fixture 'mixed'
    $older = '111_134330000000000000_capture_99'
    $epoch9 = '222_134330000000000001_capture_9'
    $newest = '222_134330000000000001_capture_11'
    foreach ($id in @($older, $epoch9, $newest)) { Add-Capture $mixed $id }
    Get-ChildItem -LiteralPath $mixed -File | Where-Object { $_.Name -like "*${older}.csv" } |
        ForEach-Object { $_.LastWriteTimeUtc = [datetime]'2035-01-01T00:00:00Z' }
    # A near-match process name must not be included (the real name contains '.').
    [IO.File]::WriteAllText((Join-Path $mixed 'relimiter_pacing_ReLimiterArchiveFixtureXgame_999_999999999999999999_capture_1.csv'), 'unrelated')
    $archive = & $archiver @arguments -SourceDirectory $mixed -Strict
    $manifest = Read-Manifest $archive
    Assert-True ($manifest.capture_groups.Count -eq 1 -and $manifest.capture_groups[0].capture_id -eq $newest) 'Numeric newest identity, not stream mtime'
    Assert-True ($null -eq $manifest.runtime.gpu_readiness_observer) 'Missing setting stays unknown'
    Assert-True ($manifest.capture_groups[0].session_filetime -ceq '134330000000000001') 'Exact newest FILETIME string'
    Assert-True ($manifest.capture_selection.excluded_ids.Count -eq 2) 'Excluded groups recorded'
    Assert-True (@($manifest.files | Where-Object { $_.capture_id -eq $newest -and $_.present }).Count -eq 2) 'Both streams from selected identity'
    Assert-ArchiveHashes $mixed $archive

    $allArchive = & $archiver @arguments -SourceDirectory $mixed -CaptureSelection All -Strict
    $manifest = Read-Manifest $allArchive
    Assert-True ($manifest.capture_groups.Count -eq 3 -and $manifest.files.Count -eq 3 * 2 + $sharedInputCount) 'All keeps three distinct groups and shared inputs once'
    Assert-True ($manifest.capture_selection.excluded_ids.Count -eq 0) 'All excludes no groups'
    Assert-True (@($manifest.files | Where-Object { $null -eq $_.capture_id }).Count -eq $sharedInputCount) 'Shared snapshots have no epoch identity'
    Assert-ArchiveHashes $mixed $allArchive
    $again = & $archiver @arguments -SourceDirectory $mixed -CaptureSelection All -Strict
    Assert-True ($again -ne $allArchive -and (Test-Path -LiteralPath $allArchive)) 'Repeated archival never overwrites'
    $exact = & $archiver @arguments -SourceDirectory $mixed -CaptureId $older -Strict
    $manifest = Read-Manifest $exact
    Assert-True ($manifest.capture_groups[0].capture_id -eq $older -and $manifest.capture_selection.mode -eq 'Exact') 'Exact older group'
    Assert-Fails { & $archiver @arguments -SourceDirectory $mixed -CaptureId legacy } '*CaptureId*'
    Assert-Fails { & $archiver @arguments -SourceDirectory $mixed -CaptureId $older -CaptureSelection All } '*cannot be combined*'
    Assert-Fails { & $archiver @arguments -SourceDirectory $mixed -CaptureId '999_999_capture_1' } '*CaptureId not found*'

    $incomplete = New-Fixture 'incomplete'
    Add-Capture $incomplete $older
    Add-Capture $incomplete $newest @('pacing')
    Assert-Fails { & $archiver @arguments -SourceDirectory $incomplete -Strict } "*Strict archive is missing inputs:*pacing_events*${newest}*"
    Assert-Fails { & $archiver @arguments -SourceDirectory $incomplete -CaptureSelection All -Strict } "*Strict archive is missing inputs:*pacing_events*${newest}*"
    $partialStreams = & $archiver @arguments -SourceDirectory $incomplete
    $manifest = Read-Manifest $partialStreams
    $missing = @($manifest.files | Where-Object { -not $_.present })
    Assert-True ($missing.Count -eq 1 -and $missing[0].capture_id -eq $newest -and
        $missing[0].expected_name -eq "relimiter_pacing_events_${processName}_$newest.csv" -and
        $null -eq $missing[0].path) 'Missing companion is explicit, not borrowed'
    $partialAll = & $archiver @arguments -SourceDirectory $incomplete -CaptureSelection All
    Assert-True (@((Read-Manifest $partialAll).files | Where-Object { -not $_.present }).Count -eq 1) 'Non-strict All preserves missing per-epoch entry'

    $orphan = '222_134330000000000001_capture_13'
    Add-Capture $incomplete $orphan @('pacing_events')
    Assert-Fails { & $archiver @arguments -SourceDirectory $incomplete } "*Required pacing capture is unavailable*${orphan}*"
    Assert-Fails { & $archiver @arguments -SourceDirectory $incomplete -CaptureSelection All } "*Required pacing capture is unavailable*${orphan}*"
    Add-Capture $incomplete '333_134330000000000002_capture_1'
    $afterOrphan = & $archiver @arguments -SourceDirectory $incomplete -Strict
    Assert-True ((Read-Manifest $afterOrphan).capture_groups[0].capture_id -eq '333_134330000000000002_capture_1') 'Newest may exclude older incomplete groups'
    Assert-Fails { & $archiver @arguments -SourceDirectory $incomplete -CaptureSelection All -Strict } "*Required pacing capture is unavailable*${orphan}*"

    $tie = New-Fixture 'tie'
    Add-Capture $tie '1_100_capture_1'
    Add-Capture $tie '2_100_capture_9'
    Assert-Fails { & $archiver @arguments -SourceDirectory $tie -Strict } '*ambiguous across PIDs*'
    $tieArchive = & $archiver @arguments -SourceDirectory $tie -CaptureSelection All -Strict
    Assert-True ((Read-Manifest $tieArchive).capture_groups.Count -eq 2) 'All preserves tied PIDs'

    $headerOnly = New-Fixture 'header-only'
    Add-Capture $headerOnly $newest
    [IO.File]::WriteAllText((Join-Path $headerOnly "relimiter_pacing_events_${processName}_$newest.csv"), "sequence,capture_epoch`r`n")
    $headerArchive = & $archiver @arguments -SourceDirectory $headerOnly -Strict
    $events = (Read-Manifest $headerArchive).files | Where-Object { $_.kind -eq 'relimiter_pacing_events' }
    Assert-True ($events.present -and $events.row_count -eq 0) 'Header-only stream is present with zero rows'

    $legacyOnly = New-Fixture 'legacy-only'
    Add-LegacyFiles $legacyOnly ''
    Add-LegacyFiles $legacyOnly $newest
    Assert-Fails { & $archiver @arguments -SourceDirectory $legacyOnly } '*No ReLimiter pacing captures*'

    $arguments.Architecture = 'Win32'
    $win32 = & $archiver @arguments -SourceDirectory $basic -Strict
    Assert-True (@((Read-Manifest $win32).files | Where-Object { $_.kind -eq 'relimiter_addon_win32' }).Count -eq 1) 'Win32 artifact selection'
    Remove-Item -LiteralPath (Join-Path $basic 'relimiter.addon32')
    Assert-Fails { & $archiver @arguments -SourceDirectory $basic } '*Required ReLimiter addon artifact*addon32*'
    $arguments.Architecture = 'x64'
    Remove-Item -LiteralPath (Join-Path $basic 'reshade.ini')
    Assert-Fails { & $archiver @arguments -SourceDirectory $basic -Strict } '*Strict archive is missing inputs*reshade.ini*'

    $arguments.DestinationRoot = $mixed
    Assert-Fails { & $archiver @arguments -SourceDirectory $mixed } '*must not overlap*'
    $arguments.DestinationRoot = Join-Path $root 'archives'
    $arguments.ProcessName = (Get-Process -Id $PID).ProcessName
    Assert-Fails { & $archiver @arguments -SourceDirectory $mixed } '*still running*'
    $arguments.ProcessName = $processName

    # Invalid header fails during copying: no .partial or final directory survives.
    $malformed = New-Fixture 'malformed'
    Add-Capture $malformed $single
    [IO.File]::WriteAllText((Join-Path $malformed "relimiter_pacing_${processName}_$single.csv"), '"unterminated')
    $beforeDirectories = @(Get-ChildItem -LiteralPath $arguments.DestinationRoot -Directory).Count
    Assert-Fails { & $archiver @arguments -SourceDirectory $malformed } '*Malformed quoted CSV header*'
    Assert-True (@(Get-ChildItem -LiteralPath $arguments.DestinationRoot -Directory).Count -eq $beforeDirectories) 'Copy failure publishes nothing and cleans partial'

    if ($RepresentativePacing) {
        $representativePath = (Resolve-Path -LiteralPath $RepresentativePacing).Path
        $originalHash = (Get-FileHash -LiteralPath $representativePath -Algorithm SHA256).Hash
        $representative = New-Fixture 'representative'
        Add-Capture $representative $newest
        Copy-Item -LiteralPath $representativePath -Destination (Join-Path $representative "relimiter_pacing_${processName}_$newest.csv")
        $representativeArchive = & $archiver @arguments -SourceDirectory $representative -Strict
        Assert-ArchiveHashes $representative $representativeArchive
        Assert-True ((Get-FileHash -LiteralPath $representativePath -Algorithm SHA256).Hash -eq $originalHash) 'Representative raw input unchanged'
        Write-Output 'Representative pacing copy/hash check passed.'
    }
    Write-Output "PASS: $script:assertions assertions (temporary fixtures only)."
}
finally {
    if (Test-Path -LiteralPath $root) { Remove-Item -LiteralPath $root -Recurse -Force }
}
