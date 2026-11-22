@echo off
rem Hosts an online game for friends without playing: the title server, the router port and the
rem invite codes, as the launcher's Host a game button does. Close this window to stop.
cd /d "%~dp0"
"..\python\python.exe" -I host.py --online --auto %*
pause
