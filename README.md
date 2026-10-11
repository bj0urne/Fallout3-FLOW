# FLOW (Gameplay Overhaul)

A [FOSE](https://fose.silverlock.org/) plugin for **Fallout 3** that reworks how the player moves, aims and looks. In third person the legs, body and camera move naturally. Shots go where the crosshair points. First person gets weapon inertia like in Half-Life 2.

Everything is done in the engine at runtime: no custom skeleton, no animation replacers, no ESP and no scripts. Every feature can be switched off or tuned in `FLOW.ini`.

## Features

### Third person movement
- **Diagonal movement:** the legs face the direction you're moving instead of running straight ahead while the body slides sideways. This includes backward diagonals and a full 360° with a controller stick.
- **Strafing with a weapon drawn:** plays the forward walk or run with the legs turned to the side, so the torso animation and your aim never change.
- **Turn in place:** the feet stay planted while you turn, then the legs pivot round to catch up, with the game's stepping animation.
- **Free look (weapon holstered):** the camera orbits freely while you stand still and the head follows it; moving turns the body to face the camera.
- **Leg and torso lag:** while turning on the move, the legs (and, with the weapon holstered, the torso) trail slightly behind. At full speed you lean into turns.
- **Acceleration:** starting, stopping and changing speed ease in and out (first and third person).
- **No landing stumble:** landing from a jump or a fall plays no stumble animation and never holds you in place: you can move, turn or jump again the moment you touch the ground. The landing sound still plays.
- **Waist twist:** the twist between legs and torso is spread up the spine instead of all at the hips.

### Camera (third person)
- Vertical lag: the camera follows your height with a soft spring, so on a jump you rise and drop ahead of it; stairs and bumps are softened too. Mouse look is never smoothed.
- Smooth view switch: switching between first and third person glides the camera out of your eyes to the third person spot and back in, easing in and out. Pressing the key again mid-glide turns it round smoothly.
- Smooth wheel zoom: each mouse wheel step glides to the new distance instead of jumping.
- Shoulder swap on a key (middle mouse button by default).
- The vanity camera (holding the POV key) keeps the player on the same side of the screen, and the mouse turns it like normal aiming.

### Crosshair and aiming
- **Dynamic crosshair:** the game's own crosshair, split in two, opens up to show your gun's actual spread. The spread includes weapon accuracy, condition, skill, crippled limbs, aiming, crouching, moving and perks. Drawing a gun opens it out to your spread and fades in the centre dot; holstering closes it again. With no gun drawn it replaces the vanilla crosshair, closed and without the dot. Every shot pulls it in toward the centre and back out. It closes while you reload, and stays closed through a reload jam (FLOW waits for the game's jam roll). It hides in scopes, in VATS and when you die. There's also a four-line style, with colour, size and outline options.
- **Shots follow the crosshair:** in third person, bullets normally leave the gun beside the camera and miss what the crosshair is on up close. In first person, a railing in front of the gun but not in front of your eyes stops them. FLOW starts each shot on the crosshair line, level with the gun, so it hits what the crosshair shows. Spread is kept and VATS is untouched.

### First person
- **Gun sway:** the arms and gun trail a little behind the view and float back, easing out instead of stopping hard. Moving sideways, the gun glides a little the other way. The sway looks the same at any pitch and fades out when aiming down the sights, in menus and in VATS.
- **Smooth jumps:** the vanilla jump and land weapon shake is replaced by a soft dip that springs back. Longer falls dip more.
- **Smooth steps:** the vanilla walk/run weapon bob is replaced by a soft rise per step that sways to alternating sides. It is quicker and higher the faster you move, with the game's own footstep sound where each foot lands.
- **Steady gun when looking up and down:** vanilla shifts the weapon on screen as you pitch the view; FLOW keeps it in place.
- **Crouch pose:** while crouched, the gun eases back toward you and a little lower, and returns when you stand up.
- **Vertical slide:** going up stairs, ledges or a jump, the gun slides down a little; coming down or falling, it floats up until you land.

Every patch site is checked byte for byte before anything is written. On a mismatch, FLOW logs the reason and leaves that feature vanilla.

## Requirements

- Fallout 3 / GOTY, patched to **1.7.0.3** without GFWL by the [Fallout Anniversary Patcher](https://www.nexusmods.com/fallout3/mods/24913)
- FOSE 1.2 beta 2 or newer
- Recommended with [TEMPO](https://github.com/bj0urne/Fallout3-TEMPO) for high framerates. FLOW's smoothing is framerate-independent on its own.

## Installing

Copy `FLOW.dll` and `FLOW.ini` into `Data\FOSE\Plugins`. Settings take effect the next time the game starts. `bDebug=1` in `[Debug]` writes a detailed `FLOW.log` next to the DLL.

## Repository layout

```
src/main.cpp           the FOSE plugin (single file)
FLOW.ini               default configuration, every setting documented
build.ps1              builds FLOW.dll and installs it into ..\Data\FOSE\Plugins
tools/package.py       builds the release zips into dist/
README.txt             player readme shipped in the release zip
nexus_description.txt  Nexus page text (BBCode)
nexus_summary.txt      Nexus brief overview and short description
```

`build.ps1` expects this folder to sit inside the Fallout 3 install folder (`Fallout 3 goty\FLOW\`). It installs into `..\Data`, and it never overwrites an existing `FLOW.ini`.

## Building

1. **Compiler:** [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), 32-bit target. For example: `winget install MartinStorsjo.LLVM-MinGW.UCRT`
2. Build:

```powershell
.\build.ps1              # build FLOW.dll and install it into Data\FOSE\Plugins
.\build.ps1 -NoInstall   # build only
```

## How it works

Addresses are for `Fallout3.exe` 1.7.0.3.

- **Bones:** `NiAVObject::UpdateWorldData` (vtable slot `0xB8`) is replaced in the NiNode vtable. Right after the engine computes the player's bone world transforms (after the animation, every frame), FLOW turns the pelvis, spine, head and the whole body. Nothing overwrites these changes, so no custom skeleton is needed.
- **Movement:** the player's movement flags (`PlayerMover::SetMovementFlags`, vtable `+0x0C`) pick the animation. The root-motion vector call at `0x007E89ED` and `Actor::Move` (PlayerCharacter vtable `+0x250`) carry strafe-as-run, controller 360° movement and acceleration.
- **Landing:** the player's landing animation (JumpLand and its directions) is started and its animation slot is ended at once with the game's own slot reset (`0x0045DB50`). The game then starts the movement it had queued for after the landing.
- **Camera:** the third person camera's final `NiAVObject::Update` call (`0x0077E538`) is redirected for the vertical lag, the shoulder swap, the vanity orbit fix and the view switch glide. During a switch (player `+0x5A8` third person now, `+0x5AA` wanted) the game's camera distance (`0x0107BA18`) is set or held so FLOW can glide between the eyes and the third person spot. Wheel zoom: the camera's two reads of the zoom (`0x0107BDBC`, at `0x0077DD9B` and `0x0077DE15`) are pointed at a smoothed copy. The view key's two `SetFirstPerson` calls (`0x0078A4BE` on press, `0x0078A5AA` on release) are hooked so a tap mid-glide reverses it.
- **Crosshair:** drawn in a Direct3D 9 overlay (`IDirect3D9::CreateDevice` → device and swap chain `Present`/`Reset`). The vanilla HUD crosshair is hidden while it shows; with no gun drawn, FLOW's closed crosshair follows the HUD mode's crosshair bit (`HUDMainMenu+0x18C` & `0x40`, already masked by the player's crosshair setting). The spread comes from the same weapon data and settings the game uses for its cone and wobble. Reloads are read from the weapon slot of the player's animation data, and the reload jam roll (`0x00491770`, called at `0x00720179`) is hooked so the crosshair knows whether a jam follows.
- **Shots:** the call to `Projectile::Create` at `0x004BF017` is redirected, and the player's shots get their start point and angles from the crosshair line.
- **First person:** the arms are turned about the head after the first person skeleton (`PlayerCharacter+0x5F0`) updates. Jump and walk/run animations on that skeleton are skipped in `AnimData::PlayAnimGroup` (`0x00461690`). The character controller's state drives the jump dip, and the ground speed drives the step bob. The walk animations' footstep text keys (`Enum: Left/Right`) are replaced by direct calls to the footstep function (`0x006C53A0`). The pitch shift comes from `fFirstPersonHandFollowMult` (vanilla 0.85), which FLOW holds at 1.

## Development and AI disclosure

FLOW was developed with **Claude (Anthropic)**, which did the reverse engineering, wrote the plugin and wrote the documentation. Henry Carlsson directed the project and tested every feature in-game.

## Credits

- The FOSE team, for the Fallout Script Extender
- lStewieAl, for the Fallout Anniversary Patcher
- Drarack's *Diagonal Movement*, which inspired the leg turning

## License

[MIT](LICENSE)
