# Test-only. Boots the game hidden and muted and replays a RESOGUN_PAD_REPLAY recording (flip-keyed). While it plays,
# it probes the screen with frame dumps: at each "S <flip> <name>" resync point of the recording the replay waits, and
# this script releases it (a file in $out\sync) once the pixel signature of <name> shows. After the replay ends it
# presses Cross until the tee view shows, then dumps the final frame. Hold $TEMP/claude/golf-heavy.lock while it runs.
# Usage: replay-to-tee.ps1 -Pad hole10.pad -Label r1 [-Timeout 560] [-Cap 40]
# Output: $TEMP/claude/pad-replay/<Label>/{boot.log,probe.log,final.raw}; the last line is the result.
param([Parameter(Mandatory)][string]$Pad,[Parameter(Mandatory)][string]$Label,[int]$Timeout=560,[int]$Cap=40)
# Signatures "X.Y.RRGGBB.TOL" in the 1920x1080 BGRA frame dump; the names are the ones the recording's S lines use.
$sigs=@{ hub='1500.40.D5C4AF.14'; select='1500.222.CC3121.25'; list='990.140.4288D0.12'; tee='60.50.D41018.20'; cup='735.369.373331.20|870.428.373331.20' }  # tee + cup (the Set Up Tee Shot glyph) together = the tee view with its menu
$root=(Resolve-Path "$PSScriptRoot\..\..").Path; $game='A:\Everybodys Golf PS4'
$out="$env:TEMP\claude\pad-replay\$Label"; Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$out\dump","$out\sync" | Out-Null
$state='A:\golf-wt\pad-replay-state'; $save="$state\save"; $S="$save\savedata\1\CUSA04687"
foreach ($d in "$S\SAVEDATA","$S\SAVEDATA.sce_backup\data") { New-Item -ItemType Directory -Force $d | Out-Null; Copy-Item "$game\work\saves\after-tutorial\DATA*.BIN" "$d\" -Force }
$env:MSYS2_ROOT="$game\tools\msys2\msys64"; $env:EG_EXE="$root\out\everybodys-golf.exe"
$env:RESOGUN_DISPLAY_MODE='windowed'; $env:RESOGUN_AUDIO='none'
$env:RESOGUN_FRAME_DUMP_TRIGGER="$out\dump\trigger"; $env:RESOGUN_DUMP_DIR=($out.Replace('\','/')) + '/dump'
$env:RESOGUN_PAD_REPLAY=$Pad; $env:RESOGUN_PAD_SYNC="$out\sync"; $env:RESOGUN_PAD_FILE="$out\pad.txt"; Set-Content "$out\pad.txt" ''
$env:GOLF_NP='offline'; $env:GHOST_MEM_GB='32'; $env:RESOGUN_IME_TEXT='Golfer'; $env:RESOGUN_FRAME_STATS='1'
$a=@("$env:USERPROFILE/.claude/skills/ghost-godot/ghost_launch.py",'python',"$root/run.py",'play','--game','work/game-1.17','--out','work/game-1.17-prepared','--any-build','--timeout',"$Timeout",'--save',$save)
$p=Start-Process python -ArgumentList $a -WorkingDirectory $game -NoNewWindow -PassThru -RedirectStandardOutput "$out\boot.log" -RedirectStandardError "$out\boot.err"
$null=$p.Handle; $p.PriorityClass='BelowNormal'; try { $p.ProcessorAffinity=[IntPtr]0x00FF0000 } catch {}
function Match($specs,$buf) { foreach ($spec in $specs.Split('|')) { if (MatchOne $spec $buf) { return $true } }; return $false }
function MatchOne($spec,$buf) { $a=$spec.Split('.'); $x=[int]$a[0]; $y=[int]$a[1]; $h=$a[2]; $tol=[int]$a[3]
  $o=($y*1920+$x)*4; $e=@([Convert]::ToInt32($h.Substring(0,2),16),[Convert]::ToInt32($h.Substring(2,2),16),[Convert]::ToInt32($h.Substring(4,2),16))
  return ([Math]::Max([Math]::Abs($buf[$o+2]-$e[0]),[Math]::Max([Math]::Abs($buf[$o+1]-$e[1]),[Math]::Abs($buf[$o]-$e[2]))) -le $tol) }
# One frame dump: returns the newest display_*.raw as a byte array (older dumps are deleted).
function ReadShared($path) { for ($k=0; $k -lt 10; $k++) { try { return (ReadOnce $path) } catch { Start-Sleep -Milliseconds 500 } }; return $null }
function ReadOnce($path) { $fs=[IO.File]::Open($path,'Open','Read','ReadWrite,Delete'); try { $ms=New-Object IO.MemoryStream; $fs.CopyTo($ms); return $ms.ToArray() } finally { $fs.Close() } }
function Shot() { Set-Content "$out\dump\trigger" '1'; Start-Sleep -Milliseconds 1300
  $fs=Get-ChildItem "$out\dump\display_*.raw" | Sort-Object Name; if (-not $fs) { return $null }
  $f=$fs[-1]; $b=(ReadShared $f.FullName); if (-not $b) { return $null }; $script:lastRaw=$f.FullName
  foreach ($o in $fs | Select-Object -SkipLast 1) { Remove-Item $o.FullName -ErrorAction SilentlyContinue }; return $b }
# The runtime reads pad.txt while it is written: open it shared, retry on a clash.
function Pad($v) { $bytes=[Text.Encoding]::ASCII.GetBytes("$v`n"); for ($k=0; $k -lt 100; $k++) { try { $f=[IO.File]::Open("$out\pad.txt",'OpenOrCreate','Write','ReadWrite,Delete'); $f.SetLength(0); $f.Write($bytes,0,$bytes.Length); $f.Close(); return } catch { Start-Sleep -Milliseconds 10 } } }
function Press() { Pad 'cross'; Start-Sleep -Milliseconds 250; Pad ''; Start-Sleep -Milliseconds 1500 }
function LastLog($pat) { $l=Select-String -Path "$out\boot.log" -Pattern $pat | Select-Object -Last 1; if ($l) { return $l.Line } else { return '' } }
$sw=[Diagnostics.Stopwatch]::StartNew(); $res='TIMEOUT'; $finished=$false; $tail=0; $settle=0; $nocup=0; $pulse=@('hub')
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $Timeout-20) {
  if (-not (Test-Path "$out\boot.log") -or -not (Select-String -Path "$out\boot.log" -SimpleMatch 'pad replay of' -Quiet)) { Start-Sleep -Seconds 2; continue }
  $buf=Shot; if (-not $buf) { continue }
  $hit=($sigs.Keys | Where-Object { Match $sigs[$_] $buf }) -join ','
  $flip=LastLog 'pad replay at flip'; $wait=LastLog 'pad replay waiting for'; $sy=LastLog 'pad replay synced'
  Add-Content "$out\probe.log" ("{0}s {1} | {2} | {3} | sigs: {4}" -f [int]$sw.Elapsed.TotalSeconds,$flip.Replace('Runtime: pad replay ',''),$wait.Replace('Runtime: pad replay ',''),$sy.Replace('Runtime: pad replay ',''),$hit)
  $m=[regex]::Match($wait,'waiting for (\S+) at'); if ($m.Success -and -not (Test-Path "$out\sync\$($m.Groups[1].Value)") -and $sigs[$m.Groups[1].Value] -and (Match $sigs[$m.Groups[1].Value] $buf)) { Set-Content "$out\sync\$($m.Groups[1].Value)" '1'; Add-Content "$out\probe.log" "  released $($m.Groups[1].Value)" }
  if (-not $finished -and (Select-String -Path "$out\boot.log" -SimpleMatch 'pad replay finished' -Quiet)) { $finished=$true; Add-Content "$out\probe.log" '  replay finished; pressing Cross until the tee view' }
  # While the replay waits for a pulse screen (the title and loading screens before the hub), press Cross every probe like the warm phase did.
  if ($m.Success -and $pulse -contains $m.Groups[1].Value -and -not (Test-Path "$out\sync\$($m.Groups[1].Value)")) { Press }
  if ($finished) {
    if (($hit -split ',' -contains 'tee') -and ($hit -split ',' -contains 'cup')) { $settle++; $nocup=0; if ($settle -ge 2) { Copy-Item $script:lastRaw "$out\final.raw"; $res="TEE after $tail presses"; break } }
    else { $settle=0; if ($hit -split ',' -contains 'tee') { $nocup++ } else { $nocup=0 }  # no Cross while the hole still loads: it would queue up and skip the menu
      # A menu fading in does not match yet: press only after two probes in a row without the view, and never at the cap.
      if ($nocup -ge 2) { if ($tail -ge $Cap) { Copy-Item $script:lastRaw "$out\final.raw"; $res="NO TEE after $tail presses"; break }
        $nocup=0; $tail++; Press } }
  }
}
if ($res -eq 'TIMEOUT' -and $script:lastRaw) { Copy-Item $script:lastRaw "$out\final.raw" }
if (-not $p.HasExited) { & taskkill /PID $p.Id /T /F | Out-Null }
"$Label $res after $([int]$sw.Elapsed.TotalSeconds) s"
