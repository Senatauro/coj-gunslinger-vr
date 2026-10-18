# Call of Juarez: Gunslinger VR

An unofficial VR mod for *Call of Juarez: Gunslinger* (Steam): stereo 3D at your headset's refresh rate, 6DoF head
and hands, guns in your hands, dynamite thrown with your arm and duels drawn from the holster.

**Version 0.1 is an early test version.** The start of the campaign, the Arcade and the Duel mode are playable, but not
everything works yet: read [Known issues](#known-issues) before playing.

Not affiliated with Techland or Ubisoft. No game files are included or changed.

## What the mod does today

- **Stereo 3D and 6DoF:** the game renders each eye; your head moves the camera 1:1, seated or standing; crouch for
  real; lean around corners.
- **Movement:** smooth locomotion by head or hand direction, or teleport; snap or smooth turning; comfort vignette.
- **Guns in your hands:** bullets leave the barrel, recoil, aim dot, two-handed aiming for rifles and shotguns, dual
  revolvers (akimbo), fingers that follow the controller, a hand pose adjusted for each weapon.
- **Weapon menu:** hold Y, point and release, using the game's own weapon wheel.
- **Dynamite:** throw it with your arm or along an arc with the trigger; the fuse burns in your hand.
- **Concentration and Sense of Death** in VR: mark targets with your head, dodge by leaning.
- **World:** open doors and pick items by pointing your free hand and squeezing the grip; ladders with one button.
- **Interface:** the game's HUD in front of you, ammo on your wrist, a 3D pause menu, cutscenes and menus on a cinema
  screen.
- **Duels in VR:** draw from the holster with the grip, focus by catching signs on the enemy, dodge for real.
- **Arcade and Duel mode** work too.
- **In-game hints** describe the VR gesture instead of the gamepad button (most of them).
- **22 VR options** in Options > Game; controls can be remapped in SteamVR's controller bindings.
- An **"Update available"** button in the main menu when a new version is out, and **"Report a bug / send feedback"**
  in the pause menu.

## What is still missing

- The rest of the campaign's scripted moments in VR (some hints, throwing an enemy's dynamite back, forced camera
  moments).
- Mexican standoff in duels; a more arcade-like duel focus.
- QTEs replaced by real hand actions in VR.
- Gatling guns and fixed cannons in VR.
- Controller vibration.
- Height calibration and character size.
- Pointing at menus with the controller.
- Options: virtual holsters instead of the weapon menu, manual reload, arms (IK), floating guns without hands.
- Bindings for controllers other than the Oculus Touch.
- Automatic updates and a VR-only leaderboard.

## Requirements

- *Call of Juarez: Gunslinger* from Steam (the current Steam version, 1.0.5.0).
- SteamVR and a PC VR headset. Tested with a Quest 3 via Steam Link, seated and standing.
- Linux: Proton Experimental (Steam library > Tools). Start the normal game once with Proton Experimental before
  installing.
- Windows: not tested yet. Reports are welcome.

## Install

The installer makes a copy of the game folder called "CoJ Gunslinger VR" next to the original (with hard links, so it
uses almost no extra disk), puts the mod in the copy and adds **Call of Juarez: Gunslinger VR** to your Steam library.
The normal game is never modified. Your progress is shared, because saves are in the Steam Cloud.

1. Download the zip from the latest release and extract it anywhere.
2. Run the installer:
   - **Windows:** double-click `cojvr-setup.exe` (Windows may warn that the file is not signed).
   - **Linux:** open a terminal in the extracted folder and run `./install-linux.sh`
3. Close Steam when the installer asks, then open Steam again.

To play: start SteamVR first, then **Call of Juarez: Gunslinger VR** from your Steam library.

**Update:** run the installer of the new version. Your VR options are kept.

**Uninstall:** `cojvr-setup.exe --uninstall` (Windows) or `./install-linux.sh --uninstall` (Linux). This removes the
copy and the Steam entry.

### Manual install (without the installer)

This puts the mod in the game folder itself, so the normal game also starts with the mod: in VR when SteamVR is
running, flat when it is not.

1. In Steam, right-click *Call of Juarez: Gunslinger* > Manage > Browse local files.
2. Copy `d3d9.dll` and the `cojvr` folder from the zip into that folder.
3. In the game's Properties > General, turn off "Enable the Steam Overlay while in-game" (see
   [Troubleshooting](#troubleshooting)).
4. Linux only: in the game's Properties, set Compatibility to Proton Experimental and the Launch options to
   `PROTON_USE_WOW64=1 %command%`.
5. Start SteamVR, then the game from Steam.

To remove it, delete `d3d9.dll` and the `cojvr` folder from the game folder.

## Controls (Oculus Touch)

| Control | Action |
|---|---|
| Left stick | Move (click: sprint) |
| Right stick | Turn; up: jump; down: crouch |
| Triggers | Fire the gun in that hand |
| A | Melee (a quick punch with your hand also works) |
| B | Concentration |
| X | Reload / use |
| Y | Weapon menu: hold, point with your left hand, release |
| Grip | Hold the gun with both hands; point your free hand at a door or item and squeeze to use it; throw dynamite with your arm |
| Left stick click | Running |
| Right stick click | Objective; hold: skills; right after throwing dynamite: split the dynamite |
| Left menu button | Pause |

With your body: crouch for real, lean to dodge in Sense of Death and in duels, draw from the holster in duels.
In menus and on the cinema screen, A confirms and B goes back.

## VR options

Options > Game: the lines starting with "VR:" (seated or standing, dominant hand, turning, teleport, comfort vignette,
HUD, aim dot, duels and more). Choices are saved in `cojvr/cojvr.ini` in the VR copy.

## Known issues

- Sometimes the right eye gets out of sync with the left. At the very start of the campaign it shakes until you pick
  up the first gun. This is kinda rare and I couldn't trace it yet. If you see this issue, please open an github issue with the description and what you did to make it happen
- Controller vibration does not work yet.
- Gatling guns and fixed cannons have no VR version yet (not tested).
- QTEs still ask for gamepad buttons (in VR, A is melee and Y does not reach the game).
- Throwing an enemy's dynamite back asks for LB, which the Touch does not have.
- Some hints still mention gamepad buttons.
- A cutscene that starts while you are physically turned away can appear behind you.
- Duels: no Mexican standoff in VR yet; focus by "signs" is temporary.
- No height calibration and only one character size for now.
- Only Oculus Touch bindings are included.
- Arcade and Duel scores made in VR are not uploaded to the online leaderboards (on purpose).

## Troubleshooting

**The game opens flat, on the monitor only.** SteamVR must be running before the game starts. If the headset went to
sleep, SteamVR may have closed by itself: start it again, then the game. On Linux, the game also needs
`PROTON_USE_WOW64=1` (the installer's launcher sets it).

**The game freezes at the end of a loading screen, or the screen stays black after "press to start".** This happens
with the Steam Overlay on. The installer's Steam entry already starts the game without it; with a manual install, turn
the overlay off in the game's Properties > General.

**Steam closes, or the game closes, when I press Play with SteamVR running (Linux).** That is Steam's "Desktop Game
Theatre" for flat games. Start the game from the **Call of Juarez: Gunslinger VR** entry that the installer adds: it is
marked as a VR game.

**The installer says Steam is running.** Close Steam from its menu (Steam > Exit), wait a few seconds and press Enter.
Steam rewrites its list of games when it closes, so the new entry can only be added with Steam closed.

**The Linux installer says Proton Experimental or the game's prefix is missing.** Install "Proton Experimental" from
the Steam library (Tools), set it in the game's Properties > Compatibility, start the normal game once and quit, then
run the installer again.

**The mod does nothing and the game is flat even with SteamVR running.** The mod only works with the Steam version
1.0.5.0 of the game; with another version it turns itself off and the game runs as usual. The first lines of
`cojvr/cojvr.log` say which version was found.

**The controllers stop responding or the guns disappear after restarting SteamVR.** Restart the game after SteamVR, so
both start in the right order.

**Windows says the installer is not signed.** The installer is not signed with a certificate. Choose "More info" >
"Run anyway", or do the manual install.

## Reporting a bug or giving feedback

In the game: pause menu > "Report a bug / send feedback (opens on the PC)". It opens this project's issue page in your
computer's browser. (Options > Game > "VR: report a bug / send feedback" does the same.) Please attach the log:
`cojvr/cojvr.log` inside the "CoJ Gunslinger VR" folder of your Steam library
(`steamapps/common/CoJ Gunslinger VR`).

## License

The mod is MIT licensed (`LICENSE`). It ships DXVK (zlib license) and the OpenVR API (BSD 3-Clause); see `licenses/`.

## Building from source

Linux with MinGW (`i686-w64-mingw32-g++`) and `zip`: `make` builds `build/d3d9.dll`; `make dist` builds the release zip
(set `DXVK_DLL` to DXVK 3.1.1's `x32/d3d9.dll` and `OPENVR_DLL` to OpenVR 2.15.6's `bin/win32/openvr_api.dll`).
`python3 tools/test_install_linux.py` checks the Linux installer against a fake Steam folder.
