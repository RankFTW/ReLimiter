#Requires -Version 5.1
[CmdletBinding()]
param(
    [ValidateSet('Enable', 'Remove')]
    [string] $Action,
    [string] $TargetSid,
    # Constructs and refreshes the window without displaying it or requesting UAC.
    [switch] $SmokeTest
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[Windows.Forms.Application]::EnableVisualStyles()
$backend = Join-Path $PSScriptRoot 'pcl_permissions.ps1'

try {
    if (-not [Environment]::Is64BitProcess) { throw 'Use the provided launcher or 64-bit Windows PowerShell.' }
    if ($Action) {
        if ($SmokeTest) { throw 'SmokeTest cannot be combined with a permission change.' }
        # The initiating window already confirmed the operation. The backend
        # still validates the explicit SID, elevation and resulting membership.
        & $backend -Action $Action -TargetSid $TargetSid -Confirm:$false | Out-Null
        exit 0
    }
    if ($TargetSid) { throw 'Launch the window as the gaming account; do not supply TargetSid to the interactive window.' }
    $initialStatus = & $backend
    if ($initialStatus.CurrentProcessElevated) {
        throw 'Open the launcher normally, not as administrator, so it identifies the gaming account before UAC.'
    }
}
catch {
    if ($SmokeTest) { throw }
    [void][Windows.Forms.MessageBox]::Show($_.Exception.Message, 'PCL permissions - error', 'OK', 'Error')
    exit 1
}

$script:targetAccountSid = $initialStatus.TargetSid
$script:worker = $null
$script:pendingAction = $null
$form = [Windows.Forms.Form]::new()
$form.Text = 'ReLimiter - PCL permissions'
$form.ClientSize = [Drawing.Size]::new(660, 440)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedDialog'
$form.MaximizeBox = $false
$form.AutoScaleMode = 'Dpi'
$form.Font = [Drawing.Font]::new('Segoe UI', 10)

$heading = [Windows.Forms.Label]::new()
$heading.Text = 'Native PCL permissions setup'
$heading.Font = [Drawing.Font]::new('Segoe UI', 15, [Drawing.FontStyle]::Bold)
$heading.SetBounds(20, 16, 620, 36)
$account = [Windows.Forms.TextBox]::new()
$account.Multiline = $true
$account.ReadOnly = $true
$account.Text = "$($initialStatus.Account)`r`n$script:targetAccountSid"
$account.SetBounds(20, 60, 620, 56)
$statusLabel = [Windows.Forms.Label]::new()
$statusLabel.SetBounds(20, 130, 620, 52)
$notice = [Windows.Forms.Label]::new()
$notice.Text = "Enabling grants account-wide performance logging rights to applications running as this user, not just ReLimiter.`r`n`r`nSign out of Windows and back in after changes. This is permissions setup, not a measurement viewer or a guarantee of ETW/PCL availability."
$notice.SetBounds(20, 190, 620, 104)
$enable = [Windows.Forms.Button]::new()
$enable.Text = 'Enable permissions'
$enable.SetBounds(20, 310, 175, 36)
$remove = [Windows.Forms.Button]::new()
$remove.Text = 'Remove permissions'
$remove.SetBounds(205, 310, 175, 36)
$refresh = [Windows.Forms.Button]::new()
$refresh.Text = 'Refresh'
$refresh.SetBounds(390, 310, 100, 36)
$result = [Windows.Forms.Label]::new()
$result.SetBounds(20, 360, 620, 64)
$form.Controls.AddRange(@($heading, $account, $statusLabel, $notice, $enable, $remove, $refresh, $result))
$timer = [Windows.Forms.Timer]::new()
$timer.Interval = 250

function Update-PermissionsStatus {
    $enable.Enabled = $false
    $remove.Enabled = $false
    try {
        $status = & $backend -TargetSid $script:targetAccountSid
        $statusLabel.Text = "Direct group membership: $($status.DirectMember)`r`nCurrent logon token membership: $($status.CurrentTokenPerformanceLogUsers)"
        $enable.Enabled = -not $status.DirectMember -and $null -eq $script:worker
        $remove.Enabled = $status.DirectMember -and $null -eq $script:worker
        return $status
    }
    catch {
        $statusLabel.Text = 'Permission status unavailable. No changes are enabled.'
        $result.Text = $_.Exception.Message
        return $null
    }
}

function Start-PermissionsChange([string] $requestedAction) {
    if ($null -ne $script:worker) { return }
    $warning = if ($requestedAction -eq 'Enable') {
        'Grant account-wide performance logging permissions?'
    } else {
        'Remove direct membership even if another tool added it? Access inherited through other groups or administrator privileges will remain.'
    }
    $answer = [Windows.Forms.MessageBox]::Show($form,
        "$warning`r`n`r`nAccount: $($initialStatus.Account)`r`nSID: $script:targetAccountSid`r`n`r`nAdministrator approval is required. Sign out/in afterward.",
        'Confirm permission change', 'YesNo', 'Warning', 'Button2')
    if ($answer -ne [Windows.Forms.DialogResult]::Yes) { return }
    try {
        $enable.Enabled = $false
        $remove.Enabled = $false
        $refresh.Enabled = $false
        $result.Text = 'Waiting for administrator approval and permission update...'
        # File paths cannot contain double quotes. SID and action are validated,
        # and -File avoids interpolating them into executable PowerShell code.
        $arguments = '-NoProfile -NonInteractive -STA -File "{0}" -Action {1} -TargetSid {2}' -f $PSCommandPath, $requestedAction, $script:targetAccountSid
        $script:worker = Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList $arguments -Verb RunAs -PassThru
        $script:pendingAction = $requestedAction
        $timer.Start()
    }
    catch {
        $result.Text = "Operation cancelled or could not start: $($_.Exception.Message)"
        $refresh.Enabled = $true
        [void](Update-PermissionsStatus)
    }
}

$enable.Add_Click({ Start-PermissionsChange 'Enable' })
$remove.Add_Click({ Start-PermissionsChange 'Remove' })
$refresh.Add_Click({
    $result.Text = ''
    [void](Update-PermissionsStatus)
})
$timer.Add_Tick({
    if ($null -eq $script:worker -or -not $script:worker.HasExited) { return }
    $timer.Stop()
    $exitCode = $script:worker.ExitCode
    $script:worker.Dispose()
    $script:worker = $null
    $refresh.Enabled = $true
    $status = Update-PermissionsStatus
    if ($null -eq $status) { return }
    if ($exitCode -eq 0 -and $status.DirectMember -eq ($script:pendingAction -eq 'Enable')) {
        $result.Text = 'Requested membership verified. Sign out of Windows and back in before launching the game.'
    } else {
        $result.Text = "Update failed or membership could not be verified (exit $exitCode). Check the worker error/console; script policy may block execution."
    }
})
$form.Add_FormClosing({
    param($sender, $eventArgs)
    if ($null -ne $script:worker) {
        $eventArgs.Cancel = $true
        $result.Text = 'Wait for the permission operation to finish before closing.'
    }
})

try {
    $status = Update-PermissionsStatus
    if ($SmokeTest) {
        if ($null -eq $status) { throw $result.Text }
        Write-Output 'PASS: GUI constructed and read-only status refreshed; no window, UAC or permission mutation.'
    } else {
        [void]$form.ShowDialog()
    }
}
finally {
    $timer.Dispose()
    $form.Dispose()
}