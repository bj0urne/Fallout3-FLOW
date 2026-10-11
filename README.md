# FCCO — Fallout Character Controller Overhaul

A [FOSE](https://fose.silverlock.org/) plugin for **Fallout 3** that reworks how the player moves, aims and looks. In third person the legs, body and camera move naturally. Shots go where the crosshair points. First person gets weapon inertia like in Half-Life 2.

Everything is done in the engine at runtime: no custom skeleton, no animation replacers, no ESP and no scripts. Every feature can be switched off or tuned in `FCCO.ini`.

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
- Shoulder swap on a key (middle mouse button by default).
- The vanity camera (holding the POV key) keeps the player on the same side of the screen.

### Crosshair and aiming
- **Dynamic crosshair:** the game's own crosshair, split in two, opens up to show your gun's actual spread. The spread includes weapon accuracy, condition, skill, crippled limbs, aiming, crouching, moving and perks. There's also a four-line style, with colour, size and outline options.
- **Shots follow the crosshair:** in third person, bullets normally leave the gun beside the camera and miss what the crosshair is on up close. In first person, a railing in front of the gun but not in front of your eyes stops them. FCCO starts each shot on the crosshair line, level with the gun, so it hits what the crosshair shows. Spread is kept and VATS is untouched.

### First person
- **Gun sway:** the arms and gun trail a little behind the view and float back, easing out instead of stopping hard. Moving sideways, the gun glides a little the other way. The sway looks the same at any pitch and fades out when aiming down the sights, in menus and in VATS.
- **Smooth jumps:** the vanilla jump and land weapon shake is replaced by a soft dip that springs back. Longer falls dip more.
- **Smooth steps:** the vanilla walk/run weapon bob is replaced by a soft dip per step that sways to alternating sides. It is quicker and deeper the faster you move, with the game's own footstep sound where each foot lands.
- **Steady gun when looking up and down:** vanilla shifts the weapon on screen as you pitch the view; FCCO keeps it in place.
- **Crouch pose:** while crouched, the gun eases back toward you and a little lower, and returns when you stand up.
- **Vertical slide:** going up stairs, ledges or a jump, the gun slides down a little; coming down or falling, it floats up until you land.

Every patch site is checked byte for byte before anything is written. On a mismatch, FCCO logs the reason and leaves that feature vanilla.

## Requirements

- Fallout 3 / GOTY, patched to **1.7.0.3** without GFWL by the [Fallout Anniversary Patcher](https://www.nexusmods.com/fallout3/mods/24913)
- FOSE 1.2 beta 2 or newer
- Recommended with [TEMPO](https://github.com/bj0urne/Fallout3-TEMPO) for high framerates. FCCO's smoothing is framerate-independent on its own.

## Installing

Copy `FCCO.dll` and `FCCO.ini` into `Data\FOSE\Plugins`. Settings take effect the next time the game starts. `bDebug=1` in `[Debug]` writes a detailed `FCCO.log` next to the DLL.

## Repository layout

```
src/main.cpp   the FOSE plugin (single file)
FCCO.ini       default configuration, every setting documented
build.ps1      builds FCCO.dll and installs it into ..\Data\FOSE\Plugins
```

`build.ps1` expects this folder to sit inside the Fallout 3 install folder (`Fallout 3 goty\FCCO\`). It installs into `..\Data`, and it never overwrites an existing `FCCO.ini`.

## Building

1. **Compiler:** [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), 32-bit target. For example: `winget install MartinStorsjo.LLVM-MinGW.UCRT`
2. Build:

```powershell
.\build.ps1              # build FCCO.dll and install it into Data\FOSE\Plugins
.\build.ps1 -NoInstall   # build only
```

## How it works

Addresses are for `Fallout3.exe` 1.7.0.3.

- **Bones:** `NiAVObject::UpdateWorldData` (vtable slot `0xB8`) is replaced in the NiNode vtable. Right after the engine computes the player's bone world transforms (after the animation, every frame), FCCO turns the pelvis, spine, head and the whole body. Nothing overwrites these changes, so no custom skeleton is needed.
- **Movement:** the player's movement flags (`PlayerMover::SetMovementFlags`, vtable `+0x0C`) pick the animation. The root-motion vector call at `0x007E89ED` and `Actor::Move` (PlayerCharacter vtable `+0x250`) carry strafe-as-run, controller 360° movement and acceleration.
- **Landing:** the player's landing animation (JumpLand and its directions) is started and its animation slot is ended at once with the game's own slot reset (`0x0045DB50`). The game then starts the movement it had queued for after the landing.
- **Camera:** the third person camera's final `NiAVObject::Update` call (`0x0077E538`) is redirected for the shoulder swap and the vanity orbit fix.
- **Crosshair:** drawn in a Direct3D 9 overlay (`IDirect3D9::CreateDevice` → device and swap chain `Present`/`Reset`). The vanilla HUD crosshair is hidden while it shows. The spread comes from the same weapon data and settings the game uses for its cone and wobble.
- **Shots:** the call to `Projectile::Create` at `0x004BF017` is redirected, and the player's shots get their start point and angles from the crosshair line.
- **First person:** the arms are turned about the head after the first person skeleton (`PlayerCharacter+0x5F0`) updates. Jump and walk/run animations on that skeleton are skipped in `AnimData::PlayAnimGroup` (`0x00461690`). The character controller's state drives the jump dip, and the ground speed drives the step dip. The walk animations' footstep text keys (`Enum: Left/Right`) are replaced by direct calls to the footstep function (`0x006C53A0`). The pitch shift comes from `fFirstPersonHandFollowMult` (vanilla 0.85), which FCCO holds at 1.

## Development and AI disclosure

FCCO was developed with **Claude (Anthropic)**, which did the reverse engineering, wrote the plugin and wrote the documentation. Henry Carlsson directed the project and tested every feature in-game.

## Credits

- The FOSE team, for the Fallout Script Extender
- lStewieAl, for the Fallout Anniversary Patcher
- Drarack's *Diagonal Movement*, which inspired the leg turning

## License

[MIT](LICENSE)
