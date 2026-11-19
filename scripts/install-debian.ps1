# SPDX-License-Identifier: GPL-2.0-or-later
# Install Debian + Plasma Mobile into the Surface Duo 1's userdata partition, from Windows 11.
# The same steps as scripts/install-debian.sh (Linux). UNTESTED on Windows so far.
#
# What it touches on the phone: ONLY the userdata partition (formatted as ext4, Debian goes to /debian on it).
# It never writes boot_*, vbmeta_*, dtbo_*, the partition table or the lock state, and it never locks the bootloader.
#
# usage (PowerShell, in the unpacked repository folder):
#   powershell -ExecutionPolicy Bypass -File scripts\install-debian.ps1 duo-debian.img duo-debian-rootfs.tar.zst duo-modules-<version>.tar.zst
# needs: fastboot (Android SDK Platform-Tools) on PATH or next to this script, Python 3, Windows' own OpenSSH client
# (ssh, ssh-keygen: Settings > System > Optional features). Windows 11: its built-in USB NCM driver gives the
# network link to the phone. The phone with an UNLOCKED bootloader, in the bootloader menu, connected over USB-C.
param(
  [Parameter(Mandatory = $true)][string]$Img,
  [Parameter(Mandatory = $true)][string]$Rootfs,
  [Parameter(Mandatory = $true)][string]$Modules
)
$ErrorActionPreference = 'Stop'
function Fail($m) { Write-Host "ERROR: $m" -ForegroundColor Red; exit 1 }

$H = Split-Path -Parent $PSScriptRoot
foreach ($f in @($Img, $Rootfs, $Modules)) { if (-not (Test-Path -LiteralPath $f)) { Fail "missing file: $f" } }
$Img = (Resolve-Path -LiteralPath $Img).Path
$Rootfs = (Resolve-Path -LiteralPath $Rootfs).Path
$Modules = (Resolve-Path -LiteralPath $Modules).Path

$env:PATH = "$PSScriptRoot;$H;$env:PATH"
foreach ($t in @('fastboot', 'ssh', 'ssh-keygen')) {
  if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { Fail "missing tool: $t" }
}
$Py = $null
foreach ($p in @('py', 'python', 'python3')) {
  $c = Get-Command $p -ErrorAction SilentlyContinue
  if ($c -and $c.Source -notmatch 'WindowsApps') { $Py = $p; break }
}
if (-not $Py) { Fail "missing Python 3 (https://www.python.org/downloads/)" }

# SSH key and a boot image that carries it
$Key = if ($env:DUO_KEY) { $env:DUO_KEY } else { Join-Path $HOME '.ssh\duo_ed25519' }
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Key) | Out-Null
if (-not (Test-Path -LiteralPath $Key)) {
  cmd /c "ssh-keygen -q -t ed25519 -N `"`" -C duo-install -f `"$Key`""
  if ($LASTEXITCODE -ne 0) { Fail "ssh-keygen failed" }
}
$KImg = Join-Path (Split-Path -Parent $Img) 'duo-debian-key.img'
& $Py -I (Join-Path $H 'tools\linux\abl\add-ssh-key.py') $Img "$Key.pub" $KImg | Out-Null
if ($LASTEXITCODE -ne 0) { Fail "add-ssh-key.py failed" }
Write-Host "boot image with your SSH key: $KImg"

$SshOpts = "-i `"$Key`" -o StrictHostKeyChecking=no -o UserKnownHostsFile=NUL -o LogLevel=ERROR -o ConnectTimeout=5 -o ServerAliveInterval=10"
$script:Phone = 'root@169.254.66.1'
# every ssh call goes through cmd.exe: its < redirection hands binary files to ssh unchanged (PowerShell pipes do not)
function PhoneRun([string]$cmd, [string]$stdinFile = '') {
  $c = "ssh $SshOpts $script:Phone `"$cmd`""
  if ($stdinFile) { $c += " < `"$stdinFile`"" }
  $out = cmd /c "$c 2>NUL"
  $script:Rc = $LASTEXITCODE
  return $out
}

Write-Host "1/4 starting Linux from RAM (nothing is written to the phone by this step)"
# fastboot through cmd.exe too: PowerShell 5.1 turns a native tool's stderr into errors
function Fb([string]$a) { $o = cmd /c "fastboot $a 2>&1"; $script:Rc = $LASTEXITCODE; return ($o | Out-String) }
$dev = (Fb 'devices').Trim()
if (-not $dev) { Fail "no phone in fastboot. Bootloader menu = from off, hold volume-down + power. If the phone is in that menu, Windows needs a fastboot driver (Google USB Driver, 'Android Bootloader Interface' in Device Manager)." }
$unl = Fb 'getvar unlocked'
if ($unl -notmatch 'unlocked:\s*yes') { Fail "the bootloader is not unlocked (fastboot flashing unlock first)" }
Fb '--set-active=a' | Out-Host
Fb "boot `"$KImg`"" | Out-Host
if ($script:Rc -ne 0) { Fail "fastboot boot failed (if the bootloader refuses the image, rebuild it with Microsoft's boot.img as the reference, see README)" }

Write-Host "2/4 waiting for the phone on USB networking (169.254.66.1 or 10.66.0.1, up to 4 minutes)"
$ok = $false
for ($i = 0; $i -lt 40 -and -not $ok; $i++) {
  foreach ($a in @('169.254.66.1', '10.66.0.1')) {
    $script:Phone = "root@$a"; PhoneRun 'true' | Out-Null
    if ($script:Rc -eq 0) { $ok = $true; break }
  }
  if (-not $ok) { Start-Sleep -Seconds 3 }
}
if (-not $ok) { Fail "no SSH answer from the phone. In Device Manager the phone should show up as a network adapter (UsbNcm); 'ipconfig' should list a 169.254.x.x or 10.66.0.2 address on it." }
Write-Host "   phone at $script:Phone"

$P = (PhoneRun 'for d in /sys/class/block/sd*; do grep -q ^PARTNAME=userdata$ $d/uevent && echo ${d##*/}; done | head -1' | Out-String).Trim()
if (-not $P) { Fail "userdata partition not found on the phone" }
$Sz = (PhoneRun "echo `$((`$(cat /sys/class/block/$P/size) / 2097152))" | Out-String).Trim()
Write-Host ""
Write-Host "3/4 userdata = /dev/$P ($Sz GB). It will be FORMATTED (ext4) - everything on it is lost." -ForegroundColor Yellow
Write-Host "    (Android's data is already gone after the bootloader unlock; Android itself stays on its own partitions.)"
$ans = Read-Host "    Type YES to continue"
if ($ans -cne 'YES') { Fail "stopped, nothing written" }
PhoneRun "set -e; umount /dev/$P 2>/dev/null || true; mkfs.ext4 -q -F -L duo-userdata /dev/$P; mkdir -p /mnt/u; mount /dev/$P /mnt/u; mkdir -p /mnt/u/debian" | Out-Host
if ($script:Rc -ne 0) { Fail "formatting userdata failed" }

Write-Host "4/4 copying Debian (this takes a while; the phone unpacks it)"
PhoneRun 'zstd -dc | tar -x -C /mnt/u/debian' $Rootfs | Out-Host
if ($script:Rc -ne 0) { Fail "copying the root file system failed" }
PhoneRun 'zstd -dc | tar -x -C /mnt/u/debian' $Modules | Out-Host
if ($script:Rc -ne 0) { Fail "copying the kernel modules failed" }
PhoneRun 'set -e; chmod 1777 /mnt/u/debian/tmp; sync; umount /mnt/u; echo copied' | Out-Host
if ($script:Rc -ne 0) { Fail "finishing the copy failed" }

Write-Host ""
Write-Host "Done. Start Debian now and after every power-off (the phone in the bootloader menu, USB to this PC):" -ForegroundColor Green
Write-Host ""
Write-Host "    fastboot --set-active=a"
Write-Host "    fastboot boot `"$KImg`""
Write-Host ""
Write-Host "Plasma Mobile starts on both screens. SSH: ssh -i `"$Key`" root@169.254.66.1"
PhoneRun 'devmem 0x0c264000 w 0' | Out-Null   # reset (PS_HOLD): `reboot` hangs this phone
