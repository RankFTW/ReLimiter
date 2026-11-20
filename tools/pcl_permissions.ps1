#Requires -Version 5.1
<#
.SYNOPSIS
Checks or changes Performance Log Users membership for native PCL collection.
.DESCRIPTION
Run Status as the gaming account first. Copy its TargetSid, then run Enable or
Remove from an administrator PowerShell with that explicit SID. Never assumes
that the administrator account is the gaming account. Does not self-elevate.
Requires 64-bit Windows PowerShell and Microsoft.PowerShell.LocalAccounts.
Membership grants account-wide performance logging rights, not just ReLimiter
access. Sign out of Windows and back in after changing membership.
Remove explicitly removes direct membership, including membership added by
another tool; no ownership of existing membership is assumed.
.EXAMPLE
.\tools\pcl_permissions.ps1
.EXAMPLE
.\tools\pcl_permissions.ps1 -Action Enable -TargetSid S-1-5-21-111-222-333-1001 -WhatIf
.EXAMPLE
.\tools\pcl_permissions.ps1 -Action Enable -TargetSid S-1-5-21-111-222-333-1001
.EXAMPLE
.\tools\pcl_permissions.ps1 -Action Remove -TargetSid S-1-5-21-111-222-333-1001
#>
[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
    [ValidateSet('Status', 'Enable', 'Remove')]
    [string] $Action = 'Status',

    [string] $TargetSid
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if ($env:OS -ne 'Windows_NT' -or -not [Environment]::Is64BitProcess) {
    throw 'Use 64-bit Windows PowerShell 5.1 to run this utility.'
}
if ($Action -ne 'Status' -and [string]::IsNullOrWhiteSpace($TargetSid)) {
    throw 'Enable/Remove requires -TargetSid. Run Status as the gaming account first; copy its TargetSid into the administrator-shell command.'
}

Import-Module Microsoft.PowerShell.LocalAccounts -ErrorAction Stop
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
try {
    $currentSid = $identity.User.Value
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    $isAdministrator = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    $groupSid = [Security.Principal.SecurityIdentifier]::new('S-1-5-32-559')
    $currentTokenMembership = $principal.IsInRole($groupSid)
}
finally {
    $identity.Dispose()
}

if ([string]::IsNullOrWhiteSpace($TargetSid)) { $TargetSid = $currentSid }
$accountSid = [Security.Principal.SecurityIdentifier]::new($TargetSid)
$accountName = $accountSid.Translate([Security.Principal.NTAccount]).Value
$group = Get-LocalGroup -SID $groupSid

function Test-DirectMembership {
    # Fail closed if Windows cannot enumerate membership (e.g. unresolved users).
    $members = @(Get-LocalGroupMember -Group $group.Name -ErrorAction Stop)
    return @($members | Where-Object { $_.SID.Value -eq $accountSid.Value }).Count -gt 0
}

$directMember = Test-DirectMembership
if ($Action -ne 'Status') {
    Write-Warning 'This changes account-wide performance logging permissions, not just ReLimiter permissions.'
    if (($Action -eq 'Enable' -and $directMember) -or
        ($Action -eq 'Remove' -and -not $directMember)) {
        Write-Host 'No change required to direct membership.'
    }
    else {
        $operation = if ($Action -eq 'Enable') { 'Add direct membership' } else { 'Remove direct membership, regardless of who added it' }
        if ($PSCmdlet.ShouldProcess("$accountName [$($accountSid.Value)] in $($group.Name)", $operation)) {
            if (-not $isAdministrator) {
                throw 'Open 64-bit Windows PowerShell as administrator and rerun with this explicit -TargetSid. This utility does not self-elevate.'
            }
            if ($Action -eq 'Enable') {
                Add-LocalGroupMember -Group $group.Name -Member $accountSid.Value -ErrorAction Stop
            }
            else {
                Remove-LocalGroupMember -Group $group.Name -Member $accountSid.Value -ErrorAction Stop
            }
            $directMember = Test-DirectMembership
            if ($directMember -ne ($Action -eq 'Enable')) {
                throw 'Membership verification failed. Inspect the local group before retrying.'
            }
            Write-Host 'Membership updated. Sign out of Windows and sign back in before launching the game.'
        }
    }
}

[pscustomobject]@{
    Account = $accountName
    TargetSid = $accountSid.Value
    Group = $group.Name
    DirectMember = $directMember
    CurrentProcessElevated = $isAdministrator
    # A different account's effective token cannot be inferred from this process.
    TargetIsCurrentAccount = ($accountSid.Value -eq $currentSid)
    CurrentTokenPerformanceLogUsers = $currentTokenMembership
}
Write-Host 'Direct membership and current-token membership can differ until sign-out/in, or due to nested groups.'
Write-Host 'Removing direct membership does not revoke access inherited through other groups or administrator privileges.'
Write-Host 'This checks permissions only; it does not start ETW or guarantee provider access or game PCL markers.'