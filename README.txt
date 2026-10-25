FLOW (Player Control Overhaul)
Version {VERSION} for Fallout 3
=========================================================

Makes moving, aiming and looking around in Fallout 3 feel modern, in third and first person.
A FOSE plugin: no custom skeleton, no animation replacers, no ESP, no scripts. Every feature can
be switched off or tuned in FLOW.ini.


WHAT IT DOES
------------
Third person movement
* Diagonal movement: the legs face the way you move (also backwards, and full 360 degrees on a
  controller stick) instead of running straight ahead while the body slides sideways.
* Strafing with a weapon drawn plays the forward walk/run with the legs turned to the side, so
  your torso and aim never change.
* Turn in place: the feet stay planted while you turn, then the legs pivot round to catch up.
* Free look (weapon holstered): the camera orbits freely while you stand still and the head
  follows it.
* Legs (and with the weapon holstered, the torso) trail slightly behind when you turn on the
  move; at full speed you lean into turns.
* Acceleration: starting and stopping ease in and out (first and third person).
* No landing stumble: you can move the moment you touch the ground after a jump or a fall.

Camera
* Smooth view switch: switching between first and third person glides the camera out of your
  eyes and back in. Press the key again mid-glide and it turns round smoothly.
* Smooth wheel zoom: each mouse wheel step glides instead of jumping.
* Soft vertical lag in third person: on a jump you rise ahead of the camera and drop ahead of it.
* Shoulder swap on a key (X by default).
* The vanity camera (holding the view key) keeps you on the same side of the screen.
* Sitting down no longer throws the camera far away: your own zoom distance is kept.

Crosshair and aiming
* A dynamic crosshair that opens up to show your gun's real spread (accuracy, condition, skill,
  crippled limbs, aiming, crouching, moving, perks).
* Drawing a gun opens it and fades in the centre dot; holstering closes it. Without a gun drawn
  it replaces the vanilla crosshair, closed.
* Every shot pulls it in toward the centre and back out. It closes while you reload (and stays
  closed through a weapon jam), and hides in scopes, VATS and when you die.
* Shots follow the crosshair: in third person bullets no longer miss what the crosshair is on up
  close, and in first person a railing in front of the gun (but not your eyes) no longer stops
  them. Spread is kept and VATS is untouched.

First person
* Gun sway: the gun trails a little behind the view, sliding sideways and up/down, and floats
  back (iSwayStyle=0 brings back the older rotation about your head).
* A soft weapon bob per step (with footstep sounds) instead of the vanilla walk/run bob.
* A soft dip on jumps and landings instead of the vanilla shake.
* The gun stays put when you look up and down (no vanilla shift).
* Moving sideways, the gun glides the other way; going up or down (stairs, ledges, jumps,
  falls), it slides against the movement.
* Automatic guns: every shot kicks the gun back and it glides forward again just in time for the
  next one, swaying side to side in rapid fire (instead of the vanilla fire animation). Flame
  weapons slide back and hold while firing; the minigun keeps its barrel spin.
* Crouch pose: the gun eases back toward you while crouched.
* Steady camera: reloads, jams and shots (the hunting rifle, for one) no longer move your view.

Safety checks
* Every patch location is verified byte for byte first. On an unexpected game version or a
  conflicting mod, FLOW logs the reason and leaves that feature vanilla.


REQUIREMENTS
------------
* Fallout 3 or Fallout 3 GOTY (tested on Steam GOTY)
* Fallout Anniversary Patcher (game version 1.7.0.3, no Games for Windows LIVE)
    https://www.nexusmods.com/fallout3/mods/24913
* Fallout Script Extender (FOSE) 1.2 beta 2 or newer
    https://fose.silverlock.org/
* Windows 10 or 11
* Recommended: TEMPO, for smooth play at high framerates (FLOW works at any framerate on its own)
    https://www.nexusmods.com/fallout3/mods/27726


INSTALLATION
------------
Mod manager (Vortex / MO2): install the archive and enable the mod.

Manual: extract into "Fallout 3\Data\" so you get:
    Data\FOSE\Plugins\FLOW.dll
    Data\FOSE\Plugins\FLOW.ini

Start the game with Fallout3.exe (the Anniversary Patcher loads FOSE) or fose_loader.exe.

To check that it works, open Data\FOSE\Plugins\FLOW.log after starting the game. It should
contain lines starting with "hooked", such as "hooked NiNode::UpdateWorldData" and "hooked
third person camera update", and no "WARNING".


CONFIGURATION
-------------
All options are in Data\FOSE\Plugins\FLOW.ini and are explained there. Changes take effect the
next time the game starts.


COMPATIBILITY
-------------
* Don't combine FLOW with other diagonal movement mods (such as Drarack's Diagonal Movement);
  both turn the legs. Or set bEnabled=0 under [Diagonal].
* First person walk/run and jump animation replacers have no visible effect in first person
  while FLOW's smooth steps / smooth jumps are on (bSmoothSteps / bSmoothJump). Third person
  animations are unaffected.
* Other crosshair mods: FLOW draws its own crosshair and hides the vanilla one. To keep another
  crosshair, set bEnabled=0 under [Crosshair].
* Mods that change the third person camera may conflict with the [Camera] features; switch them
  off there if needed.
* Enhanced Camera: works together with FLOW (first person body and sitting from Enhanced Camera,
  FLOW's camera features on top).
* Only the player is changed; NPCs are vanilla.
* Fallout 3 only. Not for New Vegas or Tale of Two Wastelands.


UNINSTALLING
------------
Remove the files above. FLOW doesn't touch your saves, so it can be added or removed at any time.


CHANGELOG
---------
1.2.2  Sitting: no more character shift when sitting down, the head follows the camera in
       third person, and the seat no longer pushes the camera in (bSeatDoesNotBlockCamera).
       Aiming down the sights: the look sway shows (fSwayAimMult), automatic guns kick
       straight back without side sway (fShotKickSideAimMult), and the crosshair can be hidden
       in first person (bHideWhenAiming, off by default).
1.2.1  First person: the body always faces the camera (with Enhanced Camera you could see
       your own neck). No more diagonal drift after letting go of the movement keys while
       turning.
1.2.0  Gun sway slides sideways and up/down instead of turning about your head (iSwayStyle).
       Steady first person camera during reloads, jams and shots (bSteadyReloadCamera,
       bSteadyFireCamera). Sitting keeps your zoom (bKeepZoomWhenForced). Works together with
       Enhanced Camera. Fixed doubled footsteps in third person. The first view switch after
       loading in third person now glides too. Quicker strafe glide, slower look sway catch-up.
1.1.0  Automatic weapons: the first person fire animation is replaced by the sway system
       (a deep kick back per shot, side-to-side sway in rapid fire; flamer slides back and
       holds; minigun keeps its spin). New [GunSway] settings: bAutomaticKick, fShotKickBack,
       fShotKickRise, fShotKickSide, fShotKickReturnMs, fShotKickAimMult, fFlameKickBack.
1.0.0  First release.
