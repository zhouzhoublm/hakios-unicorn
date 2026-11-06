#requires -Version 5.1
<#
.SYNOPSIS
  One boot attempt on the Xiaomi 12S Pro (unicorn), observed over USB, and the phone handed back to Android.

.DESCRIPTION
  Everything the AI needs to run an iteration without a human in the loop:

    1. reboot the phone into ABL fastboot            (adb reboot bootloader)
    2. snapshot every USB device, then `fastboot boot <image>`
    3. poll the USB bus every 500 ms and timestamp every device that appears or disappears,
       so "our code never enumerated" is a recorded fact, not a guess
    4. if the loader's fastboot device (`xnuloader`) appears, pull its log:
       oem log / get_staged, and `getvar result` for its own verdict
    5. always return the phone to Android (fastboot reboot, or wait for adb) and save one directory per attempt

  Safe by construction: `fastboot boot` is RAM only; nothing is flashed and no partition is written.

.PARAMETER Image
  The boot image to start (e.g. out/xnu-unicorn-selftest.img).

.PARAMETER WaitSec
  How long to watch USB after the handoff. Default 90: the phone resets to Android by itself at ~60 s today,
  so a longer window only delays the recovery.

.PARAMETER OutDir
  Where to write the attempt directory. Default: <repo>/../../out/pc-attempts/<timestamp>.

.PARAMETER PlatformTools
  Directory holding adb.exe / fastboot.exe. Default: D:\iOS\unicorn-port\tools\platform-tools.

.PARAMETER NoBoot
  Skip step 2-4: only enter fastboot, dump `getvar all` and return to Android (a liveness check).

.PARAMETER Recover
  Do not boot anything: just put the phone back into Android (after a manual experiment).

.EXAMPLE
  tools/pc/boot-attempt.ps1 -Image out/xnu-unicorn-selftest.img

.EXAMPLE
  tools/pc/boot-attempt.ps1 -Recover
#>
[CmdletBinding()]
param(
  [string]$Image,
  [int]$WaitSec = 90,
  [string]$OutDir,
  [string]$PlatformTools = 'D:\iOS\unicorn-port\tools\platform-tools',
  [switch]$NoBoot,
  [switch]$Recover
)

$ErrorActionPreference = 'Continue'
$adb = Join-Path $PlatformTools 'adb.exe'
$fb  = Join-Path $PlatformTools 'fastboot.exe'
foreach ($exe in @($adb, $fb)) { if (-not (Test-Path $exe)) { throw "missing $exe (set -PlatformTools)" } }
if (-not $Recover -and -not $NoBoot) {
  if (-not $Image) { throw '-Image is required (or use -NoBoot / -Recover)' }
  $Image = (Resolve-Path $Image).Path
}
if (-not $OutDir) {
  $OutDir = Join-Path (Split-Path (Split-Path $PSScriptRoot -Parent) -Parent) ("out/pc-attempts/" + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$log = Join-Path $OutDir 'attempt.log'
$t0 = Get-Date

function Say([string]$m) {
  $line = '{0}  {1}' -f ((Get-Date).ToString('HH:mm:ss.fff')), $m
  Write-Host $line
  Add-Content -Path $log -Value $line
}
function Wait-Fastboot([int]$timeoutSec = 30) {
  for ($i = 0; $i -lt ($timeoutSec * 2); $i++) {
    $d = (& $fb devices 2>&1) -join ' '
    if ($d -match 'fastboot') { return $true }
    Start-Sleep -Milliseconds 500
  }
  return $false
}
function Wait-Android([int]$timeoutSec = 90) {
  for ($i = 0; $i -lt ($timeoutSec * 2); $i++) {
    $d = (& $adb devices 2>&1) -join ' '
    if ($d -match '\sdevice') { return $true }
    Start-Sleep -Milliseconds 500
  }
  return $false
}
function Get-UsbDevices {
  $h = @{}
  foreach ($d in (Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue | Where-Object { $_.InstanceId -like 'USB\VID_*' })) {
    $h[$d.InstanceId] = $d.FriendlyName
  }
  return $h
}
function Recover-Phone {
  Say 'recover: returning the phone to Android'
  if ((& $fb devices 2>&1) -join ' ' -match 'fastboot') { & $fb reboot 2>&1 | Out-Null }
  if (Wait-Android 120) { Say 'recover: Android is back' } else { Say 'recover: WARNING Android did not come back; hold power to restart' }
}

Say ("attempt started; platform-tools $PlatformTools")
Say ("image: " + $(if ($Image) { "$Image  ($((Get-Item $Image).Length) bytes)" } else { '(none)' }))

try {
  if ($Recover) { Recover-Phone; Say 'done'; return }

  # ---- 1. into ABL fastboot
  $state = (& $adb get-state 2>&1) -join ' '
  Say ("adb state: " + $state)
  if ($state -match 'device') {
    & $adb reboot bootloader 2>&1 | Out-Null
  } elseif ($state -notmatch 'bootloader') {
    throw 'no adb and no fastboot device: plug the phone in and unlock it'
  }
  if (-not (Wait-Fastboot 40)) { throw 'fastboot never came up' }
  Say ("fastboot up: " + ((& $fb devices 2>&1) -join ' '))
  foreach ($v in 'product', 'unlocked', 'current-slot', 'secure', 'kernel') {
    Say ("getvar {0}: {1}" -f $v, (((& $fb getvar $v 2>&1) | Where-Object { $_ -notmatch '^Finished' }) -join ' '))
  }
  if ($NoBoot) {
    (& $fb getvar all 2>&1) | Set-Content (Join-Path $OutDir 'getvar-all.txt')
    Recover-Phone; Say 'done (NoBoot)'; return
  }

  # ---- 2. snapshot USB, then hand off
  $prev = Get-UsbDevices
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $bootLog = Join-Path $OutDir 'fastboot-boot.txt'
  $p = Start-Process -FilePath $fb -ArgumentList @('boot', ('"' + $Image + '"')) -NoNewWindow -PassThru `
                     -RedirectStandardOutput $bootLog -RedirectStandardError (Join-Path $OutDir 'fastboot-boot.err')
  Say ("handoff: fastboot boot $Image")

  # ---- 3. watch USB
  $xnuloader = $false
  $resetSeen = $false
  while ($sw.Elapsed.TotalSeconds -lt $WaitSec) {
    $cur = Get-UsbDevices
    foreach ($k in $cur.Keys) {
      if (-not $prev.ContainsKey($k)) {
        Say ('+{0,6:N1}s  APPEAR  {1}  {2}' -f $sw.Elapsed.TotalSeconds, $k, $cur[$k])
        if ($k -match 'VID_18D1&PID_4EE0' -or $cur[$k] -match 'xnuloader') { $xnuloader = $true }
      }
    }
    foreach ($k in $prev.Keys) {
      if (-not $cur.ContainsKey($k)) {
        Say ('+{0,6:N1}s  GONE    {1}' -f $sw.Elapsed.TotalSeconds, $k)
      }
    }
    $prev = $cur
    if ($cur.Values -match 'Android ADB Interface') {
      # Android's adbd: the attempt is over, either it failed or it came back
      $resetSeen = $true
      Say ('+{0,6:N1}s  Android adbd is up: the attempt is over' -f $sw.Elapsed.TotalSeconds)
      break
    }
    $d = (& $fb devices 2>&1) -join ' '
    if ($d -match 'xnuloader') { $xnuloader = $true; Say ('+{0,6:N1}s  loader fastboot visible: {2}' -f $sw.Elapsed.TotalSeconds, 0, $d); break }
    Start-Sleep -Milliseconds 500
  }
  $sw.Stop()

  # ---- 4. if the loader talked, take its log
  if ($xnuloader) {
    Say 'loader found: collecting its log'
    foreach ($cmd in @(@('oem', 'run'), @('oem', 'log'), @('getvar', 'result'))) {
      $name = 'loader-' + ($cmd -join '-') + '.txt'
      (& $fb $cmd 2>&1) | Set-Content (Join-Path $OutDir $name)
      Say ("  {0}: {1}" -f ($cmd -join ' '), (((Get-Content (Join-Path $OutDir $name)) -join ' | ').Substring(0, [Math]::Min(200, ((Get-Content (Join-Path $OutDir $name)) -join ' | ').Length))))
    }
    for ($i = 0; $i -lt 20; $i++) {
      if (-not ((& $fb devices 2>&1) -join ' ' -match 'xnuloader')) { break }
      & $fb get_staged (Join-Path $OutDir 'loader-staged.bin') 2>&1 | Out-Null
      Start-Sleep -Seconds 1
    }
  } else {
    Say 'loader never enumerated: no USB channel was reached by this image'
  }
  if (-not $resetSeen -and -not $xnuloader) { Say 'no USB change at all in the window' }

  # A serial device is the other shape the log channel can take (CDC ACM from the firmware or the loader):
  # record it even when adb/fastboot show nothing.
  $ports = @(Get-CimInstance Win32_PnPEntity -ErrorAction SilentlyContinue |
             Where-Object { $_.Name -match '\(COM\d+\)' } | Select-Object -ExpandProperty Name)
  Say ('serial ports: ' + $(if ($ports.Count) { $ports -join ' | ' } else { '(none)' }))
  ($ports | Out-String) | Set-Content (Join-Path $OutDir 'serial-ports.txt')
}
finally {
  $p | ForEach-Object { try { if (-not $_.HasExited) { $_.Kill() } } catch {} }
  if (-not $Recover) { Recover-Phone }   # the -Recover path already did it, once
  Say ("attempt directory: $OutDir")
}
