#requires -Version 5.1

<#
.SYNOPSIS
Archives stopped ReLimiter pacing captures without renaming or combining streams.
.DESCRIPTION
Archives the joined pacing families relimiter_pacing_<process>_<id>.csv and
relimiter_pacing_events_<process>_<id>.csv, where <id> is
PID_session_capture_epoch. Files of the legacy families removed in Phase 1 are
never archived. Newest (default) selects the greatest numeric session FILETIME,
then epoch; modification times do not determine capture order. All selects
every group. CaptureId selects one exact PID_session_capture_epoch suffix; it
cannot be combined with All.
Every selected group requires its own pacing rows CSV. Strict additionally
requires the pacing events CSV of every group, both INIs, both logs, and the
chosen addon. Shared support files are archival-time snapshots, not proven
epoch metadata.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDirectory,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$ProcessName,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$ApplicationSlug,

    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[A-Za-z0-9][A-Za-z0-9._-]*$')]
    [string]$Variant,

    [Parameter(Mandatory = $true)]
    [ValidateSet('x64', 'Win32')]
    [string]$Architecture,

    [string]$Application,
    [string]$Description,
    [string]$PairId,
    [string]$DestinationRoot,
    [ValidateSet('Newest', 'All')]
    [string]$CaptureSelection = 'Newest',
    [ValidatePattern('^[1-9][0-9]*_[1-9][0-9]*_capture_[1-9][0-9]*$')]
    [string]$CaptureId,
    [switch]$Strict
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-HeaderSha256([string]$Path) {
    $reader = [System.IO.File]::OpenText($Path)
    try {
        $header = $reader.ReadLine()
    }
    finally {
        $reader.Dispose()
    }
    if ($null -eq $header) { return $null }

    # Match inspect_capture.py: decode CSV fields, join their ordered values
    # with commas, then hash UTF-8 bytes without a line terminator.
    $fields = New-Object 'System.Collections.Generic.List[string]'
    $field = New-Object System.Text.StringBuilder
    $quoted = $false
    for ($index = 0; $index -lt $header.Length; ++$index) {
        $character = $header[$index]
        if ($character -eq '"') {
            if ($quoted -and $index + 1 -lt $header.Length -and
                $header[$index + 1] -eq '"') {
                [void]$field.Append('"')
                ++$index
            }
            else {
                $quoted = -not $quoted
            }
        }
        elseif ($character -eq ',' -and -not $quoted) {
            $fields.Add($field.ToString())
            [void]$field.Clear()
        }
        else {
            [void]$field.Append($character)
        }
    }
    if ($quoted) { throw "Malformed quoted CSV header: $Path" }
    $fields.Add($field.ToString())
    $normalizedHeader = [string]::Join(',', $fields)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($normalizedHeader)
        return -join ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') })
    }
    finally {
        $sha.Dispose()
    }
}

function Get-CsvRowCount([string]$Path) {
    $lineCount = 0
    $reader = [System.IO.File]::OpenText($Path)
    try {
        while ($null -ne $reader.ReadLine()) { ++$lineCount }
    }
    finally {
        $reader.Dispose()
    }
    return [Math]::Max(0, $lineCount - 1)
}

function Read-IniValues([string]$Path) {
    $values = @{}
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return $values }
    foreach ($line in [System.IO.File]::ReadLines($Path)) {
        $trimmed = $line.Trim()
        if ($trimmed.Length -eq 0 -or $trimmed.StartsWith('[') -or
            $trimmed.StartsWith(';') -or $trimmed.StartsWith('#')) {
            continue
        }
        $parts = $trimmed.Split('=', 2)
        if ($parts.Count -eq 2) {
            $values[$parts[0].Trim()] = $parts[1].Trim()
        }
    }
    return $values
}

function Ini-String($Values, [string]$Name) {
    if ($Values.ContainsKey($Name)) { return $Values[$Name] }
    return $null
}

function Ini-Number($Values, [string]$Name) {
    if (-not $Values.ContainsKey($Name)) { return $null }
    $parsed = 0.0
    if ([double]::TryParse(
            $Values[$Name],
            [Globalization.NumberStyles]::Float,
            [Globalization.CultureInfo]::InvariantCulture,
            [ref]$parsed)) {
        return $parsed
    }
    return $null
}

function Ini-Boolean($Values, [string]$Name) {
    if (-not $Values.ContainsKey($Name)) { return $null }
    switch ($Values[$Name].ToLowerInvariant()) {
        'true' { return $true }
        'false' { return $false }
        default { return $null }
    }
}

if ($ProcessName.EndsWith('.exe', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'ProcessName must omit the .exe suffix.'
}
$reservedNames = @('CON', 'PRN', 'AUX', 'NUL', 'COM1', 'COM2', 'COM3', 'COM4',
    'COM5', 'COM6', 'COM7', 'COM8', 'COM9', 'LPT1', 'LPT2', 'LPT3', 'LPT4',
    'LPT5', 'LPT6', 'LPT7', 'LPT8', 'LPT9')
if ($reservedNames -contains $ProcessName.Split('.')[0].ToUpperInvariant()) {
    throw "Reserved Windows process/file name: $ProcessName"
}
if (Get-Process -Name $ProcessName -ErrorAction SilentlyContinue) {
    throw "Process '$ProcessName' is still running; stop it before archival."
}

$source = (Resolve-Path -LiteralPath $SourceDirectory).Path
if (-not (Test-Path -LiteralPath $source -PathType Container)) {
    throw "Source directory does not exist: $SourceDirectory"
}
if (((Get-Item -LiteralPath $source).Attributes -band
        [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "Reparse-point source directories are not supported: $source"
}

if ([string]::IsNullOrWhiteSpace($DestinationRoot)) {
    $DestinationRoot = Join-Path $PSScriptRoot '..\..\..\..\debug_data\sessions'
}
if (-not (Test-Path -LiteralPath $DestinationRoot -PathType Container)) {
    New-Item -ItemType Directory -Path $DestinationRoot | Out-Null
}
$destinationRootPath = (Resolve-Path -LiteralPath $DestinationRoot).Path
if (((Get-Item -LiteralPath $destinationRootPath).Attributes -band
        [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "Reparse-point destination roots are not supported: $destinationRootPath"
}
$sourcePrefix = $source.TrimEnd('\') + '\'
$destinationPrefix = $destinationRootPath.TrimEnd('\') + '\'
if ($source.Equals($destinationRootPath, [StringComparison]::OrdinalIgnoreCase) -or
    $destinationPrefix.StartsWith($sourcePrefix, [StringComparison]::OrdinalIgnoreCase) -or
    $sourcePrefix.StartsWith($destinationPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Source and destination roots must not overlap.'
}

$stamp = (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssfffZ')
$nonce = [Guid]::NewGuid().ToString('N').Substring(0, 8)
$runId = ('{0}-{1}-{2}-{3}' -f $ApplicationSlug, $Variant, $stamp, $nonce).ToLowerInvariant()
$destination = Join-Path $destinationRootPath $runId
$partial = "$destination.partial-$([Guid]::NewGuid().ToString('N'))"
if ((Test-Path -LiteralPath $destination) -or (Test-Path -LiteralPath $partial)) {
    throw "Capture destination already exists: $destination"
}

# Archived streams. Every group needs its rows file; strict mode also needs its
# events file. Exact expected names keep the two prefixes distinct.
$streamSpecs = @(
    @{ Prefix = 'pacing'; Kind = 'relimiter_pacing' },
    @{ Prefix = 'pacing_events'; Kind = 'relimiter_pacing_events' }
)
$primaryStreamKind = 'relimiter_pacing'
$captureGroups = @{}
$capturePattern = '^relimiter_(?:' + (($streamSpecs | ForEach-Object { $_.Prefix }) -join '|') +
    ')_' + [regex]::Escape($ProcessName) +
    '_(?<id>(?<pid>[1-9][0-9]*)_(?<session>[1-9][0-9]*)_capture_(?<epoch>[1-9][0-9]*))\.csv$'
# Discover from both streams: an incomplete newer epoch must not silently fall
# back to an older rows file or borrow that older epoch's events file.
foreach ($file in Get-ChildItem -LiteralPath $source -File) {
    if ($file.Name -notmatch $capturePattern) { continue }
    $id = $Matches['id']
    $captureGroups[$id] = [pscustomobject]@{
        Id = $id
        ProcessId = [uint32]::Parse($Matches['pid'])
        Session = [uint64]::Parse($Matches['session'])
        Epoch = [uint32]::Parse($Matches['epoch'])
    }
}
if ($captureGroups.Count -eq 0) {
    throw "No ReLimiter pacing captures for process '$ProcessName' in $source"
}
$orderedGroups = @($captureGroups.Values | Sort-Object Session, Epoch, ProcessId)
if (-not [string]::IsNullOrWhiteSpace($CaptureId)) {
    if ($CaptureSelection -eq 'All') { throw 'CaptureId cannot be combined with CaptureSelection All.' }
    if (-not $captureGroups.ContainsKey($CaptureId)) { throw "CaptureId not found: $CaptureId" }
    $selectedGroups = @($captureGroups[$CaptureId])
}
elseif ($CaptureSelection -eq 'All') {
    $selectedGroups = $orderedGroups
}
else {
    $newest = $orderedGroups[-1]
    $latestProcessIds = @($orderedGroups | Where-Object { $_.Session -eq $newest.Session } |
        Select-Object -ExpandProperty ProcessId -Unique)
    if ($latestProcessIds.Count -gt 1) {
        throw 'Newest capture is ambiguous across PIDs; specify CaptureId or CaptureSelection All.'
    }
    $selectedGroups = @($newest)
}
$fileSpecs = @()
foreach ($group in $selectedGroups) {
    foreach ($stream in $streamSpecs) {
        $fileSpecs += @{
            Name = "relimiter_$($stream.Prefix)_${ProcessName}_$($group.Id).csv"
            Kind = $stream.Kind
            CaptureId = $group.Id
        }
    }
}
$fileSpecs += @(
    @{ Name = 'relimiter.ini'; Kind = 'relimiter_config'; CaptureId = $null },
    @{ Name = 'reshade.ini'; Kind = 'reshade_config'; CaptureId = $null },
    @{ Name = 'ReShade.log'; Kind = 'reshade_log'; CaptureId = $null },
    @{ Name = "relimiter_$ProcessName.log"; Kind = 'relimiter_log'; CaptureId = $null }
)
if ($Architecture -eq 'x64') {
    $addonName = 'relimiter.addon64'
    $addonKind = 'relimiter_addon_x64'
}
else {
    $addonName = 'relimiter.addon32'
    $addonKind = 'relimiter_addon_win32'
}
$fileSpecs += @{ Name = $addonName; Kind = $addonKind; CaptureId = $null }

foreach ($spec in $fileSpecs | Where-Object { $_.Kind -eq $primaryStreamKind }) {
    if (-not (Test-Path -LiteralPath (Join-Path $source $spec.Name) -PathType Leaf)) {
        throw "Required pacing capture is unavailable for '$($spec.CaptureId)': $($spec.Name)"
    }
}
if (-not (Test-Path -LiteralPath (Join-Path $source $addonName) -PathType Leaf)) {
    throw "Required ReLimiter addon artifact is unavailable: $addonName"
}
if ($Strict) {
    $missing = @($fileSpecs | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $source $_.Name) -PathType Leaf)
    } | ForEach-Object { $_.Name })
    if ($missing.Count -ne 0) {
        throw "Strict archive is missing inputs: $($missing -join ', ')"
    }
}

try {
    New-Item -ItemType Directory -Path $partial | Out-Null
    $hashes = @()
    $manifestFiles = @()
    foreach ($spec in $fileSpecs) {
        $sourcePath = Join-Path $source $spec.Name
        if (-not (Test-Path -LiteralPath $sourcePath -PathType Leaf)) {
            $manifestFiles += [ordered]@{
                kind = $spec.Kind
                capture_id = $spec.CaptureId
                expected_name = $spec.Name
                present = $false
                path = $null
                size_bytes = $null
                sha256 = $null
                ordered_header_sha256 = $null
                row_count = $null
            }
            continue
        }
        $before = Get-Item -LiteralPath $sourcePath
        if (($before.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse-point inputs are not supported: $sourcePath"
        }
        $beforeLength = $before.Length
        $beforeWriteUtc = $before.LastWriteTimeUtc
        $beforeHash = Get-Sha256 $sourcePath

        $destinationPath = Join-Path $partial $spec.Name
        Copy-Item -LiteralPath $sourcePath -Destination $destinationPath

        $after = Get-Item -LiteralPath $sourcePath
        $afterHash = Get-Sha256 $sourcePath
        $destinationHash = Get-Sha256 $destinationPath
        if ($beforeLength -ne $after.Length -or
            $beforeWriteUtc -ne $after.LastWriteTimeUtc -or
            $beforeHash -ne $afterHash -or
            $beforeHash -ne $destinationHash) {
            throw "Source changed or copy hash mismatched: $($spec.Name)"
        }
        $item = Get-Item -LiteralPath $destinationPath
        $hashes += [ordered]@{
            name = $spec.Name
            kind = $spec.Kind
            capture_id = $spec.CaptureId
            size_bytes = $item.Length
            sha256 = $destinationHash
            source_last_write_utc = $beforeWriteUtc.ToString('o')
        }
        $isCsv = $spec.Name.EndsWith('.csv', [StringComparison]::OrdinalIgnoreCase)
        $headerSha256 = $null
        $rowCount = $null
        if ($isCsv) {
            $headerSha256 = Get-HeaderSha256 $destinationPath
            $rowCount = Get-CsvRowCount $destinationPath
        }
        $manifestFiles += [ordered]@{
            kind = $spec.Kind
            capture_id = $spec.CaptureId
            expected_name = $spec.Name
            present = $true
            path = $spec.Name
            size_bytes = $item.Length
            sha256 = $destinationHash
            ordered_header_sha256 = $headerSha256
            row_count = $rowCount
        }
    }

    $ini = Read-IniValues (Join-Path $partial 'relimiter.ini')
    $addon = $manifestFiles | Where-Object { $_.kind -like 'relimiter_addon_*' } | Select-Object -First 1
    $manifestPairId = if ([string]::IsNullOrWhiteSpace($PairId)) { $null } else { $PairId }
    $manifestApplication = if ([string]::IsNullOrWhiteSpace($Application)) { $null } else { $Application }
    $manifestDescription = if ([string]::IsNullOrWhiteSpace($Description)) { $null } else { $Description }
    $manifestArchitecture = $Architecture
    $manifestAddonHash = if ($null -ne $addon) { $addon.sha256 } else { $null }
    $manifest = [ordered]@{
        schema_version = 3
        run_id = $runId
        pair_id = $manifestPairId
        variant = $Variant
        capture_selection = [ordered]@{
            mode = if ($CaptureId) { 'Exact' } else { $CaptureSelection }
            newest_order = 'numeric session FILETIME, then epoch; never file mtime'
            discovered_ids = @($orderedGroups | ForEach-Object { $_.Id })
            excluded_ids = @($orderedGroups | Where-Object { $_.Id -notin $selectedGroups.Id } |
                ForEach-Object { $_.Id })
        }
        capture_groups = @($selectedGroups | ForEach-Object {
            [ordered]@{
                capture_id = $_.Id
                process_id = $_.ProcessId
                session_filetime = $_.Session.ToString()
                capture_epoch = $_.Epoch
            }
        })
        captured_at_utc = $null
        archived_at_utc = (Get-Date).ToUniversalTime().ToString('o')
        scenario = [ordered]@{
            application = $manifestApplication
            executable = "$ProcessName.exe"
            game_version = $null
            description = $manifestDescription
            route_or_scene = $null
            warmup_seconds = $null
            capture_seconds = $null
            run_order = $null
            annotations = @()
        }
        software = [ordered]@{
            relimiter_version = $null
            relimiter_build = $null
            relimiter_architecture = $manifestArchitecture
            relimiter_artifact_sha256 = $manifestAddonHash
            reshade_version = $null
            os_version = $null
            gpu = $null
            gpu_driver = $null
            cpu = $null
            frameview_version = $null
        }
        display = [ordered]@{
            model = $null
            resolution = $null
            refresh_hz = $null
            vrr_enabled = $null
            vrr_floor_hz = $null
            vrr_ceiling_hz = $null
            window_mode = Ini-String $ini 'window_mode'
            present_mode = $null
            vsync_mode = Ini-String $ini 'vsync_mode'
            tearing_allowed = $null
        }
        runtime = [ordered]@{
            configured_values_source = 'archived_relimiter.ini'
            api = $null
            enforcement_path = $null
            marker_source = Ini-String $ini 'enforcement_marker'
            reflex_state = $null
            target_fps = Ini-Number $ini 'target_fps'
            pacing_mode = $null
            wake_guard_us = Ini-Number $ini 'initial_wake_guard_us'
            background_fps = Ini-Number $ini 'background_fps'
            fg_off_fps = Ini-Number $ini 'fg_off_fps'
            fg_type = $null
            fg_configured_multiplier = $null
            fg_detected_multiplier = $null
            dmfg_mode = Ini-Boolean $ini 'dynamic_mfg_passthrough'
            dmfg_driver_target_fps = $null
            adaptive_enabled = Ini-Boolean $ini 'adaptive_smoothing'
            adaptive_percentile = Ini-Number $ini 'smoothing_percentile'
            adaptive_window = Ini-String $ini 'smoothing_window'
            adaptive_release_speed = Ini-Number $ini 'smoothing_release_speed'
            adaptive_bias_us = Ini-Number $ini 'smoothing_bias_us'
            adaptive_deadline_reserve_mode = Ini-String $ini 'adaptive_deadline_reserve_mode'
            estimator_a_mode = Ini-String $ini 'estimator_a_mode'
            estimator_a_fixed_reserve_us = Ini-Number $ini 'estimator_a_fixed_reserve_us'
            presentation_gate_location = Ini-String $ini 'presentation_gate_location'
            native_arrival_predictor_mode = Ini-String $ini 'native_arrival_predictor_mode'
            gpu_readiness_observer = Ini-Boolean $ini 'gpu_readiness_observer'
            gpu_present_gate_mode = Ini-String $ini 'gpu_present_gate_mode'
            gpu_present_gate_slack_us = Ini-Number $ini 'gpu_present_gate_slack_us'
            gpu_present_gate_max_delay_us = Ini-Number $ini 'gpu_present_gate_max_delay_us'
            native_pcl_enabled = Ini-Boolean $ini 'native_pcl_enabled'
            native_pcl_status = $null
        }
        files = $manifestFiles
        integrity_notes = @(
            'Raw inputs were copied only after the named process was absent.',
            'Every source was stable across pre/post hashing and matched its archived copy.',
            'Streams are associated only by exact filename capture_id; never pool different groups as one run.',
            'Only the joined pacing families were archived; legacy family files in the source directory were not copied.',
            'Shared addon/config/log files are archival-time snapshots, not verified metadata for any selected epoch.',
            'Values parsed from relimiter.ini are configured values, not proof of active runtime authority.',
            'captured_at_utc and unavailable environment metadata remain null rather than inferred.'
        )
    }

    $hashes | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $partial 'hashes.json') -Encoding utf8
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $partial 'manifest.json') -Encoding utf8
    if (Get-Process -Name $ProcessName -ErrorAction SilentlyContinue) {
        throw "Process '$ProcessName' restarted during archival; no final archive was published."
    }
    Move-Item -LiteralPath $partial -Destination $destination
    Write-Output $destination
}
catch {
    if (Test-Path -LiteralPath $partial) {
        Remove-Item -LiteralPath $partial -Recurse -Force
    }
    throw
}
