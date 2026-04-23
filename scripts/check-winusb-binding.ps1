param(
    [string]$Vid = "CAFE"
)

$pattern = "VID_$Vid"
$devices = Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like "*$pattern*" }

if (-not $devices) {
    Write-Host "No present USB devices with VID_$Vid found."
    exit 1
}

Write-Host "Found devices:"
$devices | Format-Table -AutoSize Status, Class, FriendlyName, InstanceId

Write-Host ""
Write-Host "Tip: ensure the HFSDR vendor interface shows WinUSB service (WinUSB.sys)."
