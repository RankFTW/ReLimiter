#Requires -Version 5.1
# Directly runs the utility with mocked group APIs. Never changes Windows groups.
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$utility = Join-Path $PSScriptRoot '..\tools\pcl_permissions.ps1'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
try { $sid = $identity.User.Value } finally { $identity.Dispose() }
$mockState = @{ Member = $false; EnumerationFailure = $false; MutationCount = 0 }

function Import-Module { param($Name, $ErrorAction) }
function Get-LocalGroup {
    param($SID)
    if ($SID.Value -ne 'S-1-5-32-559') { throw 'Wrong group SID' }
    [pscustomobject]@{ Name = 'Localized logging group' }
}
function Get-LocalGroupMember {
    param($Group, $ErrorAction)
    if ($Group -ne 'Localized logging group') { throw 'Localized group not used' }
    if ($mockState.EnumerationFailure) { throw 'Simulated enumeration failure' }
    if ($mockState.Member) {
        [pscustomobject]@{ SID = [Security.Principal.SecurityIdentifier]::new($sid) }
    }
}
function Add-LocalGroupMember { $mockState.MutationCount++; throw 'Unexpected mutation' }
function Remove-LocalGroupMember { $mockState.MutationCount++; throw 'Unexpected mutation' }
function Assert-True($value, $message) { if (-not $value) { throw $message } }
function Assert-Fails($arguments, $expected) {
    try { & $utility @arguments | Out-Null }
    catch {
        Assert-True ($_.Exception.Message -like "*$expected*") "Unexpected error: $_"
        return
    }
    throw "Expected failure: $expected"
}

$status = & $utility
Assert-True ($status.TargetSid -eq $sid -and -not $status.DirectMember) 'Default status failed'
Assert-True $status.TargetIsCurrentAccount 'Wrong current account'
Assert-Fails @{ Action = 'Enable'; Confirm = $false } 'requires -TargetSid'
Assert-Fails @{ Action = 'Remove'; Confirm = $false } 'requires -TargetSid'
Assert-Fails @{ TargetSid = 'invalid-sid' } 'sddlForm'
& $utility -Action Enable -TargetSid $sid -WhatIf | Out-Null
& $utility -Action Remove -TargetSid $sid -Confirm:$false | Out-Null
$mockState.Member = $true
$status = & $utility -Action Enable -TargetSid $sid -Confirm:$false
Assert-True $status.DirectMember 'Existing membership was not retained'
& $utility -Action Remove -TargetSid $sid -WhatIf | Out-Null
$mockState.EnumerationFailure = $true
Assert-Fails @{ Action = 'Enable'; TargetSid = $sid; Confirm = $false } 'Simulated enumeration failure'
Assert-True ($mockState.MutationCount -eq 0) 'A mutation was attempted'
Write-Host 'PASS: status, explicit identity, invalid SID, localized group, idempotence, dry-run, fail-closed enumeration; zero mutations.'