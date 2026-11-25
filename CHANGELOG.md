# Changelog

Each release's section becomes the text of its GitHub release (`tools/release_notes.py`, run by
`.github/workflows/build.yml` when a `v*` tag is pushed). Add the section before tagging: a tag
without one does not publish. Keep each entry on one line: GitHub shows a line break in release
text as a break. Sections from v0.15 down are from the Resogun runner this port is built on.

## v0.1.5

- Fixed Host a game in v0.1.4: the release left out the online server that v0.1.3 shipped, so hosting stopped with an error. Joining was not affected

## v0.1.4

- Fixed Host a game saying "Set up the game files first" on games set up from the launcher (issue #1): setup now writes the file the online server needs, and a game already set up gets it on its next start, in about a second
- Japan's tie-in outfits that no add-on grants (Final Fantasy 30th anniversary with the Chocobo cart, Level-5, Atelier, Famitsu): 55 parts, unlocked from the start by default. On the Saves tab, Earn in game sells them in the shop's coin tab for 1500 to 2500 coins each, and Original keeps them locked as on a PS4 today
- Fewer stutters on the first visit to a course: shader variants that compile to the same code now share one pipeline, so about a third fewer get built and hitches over a second dropped from about 30 to about 12 in testing. Your shader cache rebuilds those once after updating
- Screen fades draw again (title, loading and moving between holes); the v0.1.2 fix for the inverted-color flash had hidden them
- New Display row, Miss-shot flash: the game flashes the screen in inverted colors when you miss a swing's timing. Off (default) hides it; On shows it as the PS4 draws it

## v0.1.3

- Easy online play: Host a game on the Online tab, share the invite code, and a friend joins with it; games on your local network show up in a list, and the router port opens automatically. Tested with two copies on one PC; play between two homes is not tested yet, so please report how it goes
- Trophies: all 31 trophies have their real names, grades and descriptions, a popup shows when one unlocks, and a new Trophies tab lists them per save
- 60 FPS mode now runs through one small hook on the game's flip-rate call and keeps game speed normal
- Shader fixes rewritten (shadow lines, face lighting), with resource limits set from what the game uses

## v0.1.2

- Fixed a rare crash while a course loads: the GPU could read a command buffer after the game had started refilling it
- Fixed a crash on texture formats the renderer can't convert; it now logs the texture and keeps going
- Fixed the whole screen flashing to inverted colors for about half a second when the club hits the ball
- First visits to a course load faster: the GPU thread no longer waits on the renderer for every load-time sync
- Smoother frame pacing: the runtime asks Windows for a 1 ms timer, and thread waits skip a needless unlock and relock (held back from v0.1.1 until the load crash above was found)

## v0.1.1

- Updates now clean up after themselves: a refused, failed, canceled or finished update leaves no empty staging folder or half-downloaded file in `data\update`, and your saves, settings, logs and prepared game are never touched
- The update check ignores draft and pre-release versions and development builds, and stays silent when GitHub is unreachable or limiting requests, so it never gets in the way of Play
- Game files: a dump folder without sce_module is caught before setup starts, the advice for a missing or unreadable file now names the file, and folder paths pasted with quotes or spaces around them work
- The 60 FPS mode row in Display Settings says it needs PS4 Pro mode and a restart

## v0.1.0

- First public Windows release of the Everybody's Golf (PS4) PC port. You need your own decrypted dump of CUSA04687 with update 1.17; nothing from the game is included
- Unpack the zip and start EverybodysGolf.exe: the first run opens Game files, where you pick your dump and press Set up game files
- A full 9-hole round plays: swing, ball flight, lies, putting, scorecard and the move to the next hole
- 60 FPS mode in every scene, and render resolution up to 3840 x 2160, both in PS4 Pro mode (the default)
- Launcher tabs for every option the game reads: Display, Graphics, Sound & controls, Saves, HD textures, Online and Game files
- The launcher checks GitHub for a newer release at most once a day, checks the download's SHA-256 before it installs it, keeps your data folder, and puts the previous version back if an update fails. Turn the check off on the Play tab
- Known limits: the first visit to each course compiles shaders for about a minute; some character faces show light graphical faults; online play needs a title server you or a friend run from the Online tab, and each round is still played alone

## v0.15

- Cheats added by popular demand. Press L3+R3 to enter the cheats menu

## v0.14

- Game files other than the tested version are allowed again: setup shows a one-time warning that they are untested and might cause bugs
- More fixes for missing effects

## v0.13

- Fixed missing effects, like the big flash and light beams when your ship explodes after losing a life

## v0.12

- No need to run the setup again after updating: the game prepares itself on its first start
- Starting the game before it's set up (or after its folders moved) opens the setup
- To make running the game more consistent, the tested version of the game (in README) is now required, and the setup refuses game files other than it
- Local co-op: added support for a second connected controller

## v0.11

- A DualShock 4 or DualSense connected by USB now plays the game's controller sounds on its own speaker, like on a PS4 or PS5.
  - Only tested on a Dualsense. I don't have a DS4 atm, but in theory it works the same way. If it doesn't, please let me know.
- Select+Start on a controller now asks to close the game, like F4, and holding Start opens the display settings, like holding Select.
- Fixed borderless fullscreen mode, no longer behaves like exclusive fullscreen.

## v0.10

- Fix compiled shaders not being reused on Linux (issue #8)

## v0.9

- The game now frees video memory sooner when other programs (a browser, Discord, a recorder) take some, which may prevent the crashes after a few levels on 8 GB graphics cards (issue #6).

## v0.8

- Support for 16:10 resolutions. Steam Deck 🖤

## v0.7

- Fixed a crash when a level loads after a long session: the renderer could copy stale GPU data back over memory the game had just written (here a shader's header), so the game read a bad address (issue #1). This includes writes that left some bytes as they were, such as the zero high bytes of an address.
- When the graphics driver resets the GPU after a hang, the game now stops with a message in the log instead of freezing until it is killed.
- The log no longer prints the game's audio levels every 3 seconds; the per-port summary at exit remains.

## v0.6

- Setup checks your eboot.bin against the unmodified update 1.10 this release was tested with. For a modified one (a DLC unlocker patch, for example) or another version, it warns that the game may be unstable or crash and asks before going on. All add-ons already show as owned without an unlocker.
- Dumps whose eboot.bin was patched for a DLC unlocker (it imports from a `dlcldr` module) now start, instead of stopping at boot with exit code 20.

## v0.5

- Shaders that interpolate a flat-shaded input are handled as on the PS4's GPU instead of stopping the game (issue #1: a stop with exit code 23 when level 3 loaded after the level 2 boss). The game data corruption behind that crash is still being investigated.

## v0.4

- Sharpening: AMD FSR 1 RCAS over the picture, for crisper edges without halos. On by default; a new Sharpening row in Display Settings (F10), and **F9** switches it on or off during play to compare. The game's own HUD and menus are sharpened along with the scene.
- 144 FPS frame rate, between 120 and 180. A 144 or 165 Hz display now runs at 144 instead of 120.

## v0.3

- Aspect ratios: 21:9 and 32:9 show more of the level to the sides, 4:3 more above and below, nothing stretched. The menus stay 16:9.
- 30 FPS cap for PCs that cannot hold 60, at normal game speed.

## v0.2

- HDR setting: the game's own HDR10 output on an HDR display.
- 90 FPS frame rate (smooth on a 120 Hz display with VRR).
- Faster rendering where the game writes next to data the GPU has written.

## v0.1

- First release: Resogun (PS4, CUSA00038 v1.10) running natively on Windows and Linux.
