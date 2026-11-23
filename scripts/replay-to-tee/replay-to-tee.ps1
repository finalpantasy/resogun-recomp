# Test-only. Boots the game hidden and muted, replays a RESOGUN_PAD_REPLAY recording (flip-keyed) and
# dumps the frame a few seconds after the replay ends. Hold $TEMP/claude/golf-heavy.lock while it runs.
# Usage: replay-to-tee.ps1 -Pad hole10.pad -Label r1 [-Timeout 560] [-Settle 8] [-Record file.pad]
# -Record also records what the guest receives (to check the replay delivers the same states).
# Output: $TEMP/claude/pad-replay/<Label>/{boot.log,dump/display_*.raw}; the last line is the result.
param([Parameter(Mandatory)][string]$Pad,[Parameter(Mandatory)][string]$Label,[int]$Timeout=560,[int]$Settle=8,[string]$Record='')
$root=(Resolve-Path "$PSScriptRoot\..\..").Path
$port=$root; $game='A:\Everybodys Golf PS4'
$out="$env:TEMP\claude\pad-replay\$Label"; Remove-Item -Recurse -Force $out -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$out\dump" | Out-Null
# Scratch state: a fresh copy of the after-tutorial save, never out/save.
$state='A:\golf-wt\pad-replay-state'; $save="$state\save"
$S="$save\savedata\1\CUSA04687"
if (-not (Test-Path "$S\SAVEDATA")) { Copy-Item -Recurse "$env:TEMP\claude\save-profile\*" $save -Force -ErrorAction SilentlyContinue; New-Item -ItemType Directory -Force "$S\SAVEDATA","$S\SAVEDATA.sce_backup\data" | Out-Null }
Copy-Item "$game\work\saves\after-tutorial\DATA*.BIN" "$S\SAVEDATA\" -Force
New-Item -ItemType Directory -Force "$S\SAVEDATA.sce_backup\data" | Out-Null
Copy-Item "$game\work\saves\after-tutorial\DATA*.BIN" "$S\SAVEDATA.sce_backup\data\" -Force
$env:MSYS2_ROOT="$game\tools\msys2\msys64"
$env:EG_EXE="$port\out\everybodys-golf.exe"
$env:RESOGUN_DISPLAY_MODE='windowed'; $env:RESOGUN_AUDIO='none'
$env:RESOGUN_FRAME_DUMP_TRIGGER="$out\dump\trigger"; $env:RESOGUN_DUMP_DIR=($out.Replace('\','/')) + '/dump'
$env:RESOGUN_PAD_REPLAY=$Pad; if ($Record) { $env:RESOGUN_PAD_RECORD=$Record }
$env:GOLF_NP='offline'; $env:GHOST_MEM_GB='32'; $env:RESOGUN_IME_TEXT='Golfer'; $env:RESOGUN_FRAME_STATS='1'
$a=@("$env:USERPROFILE/.claude/skills/ghost-godot/ghost_launch.py",'python',"$port/run.py",'play','--game','work/game-1.17','--out','work/game-1.17-prepared','--any-build','--timeout',"$Timeout",'--save',$save)
$p=Start-Process python -ArgumentList $a -WorkingDirectory $game -NoNewWindow -PassThru -RedirectStandardOutput "$out\boot.log" -RedirectStandardError "$out\boot.err"
$null=$p.Handle; $p.PriorityClass='BelowNormal'; try { $p.ProcessorAffinity=[IntPtr]0x00FF0000 } catch {}
$sw=[Diagnostics.Stopwatch]::StartNew(); $res='TIMEOUT'
while (-not $p.HasExited -and $sw.Elapsed.TotalSeconds -lt $Timeout-20) {
  if (Select-String -Path "$out\boot.log" -SimpleMatch 'pad replay finished' -Quiet) {
    Start-Sleep -Seconds $Settle; Set-Content "$out\dump\trigger" '1'; Start-Sleep -Seconds 3; $res='DUMPED'; break }
  Start-Sleep -Seconds 2
}
if (-not $p.HasExited) { & taskkill /PID $p.Id /T /F | Out-Null }
"$Label $res after $([int]$sw.Elapsed.TotalSeconds) s; $(Select-String -Path "$out\boot.log" -Pattern 'pad replay' | ForEach-Object { $_.Line } | Select-Object -First 3)"
