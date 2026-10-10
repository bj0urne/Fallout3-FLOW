// STRIDE - FOSE plugin for Fallout 3 (1.7.0.3) that makes the player's third person legs move
// more naturally. The upper body (aim, weapon, camera) is never affected.
//   Diagonal:     when moving diagonally, the legs face the direction of travel instead of running
//                 straight ahead while the body slides sideways.
//   Turn in place: when standing still and turning, the feet stay planted until the twist gets
//                 too large (or you stop turning), then the legs pivot round to catch up.
//
// How it works:
//   The engine computes every scene graph node's world transform in NiAVObject::UpdateWorldData
//   (vtable slot 0xB8, 0x833110), called parent-before-child during every update pass, after the
//   animation has written the local transforms. We replace that slot in the NiNode vtable. When the
//   player's pelvis bone is updated we rotate its world transform around the vertical axis, so the
//   legs and everything below follow. When the upper-body bone is updated we rotate it back by the
//   same amount around the same pivot, restoring the original pose from the waist up.
//   Because this runs inside the engine's own update, after the animation every time, nothing
//   overwrites it: no custom skeleton and no script functions are needed.
//
//   The diagonal direction comes from the player's movement flags (PlayerMover+0x94), the same
//   flags the game uses to pick the movement animation. Forward+left plays the forward run, so the
//   legs turn 45 degrees left; back+left plays the backward run, so they turn 45 degrees right.
//   Turning in place follows the player's heading (TESObjectREFR+0x28): while no movement flag is
//   set, each heading change is added to the legs' twist with the opposite sign, so they keep
//   their world facing.
//
// All patch sites are verified byte-for-byte before anything is written.

#include <windows.h>
#include <d3d9.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <vector>

#define PLUGIN_NAME        "STRIDE"
#define PLUGIN_VERSION     1
#define PLUGIN_VERSION_STR "0.1.0"

// ---------------------------------------------------------------------------------------------
// FOSE plugin interface (subset of fose/PluginAPI.h)

struct PluginInfo {
	uint32_t    infoVersion;
	const char* name;
	uint32_t    version;
};

struct FOSEInterface {
	uint32_t foseVersion;
	uint32_t runtimeVersion;
	uint32_t editorVersion;
	uint32_t isEditor;
};

// ---------------------------------------------------------------------------------------------
// Fallout3.exe 1.7.0.3 addresses and layouts

namespace addr {
	constexpr uintptr_t kThePlayer          = 0x0107A104;  // PlayerCharacter*
	constexpr uintptr_t kNiNodeVtbl         = 0x00E213CC;
	constexpr uintptr_t kRootNodeVtbl       = 0x00E8FBC8;  // class of actor 3D roots ("Scene Root")
	constexpr uintptr_t kRagdollCtrlVtbl    = 0x00E32C84;  // bhkRagdollController
	constexpr uintptr_t kPlayerMoverVtbl    = 0x00E1EE04;
	constexpr uintptr_t kUpdateWorldData    = 0x00833110;  // NiAVObject::UpdateWorldData(NiUpdateData*)
	constexpr uintptr_t kSlotUpdateWorldData = 0xB8;       // vtable offset
	// PlayerMover::SetMovementFlags(flags): called with the flags from the controls. Turn bits
	// (0x30) are also latched in +0x98 and re-applied by the per-frame update (0x7E8D40) for
	// 0.1 s (float at 0xF5EAB8), which is what swaps the idle for the turn animation.
	// In PlayerMover's update (0x7E8950): call to AnimData::GetMovementVector(out, actor, a, b)
	// (0x45D370, thiscall, ret 10h). With movement flag 0x40 set (always, for the player) this
	// animation root-motion vector, in the actor's local frame, is the direction you move.
	constexpr uintptr_t kCallAnimMoveVector = 0x007E89ED;
	// AnimData::PlayAnimGroup (thiscall, 4 args, ret 0x10; group = low byte of the first): starts an
	// animation group on a skeleton's animation data (AnimData+8 = that skeleton's root).
	constexpr uintptr_t kPlayAnimGroup      = 0x00461690;
	constexpr uintptr_t kFrameDelta         = 0x01090BAC;  // main TimeGlobal frame delta (game seconds), as used by 0x45D370
	constexpr uintptr_t kActorMove          = 0x00709320;  // Actor::Move, vtable +0x250
	constexpr uintptr_t kSlotActorMove      = 0x250;
	constexpr uintptr_t kPlayerVtbl         = 0x00E18114;
	constexpr uintptr_t kPlayerMoverMoveRet = 0x007E8C8E;  // return address of PlayerMover's call (call edx at 0x7E8C8C)
	constexpr uintptr_t kAnimMoveVector     = 0x0045D370;
	// Third person camera update (around 0x77DD60): the final camera position is written to the
	// camera node's local translation (0x77E4C9), then NiAVObject::Update (0x8264C0, thiscall,
	// one arg) is called on it here.
	constexpr uintptr_t kCallCameraUpdate   = 0x0077E538;
	constexpr uintptr_t kNiAVObjectUpdate   = 0x008264C0;
	// Game settings {vtbl, value, name}: camera side offset and the aim point beside the player.
	constexpr uintptr_t kSettingShoulderX   = 0x00F5EAFC;  // fOverShoulderPosX (40)
	constexpr uintptr_t kSettingAimPointX   = 0x00F5EB44;  // fOverShoulderOffsetPointX (98.4)
	constexpr uintptr_t kSetMovementFlags   = 0x007E8F70;
	// Crosshair. Actor::GetWeaponStat(type) (0x701C60, thiscall, ret 4): type 1 = weapon condition
	// spread factor (fWeaponConditionSpread1..10), type 2 = gun wobble multiplier (condition, skill,
	// crippled limbs, iron sights, crouch/walk/run, "Calculate Gun Spread" perks). The fire function
	// (0x4BE1A0) gives each player shot a random offset within (weapon.spread * type 1 +
	// weapon.minSpread) degrees (0x4BE6FF); type 2 only reaches the player's shots through the gun
	// wobble (0x778A90, smoothed at 0xF6EA18). Weapon = process vfunc +0x148 -> entry+8 -> WEAP.
	constexpr uintptr_t kGetWeaponStat      = 0x00701C60;
	constexpr uintptr_t kIsMenuMode         = 0x00619160;  // bool __cdecl ()
	constexpr uintptr_t kHUDMainMenu        = 0x010762B4;  // HUDMainMenu*; +0x12C crosshair tile, +0x18C mode mask (0x40 = crosshair)
	constexpr uintptr_t kTileGetFloat       = 0x00BECF40;  // float Tile::GetValue(id), thiscall
	constexpr uintptr_t kTileSetFloat       = 0x00BEE190;  // Tile::SetFloat(id, value, propagate), thiscall
	constexpr uintptr_t kTileGetString      = 0x00BECF60;  // const char* Tile::GetString(id), thiscall
	// HUD mode change (0x64DE10): its call setting the crosshair's visibility (Tile::SetFloat)
	constexpr uintptr_t kCallHudCrosshairVisible = 0x0064E3BD;
	constexpr uintptr_t kSettingNonAttackWobble = 0x00F60530;  // fNonAttackGunWobbleMult (value at +4)
	// Third person aim: the fire function (0x4BE1A0) creates each projectile with
	// Projectile::Create (0x7C9D80, cdecl, 16 dword args: +1 shooter, +4..+6 start position,
	// +7 heading, +8 pitch in radians) from this call.
	constexpr uintptr_t kCallCreateProjectile = 0x004BF017;
	constexpr uintptr_t kCreateProjectile   = 0x007C9D80;
	constexpr uintptr_t kVATSMode           = 0x0108D0A0;  // int, 0 = not in VATS (GetVATSMode 0x5122D0)
	constexpr uintptr_t kSettingHUDColor    = 0x01075B74;  // uHUDColor:Interface (RGBA, R in the high byte)
	constexpr uintptr_t kCameraRoot         = 0x0107A224;  // scene camera root; +0xAC = NiCamera
	constexpr uintptr_t kSlotSetMovementFlags = 0x0C;
	// Game state checks, taken from the script condition functions IsWeaponOut (0x510E50 ->
	// Actor::IsWeaponOut 0x6F6C70), GetSitting (0x50F9D0) and GetKnockedState (0x510EC0).
	constexpr uintptr_t kIsWeaponOut        = 0x006F6C70;  // process vfunc +0x450
	constexpr uintptr_t kGetSittingUse      = 0x0050F9F2;  // actor vfunc +0x214
	constexpr uintptr_t kGetKnockedUse      = 0x00510F00;  // process vfunc +0x408
	// Actor::IsDead (actor vfunc +0x22C, called by GetDead 0x50F840): reads the life state +0xFC.
	constexpr uintptr_t kIsDead             = 0x0070C940;
	constexpr uintptr_t kVanityCamera      = 0x0107BA5B;  // bool, hold-POV / idle vanity camera
}

namespace vslot {
	constexpr uintptr_t kProcessIsWeaponOut = 0x450;
	constexpr uintptr_t kProcessKnockState  = 0x408;
	constexpr uintptr_t kProcessIsAiming    = 0x400;  // aiming down the sights (HighProcess +0x228)
	constexpr uintptr_t kProcessCharController = 0x288;  // bhkCharacterController*; state at +0x270
	constexpr uintptr_t kActorSitSleepState = 0x214;
}

namespace off {
	constexpr uintptr_t kFirstPersonNode = 0x5F0;  // PlayerCharacter -> first person skeleton root
	constexpr uintptr_t kRefrRotX        = 0x20;   // TESObjectREFR pitch, radians (look up/down)
	constexpr uintptr_t kRefrRotZ        = 0x28;   // TESObjectREFR heading, radians
	constexpr uintptr_t kRefrPos         = 0x2C;   // TESObjectREFR position (feet for actors)
	constexpr uintptr_t kActorProcess    = 0x60;   // Actor -> BaseProcess
	constexpr uintptr_t kActorLifeState  = 0xFC;   // Actor -> life state (0 alive, 1 dying, 2 dead, ...)
	constexpr uintptr_t kRefrRenderState = 0x5C;   // TESObjectREFR -> render state
	constexpr uintptr_t kRenderStateNode = 0x14;   // render state -> 3rd person root NiNode
	constexpr uintptr_t kPlayerMover     = 0x184;  // PlayerCharacter -> PlayerMover
	constexpr uintptr_t kMoverFlags      = 0x94;   // PlayerMover -> movement flags
	constexpr uintptr_t kMoverInputX     = 0x88;   // PlayerMover -> movement input vector (analog)
	constexpr uintptr_t kMoverInputY     = 0x8C;
	constexpr uintptr_t kThirdPerson     = 0x5A8;  // PlayerCharacter -> bool
	constexpr uintptr_t kRagdollCtrl     = 0xA4;   // Actor -> bhkRagdollController (holds foot IK)
	constexpr uintptr_t kFootIKEnabled   = 0x21D;  // bhkRagdollController -> bool (ToggleFootIK)
}

enum MoveFlags : uint32_t {
	kMove_Forward   = 0x01,
	kMove_Backward  = 0x02,
	kMove_Left      = 0x04,
	kMove_Right     = 0x08,
	kMove_Direction = 0x0F,
	kMove_TurnLeft  = 0x10,
	kMove_TurnRight = 0x20,
	kMove_Turn      = 0x30,
	kMove_Keyboard  = 0x40,  // set with keyboard movement: direction from the animation (8 ways)
};

struct NiMatrix3 { float m[3][3]; };
struct NiPoint3 { float x, y, z; };
struct NiTransform {
	NiMatrix3 rot;
	NiPoint3  pos;
	float     scale;
};

// Only the fields used here; offsets confirmed from NiAVObject::UpdateWorldData (0x8330A0) and
// NiNode::UpdateDownwardPass (0x82DB90).
struct NiAVObject {
	void**      vtbl;      // 00
	uint32_t    refCount;  // 04
	const char* name;      // 08
	uint8_t     pad0C[0x18 - 0x0C];
	NiAVObject* parent;    // 18
	uint8_t     pad1C[0x30 - 0x1C];
	uint32_t    flags;     // 30
	NiTransform local;     // 34
	NiTransform world;     // 68
};
static_assert(offsetof(NiAVObject, parent) == 0x18, "parent");
static_assert(offsetof(NiAVObject, local) == 0x34, "local");
static_assert(offsetof(NiAVObject, world) == 0x68, "world");

static_assert(sizeof(NiAVObject) == 0x9C, "NiAVObject size");

struct NiNode {
	NiAVObject   base;
	uint8_t      pad9C[0xA0 - 0x9C];
	NiAVObject** children;     // A0
	uint16_t     capacity;     // A4
	uint16_t     firstFree;    // A6 (iteration bound)
};
static_assert(offsetof(NiNode, children) == 0xA0, "children");
static_assert(offsetof(NiNode, firstFree) == 0xA6, "firstFree");

// ---------------------------------------------------------------------------------------------
// Config (Data\FOSE\Plugins\STRIDE.ini)

constexpr float kPi       = 3.14159265f;
constexpr float kDegToRad = kPi / 180.0f;
constexpr float kRadToDeg = 180.0f / kPi;
constexpr uint32_t kTileVisible = 0xFA3;  // kTileValue_visible
constexpr uint32_t kTileFilename = 0xFCC;
constexpr uint32_t kTileSystemColor = 0xFF4;  // 1 = HUDMain (uHUDColor), 2 = HUDAlt (red)
constexpr uint32_t kTileUser1 = 0x1005;

struct TurnConfig {                 // angles in radians, times in seconds
	bool  enabled       = true;
	float threshold     = 60.0f * kDegToRad;   // twist at which the legs start to catch up
	float maxTwist      = 90.0f * kDegToRad;   // hard limit
	float catchUpSpeed  = 120.0f * kDegToRad;  // top speed, per second
	float catchUpAccel  = 450.0f * kDegToRad;  // per second squared (also used to slow down)
	bool  turnAnim      = true;                // drive the game's turn animation (see SetMovementFlags)
	float settleDelay   = 1.0f;                // standing still this long: legs line up (0 = never)
	float moveAlignTime = 0.08f;               // smoothing when you start moving
};

struct FreeLookConfig {             // angles in radians, times in seconds
	bool  enabled       = true;
	float alignTime     = 0.15f;                // body turning to face the camera
	float headMaxYaw    = 80.0f * kDegToRad;    // beyond this the head looks ahead again
	float headMaxUp     = 35.0f * kDegToRad;
	float headMaxDown   = 40.0f * kDegToRad;
	float headTime      = 0.15f;                // head smoothing
	float neckShare     = 0.4f;                 // part of the head turn done by Bip01 Neck1
};

struct MoveConfig {                 // angles in radians, times in seconds
	bool  legLag        = true;
	bool  torsoLag      = true;                 // weapon holstered: torso trails too, head looks ahead
	float legLagMax     = 25.0f * kDegToRad;    // how far the legs may trail behind
	float legLagTime    = 0.15f;                // how quickly they catch up
	bool  lean          = true;
	float leanMax       = 8.0f * kDegToRad;
	float leanPerTurn   = 4.0f / 100.0f;        // degrees of lean per degree/second of turning
	float leanTime      = 0.15f;                // lean smoothing
	bool  accel         = true;                 // ease into / out of movement
	float accelTime     = 0.06f;
	float decelTime     = 0.06f;
};

struct CameraConfig {               // times in seconds
	bool  enabled       = true;
	float pivotSmoothZ  = 0.10f;    // smoothing of the height of the point the camera follows
	bool  shoulderSwap  = true;
	int   shoulderKey   = VK_MBUTTON;
	float shoulderTime  = 0.167f;
	bool  vanityFix     = true;     // vanity camera orbits keep the player on the same screen side
};

struct CrosshairConfig {
	bool  enabled       = true;
	int   style         = 0;        // 0 = the game's crosshair split in two, 1 = four lines
	float size          = 1.0f;     // style 0: 1 = the game's own crosshair size
	float dotSize       = 3.0f;     // style 0: centre dot, pixels at 1080p (0 = none)
	float scale         = 1.0f;     // gap = spread cone * scale (calibration)
	float wobbleDeg     = 8.0f;     // degrees the gun sways at full wobble (measured: ~8)
	bool  converge      = true;     // third person: shots fly along the crosshair line
	bool  convergeFirst = true;     // first person: the same
	bool  hideTracers   = true;     // no tracer on those shots (it would come out of the camera)
	float minGap        = 3.0f;     // pixels at 1080p
	float lineLength    = 9.0f;     // pixels at 1080p
	float thickness     = 2.0f;     // pixels at 1080p
	float smoothTime    = 0.06f;    // seconds
	bool  hudColor      = true;     // use the game's HUD colour
	int   r = 26, gr = 255, b = 128;
	float opacity       = 0.9f;
	bool  outline       = true;
	bool  hideScoped    = true;
};

struct GunSwayConfig {
	bool  enabled = true;
	float amount  = 0.3f;    // share of the view's turn the gun trails behind
	float time    = 0.1f;    // seconds: how quickly the gun catches up
	float maxDeg  = 8.0f;    // the sway approaches this (degrees), ever more slowly
	bool  smoothJump = true; // no first person jump/land animations; a soft dip instead
	bool  lockPitch = true;  // the arms follow the view up/down fully (no vanilla pitch shift)
	float jumpDip = 1.5f;    // units the arms dip when jumping
	float landDip = 3.0f;    // ...and when landing after about a second in the air
};

static struct Config {
	bool  enabled        = true;
	float upperBodyTurn  = 0.0f;    // 0 = upper body keeps facing forward, 1 = turns with the legs
	float waistTwist     = 0.5f;    // share of the legs' turn taken by the lower spine (belly)
	bool  diagonals      = true;
	float maxAngle       = 45.0f;   // degrees the legs turn on a diagonal
	float turnTime       = 0.25f;   // seconds, roughly how long the legs take to turn
	bool  backDiagonals  = true;    // also turn the legs when moving diagonally backwards
	bool  wholeBodyDiagonal = true; // weapon holstered: diagonals turn the whole body
	bool  strafeAsRun    = true;    // weapon drawn: pure strafe = forward run with legs turned
	bool  analog360      = true;    // controller: legs/body follow the exact stick angle
	float strafeAngle    = 90.0f;   // degrees the legs turn for that
	TurnConfig turn;
	FreeLookConfig look;
	MoveConfig move;
	CameraConfig cam;
	CrosshairConfig xh;
	GunSwayConfig sway;
	char  bodyNode[64]   = "Scene Root";  // whole body turn; Scene Root = the actor's 3D root
	bool  footIKOff      = false;   // pause the game's foot IK while the body/legs are turned
	char  legsNode[64]   = "Bip01 Pelvis";
	char  upperNode[64]  = "";      // empty = automatic
	bool  debug          = false;
} g_cfg;

static char  g_dir[MAX_PATH];
static FILE* g_log;

static void Log(const char* fmt, ...)
{
	if (!g_log) return;
	SYSTEMTIME st;
	GetLocalTime(&st);
	fprintf(g_log, "[%02u:%02u:%02u.%03u] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(g_log, fmt, ap);
	va_end(ap);
	fputc('\n', g_log);
	fflush(g_log);
}

static float IniFloat(const char* path, const char* sec, const char* key, float def)
{
	char def_s[64], buf[64];
	snprintf(def_s, sizeof(def_s), "%g", def);
	GetPrivateProfileStringA(sec, key, def_s, buf, sizeof(buf), path);
	return (float)atof(buf);
}

static void LoadConfig()
{
	char path[MAX_PATH];
	snprintf(path, sizeof(path), "%s" PLUGIN_NAME ".ini", g_dir);
	auto b = [&](const char* sec, const char* key, bool def) {
		return GetPrivateProfileIntA(sec, key, def ? 1 : 0, path) != 0;
	};
	auto deg = [&](const char* sec, const char* key, float defRad) {
		return IniFloat(path, sec, key, defRad * kRadToDeg) * kDegToRad;
	};
	auto clampf = [](float& v, float lo, float hi) { v = v < lo ? lo : (v > hi ? hi : v); };

	g_cfg.enabled       = b("General", "bEnabled", g_cfg.enabled);
	g_cfg.upperBodyTurn = IniFloat(path, "General", "fUpperBodyTurn", g_cfg.upperBodyTurn);
	g_cfg.waistTwist    = IniFloat(path, "General", "fWaistTwist", g_cfg.waistTwist);

	g_cfg.diagonals     = b("Diagonal", "bEnabled", g_cfg.diagonals);
	g_cfg.maxAngle      = IniFloat(path, "Diagonal", "fMaxAngle", g_cfg.maxAngle);
	g_cfg.turnTime      = IniFloat(path, "Diagonal", "fTurnTimeMs", g_cfg.turnTime * 1000.0f) / 1000.0f;
	g_cfg.backDiagonals = b("Diagonal", "bBackDiagonals", g_cfg.backDiagonals);
	g_cfg.wholeBodyDiagonal = b("Diagonal", "bWholeBodyWhenHolstered", g_cfg.wholeBodyDiagonal);
	g_cfg.strafeAsRun   = b("Diagonal", "bStrafeAsRunWithWeapon", g_cfg.strafeAsRun);
	g_cfg.analog360     = b("Diagonal", "bController360", g_cfg.analog360);
	Log("config: Diagonal: bController360=%d", g_cfg.analog360);
	g_cfg.strafeAngle   = IniFloat(path, "Diagonal", "fStrafeAngle", g_cfg.strafeAngle);

	TurnConfig& t = g_cfg.turn;
	t.enabled       = b("TurnInPlace", "bEnabled", t.enabled);
	t.threshold     = deg("TurnInPlace", "fThreshold", t.threshold);
	t.maxTwist      = deg("TurnInPlace", "fMaxTwist", t.maxTwist);
	t.catchUpSpeed  = deg("TurnInPlace", "fCatchUpSpeed", t.catchUpSpeed);
	t.catchUpAccel  = deg("TurnInPlace", "fCatchUpAccel", t.catchUpAccel);
	t.turnAnim      = b("TurnInPlace", "bTurnAnimation", t.turnAnim);
	t.settleDelay   = IniFloat(path, "TurnInPlace", "fSettleDelay", t.settleDelay);
	t.moveAlignTime = IniFloat(path, "TurnInPlace", "fMoveAlignTimeMs", t.moveAlignTime * 1000.0f) / 1000.0f;

	FreeLookConfig& l = g_cfg.look;
	l.enabled       = b("FreeLook", "bEnabled", l.enabled);
	l.alignTime     = IniFloat(path, "FreeLook", "fBodyAlignTimeMs", l.alignTime * 1000.0f) / 1000.0f;
	l.headMaxYaw    = deg("FreeLook", "fHeadMaxYaw", l.headMaxYaw);
	l.headMaxUp     = deg("FreeLook", "fHeadMaxUp", l.headMaxUp);
	l.headMaxDown   = deg("FreeLook", "fHeadMaxDown", l.headMaxDown);
	l.headTime      = IniFloat(path, "FreeLook", "fHeadTurnTimeMs", l.headTime * 1000.0f) / 1000.0f;
	l.neckShare     = IniFloat(path, "FreeLook", "fNeckShare", l.neckShare);

	MoveConfig& m = g_cfg.move;
	m.legLag        = b("Moving", "bLegLag", m.legLag);
	m.torsoLag      = b("Moving", "bTorsoLag", m.torsoLag);
	m.legLagMax     = deg("Moving", "fLegLagMax", m.legLagMax);
	m.legLagTime    = IniFloat(path, "Moving", "fLegLagTimeMs", m.legLagTime * 1000.0f) / 1000.0f;
	m.lean          = b("Moving", "bLean", m.lean);
	m.leanMax       = deg("Moving", "fLeanMax", m.leanMax);
	m.leanPerTurn   = IniFloat(path, "Moving", "fLeanPer100DegPerSec", m.leanPerTurn * 100.0f) / 100.0f;
	m.leanTime      = IniFloat(path, "Moving", "fLeanTimeMs", m.leanTime * 1000.0f) / 1000.0f;
	m.accel         = b("Moving", "bAcceleration", m.accel);
	m.accelTime     = IniFloat(path, "Moving", "fAccelerationMs", m.accelTime * 1000.0f) / 1000.0f;
	m.decelTime     = IniFloat(path, "Moving", "fDecelerationMs", m.decelTime * 1000.0f) / 1000.0f;

	GunSwayConfig& gs = g_cfg.sway;
	gs.enabled = b("GunSway", "bEnabled", gs.enabled);
	gs.amount  = IniFloat(path, "GunSway", "fAmount", gs.amount);
	gs.time    = IniFloat(path, "GunSway", "fCatchUpMs", gs.time * 1000.0f) / 1000.0f;
	gs.maxDeg  = IniFloat(path, "GunSway", "fMaxDegrees", gs.maxDeg);
	clampf(gs.amount, 0.0f, 2.0f);
	clampf(gs.time, 0.01f, 2.0f);
	clampf(gs.maxDeg, 0.0f, 30.0f);
	gs.smoothJump = b("GunSway", "bSmoothJump", gs.smoothJump);
	gs.lockPitch  = b("GunSway", "bLockPitch", gs.lockPitch);
	gs.jumpDip    = IniFloat(path, "GunSway", "fJumpDip", gs.jumpDip);
	gs.landDip    = IniFloat(path, "GunSway", "fLandDip", gs.landDip);
	clampf(gs.jumpDip, 0.0f, 20.0f);
	clampf(gs.landDip, 0.0f, 20.0f);
	Log("config: GunSway: bEnabled=%d fAmount=%.2f fCatchUpMs=%.0f fMaxDegrees=%.1f bSmoothJump=%d fJumpDip=%.1f "
	    "fLandDip=%.1f bLockPitch=%d", gs.enabled, gs.amount, gs.time * 1000.0f, gs.maxDeg, gs.smoothJump, gs.jumpDip,
	    gs.landDip, gs.lockPitch);

	CrosshairConfig& x = g_cfg.xh;
	x.enabled    = b("Crosshair", "bEnabled", x.enabled);
	x.style      = GetPrivateProfileIntA("Crosshair", "iStyle", x.style, path);
	x.size       = IniFloat(path, "Crosshair", "fSize", x.size);
	x.dotSize    = IniFloat(path, "Crosshair", "fDotSize", x.dotSize);
	x.scale      = IniFloat(path, "Crosshair", "fSpreadScale", x.scale);
	x.wobbleDeg  = IniFloat(path, "Crosshair", "fWobbleDegrees", x.wobbleDeg);
	x.converge   = b("Crosshair", "bThirdPersonAimAtCrosshair", x.converge);
	x.convergeFirst = b("Crosshair", "bFirstPersonAimAtCrosshair", x.convergeFirst);
	x.hideTracers   = b("Crosshair", "bHideTracersOnCrosshairLine", x.hideTracers);
	x.minGap     = IniFloat(path, "Crosshair", "fMinGap", x.minGap);
	x.lineLength = IniFloat(path, "Crosshair", "fLineLength", x.lineLength);
	x.thickness  = IniFloat(path, "Crosshair", "fThickness", x.thickness);
	x.smoothTime = IniFloat(path, "Crosshair", "fSmoothMs", x.smoothTime * 1000.0f) / 1000.0f;
	x.hudColor   = b("Crosshair", "bUseHUDColor", x.hudColor);
	x.r          = GetPrivateProfileIntA("Crosshair", "iRed", x.r, path);
	x.gr         = GetPrivateProfileIntA("Crosshair", "iGreen", x.gr, path);
	x.b          = GetPrivateProfileIntA("Crosshair", "iBlue", x.b, path);
	x.opacity    = IniFloat(path, "Crosshair", "fOpacity", x.opacity);
	x.outline    = b("Crosshair", "bOutline", x.outline);
	x.hideScoped = b("Crosshair", "bHideWhenScoped", x.hideScoped);
	clampf(x.scale, 0.1f, 10.0f);
	clampf(x.wobbleDeg, 0.0f, 45.0f);
	clampf(x.size, 0.1f, 10.0f);
	clampf(x.dotSize, 0.0f, 50.0f);
	clampf(x.minGap, 0.0f, 100.0f);
	clampf(x.lineLength, 1.0f, 100.0f);
	clampf(x.thickness, 1.0f, 20.0f);
	clampf(x.smoothTime, 0.0f, 1.0f);
	clampf(x.opacity, 0.0f, 1.0f);
	Log("config: Crosshair: iStyle=%d fSize=%.2f fDotSize=%.1f fWobbleDegrees=%.1f bThirdPersonAimAtCrosshair=%d "
	    "bFirstPersonAimAtCrosshair=%d bHideTracersOnCrosshairLine=%d",
	    x.style, x.size, x.dotSize, x.wobbleDeg, x.converge, x.convergeFirst, x.hideTracers);
	Log("config: Crosshair: bEnabled=%d fSpreadScale=%.2f fMinGap=%.1f fLineLength=%.1f fThickness=%.1f "
	    "fSmoothMs=%.0f bUseHUDColor=%d rgb=%d,%d,%d fOpacity=%.2f bOutline=%d bHideWhenScoped=%d",
	    x.enabled, x.scale, x.minGap, x.lineLength, x.thickness, x.smoothTime * 1000.0f, x.hudColor,
	    x.r, x.gr, x.b, x.opacity, x.outline, x.hideScoped);

	CameraConfig& c = g_cfg.cam;
	c.enabled       = b("Camera", "bEnabled", c.enabled);
	c.pivotSmoothZ  = IniFloat(path, "Camera", "fFollowSmoothVerticalMs", c.pivotSmoothZ * 1000.0f) / 1000.0f;
	c.vanityFix     = b("Camera", "bVanityOrbitFix", c.vanityFix);
	c.shoulderSwap  = b("Camera", "bShoulderSwap", c.shoulderSwap);
	c.shoulderKey   = GetPrivateProfileIntA("Camera", "iShoulderSwapKey", c.shoulderKey, path);
	c.shoulderTime  = IniFloat(path, "Camera", "fShoulderSwapMs", c.shoulderTime * 1000.0f) / 1000.0f;
	clampf(c.pivotSmoothZ, 0.0f, 1.0f);
	clampf(c.shoulderTime, 0.0f, 3.0f);
	Log("config: Camera: bEnabled=%d fFollowSmoothVerticalMs=%.0f bVanityOrbitFix=%d "
	    "bShoulderSwap=%d iShoulderSwapKey=0x%X fShoulderSwapMs=%.0f",
	    c.enabled, c.pivotSmoothZ * 1000.0f, c.vanityFix,
	    c.shoulderSwap, c.shoulderKey, c.shoulderTime * 1000.0f);

	GetPrivateProfileStringA("Skeleton", "sBodyNode", g_cfg.bodyNode, g_cfg.bodyNode, sizeof(g_cfg.bodyNode), path);
	g_cfg.footIKOff = b("Skeleton", "bDisableFootIKWhenTurned", g_cfg.footIKOff);
	GetPrivateProfileStringA("Skeleton", "sLegsNode", g_cfg.legsNode, g_cfg.legsNode, sizeof(g_cfg.legsNode), path);
	GetPrivateProfileStringA("Skeleton", "sUpperBodyNode", g_cfg.upperNode, g_cfg.upperNode, sizeof(g_cfg.upperNode), path);
	g_cfg.debug         = b("Debug", "bDebug", g_cfg.debug);

	clampf(g_cfg.maxAngle, 0.0f, 90.0f);
	clampf(g_cfg.strafeAngle, 0.0f, 120.0f);
	clampf(g_cfg.turnTime, 0.0f, 2.0f);
	clampf(g_cfg.upperBodyTurn, 0.0f, 1.0f);
	clampf(g_cfg.waistTwist, 0.0f, 1.0f);
	clampf(t.maxTwist, 0.0f, 120.0f * kDegToRad);
	clampf(t.threshold, 0.0f, t.maxTwist);
	clampf(t.catchUpSpeed, 10.0f * kDegToRad, 3600.0f * kDegToRad);
	clampf(t.catchUpAccel, 30.0f * kDegToRad, 36000.0f * kDegToRad);
	clampf(t.settleDelay, 0.0f, 60.0f);
	clampf(t.moveAlignTime, 0.0f, 2.0f);
	clampf(l.alignTime, 0.0f, 2.0f);
	clampf(l.headMaxYaw, 0.0f, 90.0f * kDegToRad);
	clampf(l.headMaxUp, 0.0f, 80.0f * kDegToRad);
	clampf(l.headMaxDown, 0.0f, 80.0f * kDegToRad);
	clampf(l.headTime, 0.0f, 2.0f);
	clampf(l.neckShare, 0.0f, 1.0f);
	clampf(m.legLagMax, 0.0f, 60.0f * kDegToRad);
	clampf(m.legLagTime, 0.01f, 2.0f);
	clampf(m.leanMax, 0.0f, 30.0f * kDegToRad);
	clampf(m.leanPerTurn, 0.0f, 1.0f);
	clampf(m.leanTime, 0.0f, 2.0f);
	clampf(m.accelTime, 0.0f, 2.0f);
	clampf(m.decelTime, 0.0f, 2.0f);

	Log("config: bEnabled=%d fUpperBodyTurn=%.2f | Diagonal: bEnabled=%d fMaxAngle=%.1f fTurnTimeMs=%.0f "
	    "bBackDiagonals=%d | TurnInPlace: bEnabled=%d fThreshold=%.1f fMaxTwist=%.1f fCatchUpSpeed=%.0f "
	    "fCatchUpAccel=%.0f bTurnAnimation=%d fSettleDelay=%.2f fMoveAlignTimeMs=%.0f | sLegsNode=\"%s\" "
	    "sUpperBodyNode=\"%s\" bDebug=%d",
	    g_cfg.enabled, g_cfg.upperBodyTurn, g_cfg.diagonals, g_cfg.maxAngle, g_cfg.turnTime * 1000.0f,
	    g_cfg.backDiagonals, t.enabled, t.threshold * kRadToDeg, t.maxTwist * kRadToDeg,
	    t.catchUpSpeed * kRadToDeg, t.catchUpAccel * kRadToDeg, t.turnAnim, t.settleDelay, t.moveAlignTime * 1000.0f,
	    g_cfg.legsNode, g_cfg.upperNode, g_cfg.debug);
	Log("config: fWaistTwist=%.2f | Diagonal: bWholeBodyWhenHolstered=%d bStrafeAsRunWithWeapon=%d "
	    "fStrafeAngle=%.0f | Skeleton: sBodyNode=\"%s\" bDisableFootIKWhenTurned=%d",
	    g_cfg.waistTwist, g_cfg.wholeBodyDiagonal, g_cfg.strafeAsRun, g_cfg.strafeAngle, g_cfg.bodyNode,
	    g_cfg.footIKOff);
	Log("config: FreeLook: bEnabled=%d fBodyAlignTimeMs=%.0f fHeadMaxYaw=%.1f fHeadMaxUp=%.1f "
	    "fHeadMaxDown=%.1f fHeadTurnTimeMs=%.0f fNeckShare=%.2f",
	    l.enabled, l.alignTime * 1000.0f, l.headMaxYaw * kRadToDeg, l.headMaxUp * kRadToDeg,
	    l.headMaxDown * kRadToDeg, l.headTime * 1000.0f, l.neckShare);
	Log("config: Moving: bLegLag=%d bTorsoLag=%d fLegLagMax=%.1f fLegLagTimeMs=%.0f bLean=%d fLeanMax=%.1f "
	    "fLeanPer100DegPerSec=%.2f fLeanTimeMs=%.0f",
	    m.legLag, m.torsoLag, m.legLagMax * kRadToDeg, m.legLagTime * 1000.0f, m.lean, m.leanMax * kRadToDeg,
	    m.leanPerTurn * 100.0f, m.leanTime * 1000.0f);
	Log("config: Moving: bAcceleration=%d fAccelerationMs=%.0f fDecelerationMs=%.0f", m.accel,
	    m.accelTime * 1000.0f, m.decelTime * 1000.0f);
}

// ---------------------------------------------------------------------------------------------
// Scene graph helpers

// NiObject vtable +0x0C: GetAsNiNode() - returns this for NiNode and subclasses, else null.
static NiNode* AsNode(NiAVObject* o)
{
	typedef NiNode*(__thiscall* Fn)(NiAVObject*);
	return o ? ((Fn)o->vtbl[3])(o) : nullptr;
}

static bool NameIs(const NiAVObject* o, const char* name)
{
	return o && o->name && _stricmp(o->name, name) == 0;
}

static NiAVObject* FindByName(NiAVObject* o, const char* name, int depth = 0)
{
	if (!o || depth > 64) return nullptr;
	if (NameIs(o, name)) return o;
	NiNode* n = AsNode(o);
	if (!n) return nullptr;
	for (uint16_t i = 0; i < n->firstFree; i++)
		if (NiAVObject* hit = FindByName(n->children[i], name, depth + 1))
			return hit;
	return nullptr;
}

static void DumpTree(NiAVObject* o, int depth)
{
	if (!o || depth > 16) return;
	Log("  %*s%s [vtbl %08X]", depth * 2, "", o->name ? o->name : "(null)", (unsigned)(uintptr_t)o->vtbl);
	NiNode* n = AsNode(o);
	if (!n) return;
	for (uint16_t i = 0; i < n->firstFree; i++)
		DumpTree(n->children[i], depth + 1);
}

static void* Player()
{
	return *(void**)addr::kThePlayer;
}

static NiAVObject* PlayerRoot()
{
	uint8_t* pc = (uint8_t*)Player();
	if (!pc) return nullptr;
	uint8_t* rs = *(uint8_t**)(pc + off::kRefrRenderState);
	if (!rs) return nullptr;
	return *(NiAVObject**)(rs + off::kRenderStateNode);
}

// Heading (rotation about Z) in radians; increases when turning right.
static float PlayerHeading()
{
	uint8_t* pc = (uint8_t*)Player();
	return pc ? *(float*)(pc + off::kRefrRotZ) : NAN;
}

static bool InThirdPerson()
{
	uint8_t* pc = (uint8_t*)Player();
	return pc && *(bool*)(pc + off::kThirdPerson);
}

// Look pitch in radians (TESObjectREFR rotX); sign confirmed in game, see UpdateHead.
static float PlayerPitch()
{
	uint8_t* pc = (uint8_t*)Player();
	return pc ? *(float*)(pc + off::kRefrRotX) : 0.0f;
}

static bool g_stateChecksOk;  // the game functions below matched at load

template <typename R>
static R VCall(void* obj, uintptr_t slot)
{
	typedef R(__thiscall* Fn)(void*);
	return ((Fn)(*(void***)obj)[slot / 4])(obj);
}

static void* PlayerProcess()
{
	uint8_t* pc = (uint8_t*)Player();
	return pc ? *(void**)(pc + off::kActorProcess) : nullptr;
}

static bool PlayerWeaponOut()
{
	void* proc = PlayerProcess();
	return proc && VCall<bool>(proc, vslot::kProcessIsWeaponOut);
}

static uint32_t PlayerSitSleepState()   // 0 = not sitting/sleeping
{
	void* pc = Player();
	return pc ? VCall<uint32_t>(pc, vslot::kActorSitSleepState) : 0;
}

static uint32_t PlayerKnockState()      // 1..4 = exploding/knocked down/getting up
{
	void* proc = PlayerProcess();
	return proc ? VCall<uint32_t>(proc, vslot::kProcessKnockState) : 0;
}

static bool g_lifeStateOk;  // Actor::IsDead matched at load (life state offset confirmed)

// Non-null while the player's skeleton is (or may be) driven by the Havok ragdoll: dying, dead,
// unconscious, or knocked down by an explosion. Bones must not be touched then: turning them under
// the ragdoll makes the physics explode (body and death camera thrown kilometres away).
static const char* PlayerRagdollReason()
{
	uint8_t* pc = (uint8_t*)Player();
	if (!pc) return nullptr;
	if (g_lifeStateOk && *(uint32_t*)(pc + off::kActorLifeState) != 0) return "dead/dying";
	if (g_stateChecksOk) {
		const uint32_t knock = PlayerKnockState();
		if (knock >= 1 && knock <= 4) return "knocked down";
	}
	return nullptr;
}

static uint32_t PlayerMoveFlags()
{
	uint8_t* pc = (uint8_t*)Player();
	if (!pc) return 0;
	void** mover = *(void***)(pc + off::kPlayerMover);
	if (!mover || (uintptr_t)*mover != addr::kPlayerMoverVtbl) return 0;
	return *(uint32_t*)((uint8_t*)mover + off::kMoverFlags);
}

// ---------------------------------------------------------------------------------------------
// Rotation state

static struct State {
	NiAVObject* root    = nullptr;  // player 3D root the nodes below were found under
	NiAVObject* legs    = nullptr;  // rotated toward the movement direction
	NiAVObject* upper   = nullptr;  // upper body: turned back (if below legs) or partly along
	bool        upperIsChild = false;
	NiAVObject* spine1  = nullptr;  // waist twist (see WaistShare)
	NiAVObject* spine2  = nullptr;
	NiAVObject* body    = nullptr;  // Bip01: whole skeleton, turned back during free look
	NiAVObject* neck    = nullptr;  // Bip01 Neck1 and Bip01 Head: look toward the camera
	NiAVObject* head    = nullptr;
	bool        ragdoll = false;    // dead/knocked down: hands off the skeleton (PlayerRagdollReason)
	bool        freeLook = false;   // weapon holstered, standing still: body stays put
	bool        torsoLag = false;   // weapon holstered, moving: whole body trails behind turns
	bool        holsteredMoving = false;  // weapon holstered, moving (free look's only blocker)
	float       bodyDiag = 0.0f;    // whole body yaw from forward diagonals (weapon holstered)
	float       bodyDiagVel = 0.0f;
	const char* freeLookBlocker = "";  // debug: why free look is off
	float       bodyYaw = 0.0f;     // whole body yaw relative to the actor heading, + = left
	float       headYaw = 0.0f;     // head yaw relative to the body
	float       headPitch = 0.0f;   // + = up
	bool        headLooking = true; // false after the camera went past fHeadMaxYaw
	float       turnRate = 0.0f;    // smoothed heading change, rad/s
	float       lean    = 0.0f;     // body roll, + = lean right
	uint8_t*    footIKCtrl = nullptr;  // bDisableFootIKWhenTurned state
	bool        footIKPaused = false;
	bool        footIKSaved = true;
	double      debugTimer = 0.0;
	float       diagonal = 0.0f;    // leg yaw from diagonal movement, radians, + = left
	float       diagonalVel = 0.0f; // its rate of change (spring state), rad/s
	float       twist   = 0.0f;     // leg yaw from turning in place
	float       angle   = 0.0f;     // diagonal + twist
	float       applied = 0.0f;     // yaw currently applied to legs->world
	float       lastHeading = 0.0f;
	bool        haveHeading = false;
	bool        catchingUp = false; // legs are pivoting back toward the body
	float       catchVel = 0.0f;    // current catch-up speed, rad/s
	double      stillTime = 0.0;    // seconds standing still without turning
	float       lastDHeading = 0.0f;
	uint32_t    lastTurnBits = 0;   // debug logging only
	uint32_t    strafeKeys = 0;     // real L/R key while a pure strafe is sent to the game as forward
	bool        analog = false;     // controller stick movement (exact angle, see Hook_SetMovementFlags)
	bool        analogBack = false; // playing the backward animation
	float       analogAngle = 0.0f; // stick direction relative to facing, + = left
	NiPoint3    pivot   = {};       // legs world position when it was applied
	int64_t     lastQpc = 0;
	uint32_t    lastFlags = 0;
} g;

static double g_qpcFreq;

static int64_t QpcNow()
{
	LARGE_INTEGER li;
	QueryPerformanceCounter(&li);
	return li.QuadPart;
}

// Whole-body yaw applied at Bip01, relative to the actor heading (+ = left).
static float BodyYawTotal()
{
	return g.bodyYaw + g.bodyDiag;
}

static void Rescan(NiAVObject* root)
{
	g.root = root;
	g.legs = FindByName(root, g_cfg.legsNode);
	g.upper = nullptr;
	g.applied = 0.0f;

	// The hook only sees plain NiNodes (the class whose vtable is patched).
	auto hooked = [](NiAVObject* o) { return o && (uintptr_t)o->vtbl == addr::kNiNodeVtbl; };
	g.upperIsChild = false;
	if (hooked(g.legs)) {
		// The upper body bone is either below the legs bone (then it is turned back) or next to
		// it (vanilla FO3: Bip01 NonAccum -> {Bip01 Pelvis -> thighs, Bip01 Spine -> torso}),
		// then it is left alone unless fUpperBodyTurn asks for it to follow.
		const char* upperName = g_cfg.upperNode[0] ? g_cfg.upperNode : "Bip01 Spine";
		g.upper = FindByName(g.legs, upperName);
		if (g.upper) {
			g.upperIsChild = true;
			// Thighs hanging off the spine: turn back the next spine bone instead.
			NiAVObject* thigh = FindByName(g.legs, "Bip01 L Thigh");
			if (!g_cfg.upperNode[0] && thigh && thigh->parent == g.upper)
				g.upper = FindByName(g.upper, "Bip01 Spine1");
		} else if (g.legs->parent) {
			g.upper = FindByName(g.legs->parent, upperName);
		}
		if (!hooked(g.upper)) g.upper = nullptr;
	} else {
		g.legs = nullptr;
	}

	Log("player 3D %p: legs \"%s\" %p, upper body \"%s\" %p (%s)", (void*)root,
	    g.legs ? g.legs->name : g_cfg.legsNode, (void*)g.legs,
	    g.upper ? g.upper->name : "?", (void*)g.upper,
	    g.upperIsChild ? "below legs, turned back" : "beside legs");
	if (!g.legs || (g.upperIsChild && !g.upper)) {
		// Turning the legs without turning the torso back would twist the aim; do nothing.
		Log("WARNING: skeleton nodes not found; diagonal legs inactive for this skeleton");
		g.legs = g.upper = nullptr;
	}
	// Waist twist bones (vanilla layout only: Spine -> Spine1 -> Spine2).
	g.spine1 = g.spine2 = nullptr;
	if (g.upper && !g.upperIsChild) {
		g.spine1 = FindByName(g.upper, "Bip01 Spine1");
		g.spine2 = g.spine1 ? FindByName(g.spine1, "Bip01 Spine2") : nullptr;
		if (!hooked(g.spine1) || !hooked(g.spine2) || g.spine1->parent != g.upper ||
		    g.spine2->parent != g.spine1) {
			g.spine1 = g.spine2 = nullptr;
			Log("waist twist: Spine1/Spine2 not found; twist stays at the hips");
		}
	}

	// Free look: the legs bone must hang below the body bone so the whole skeleton turns.
	// Turning the 3D root (default) instead of Bip01 keeps the game's foot IK consistent: it
	// places the feet in the root's frame, so with Bip01 turned it cast its ground rays where the
	// feet would be without the turn (one leg straightened, the body lifted onto steps/curbs).
	g.body = g_cfg.bodyNode[0] ? FindByName(root, g_cfg.bodyNode) : root;
	g.neck = FindByName(root, "Bip01 Neck1");
	g.head = FindByName(root, "Bip01 Head");
	const bool bodyHooked = g.body && ((uintptr_t)g.body->vtbl == addr::kNiNodeVtbl ||
	                                   (uintptr_t)g.body->vtbl == addr::kRootNodeVtbl);
	if (!bodyHooked || !g.legs || !FindByName(g.body, g.legs->name)) g.body = nullptr;
	if (!hooked(g.neck)) g.neck = nullptr;
	if (!hooked(g.head)) g.head = nullptr;
	Log("free look nodes: body \"%s\" %p, neck %p, head %p", g.body ? g.body->name : g_cfg.bodyNode,
	    (void*)g.body, (void*)g.neck, (void*)g.head);
	if (g_cfg.look.enabled && !g.body)
		Log("WARNING: body node \"%s\" not usable; free look inactive for this skeleton", g_cfg.bodyNode);
	if (g_cfg.debug) {
		Log("skeleton:");
		DumpTree(root, 0);
	}
}

static float WrapPi(float a)
{
	while (a > kPi) a -= 2.0f * kPi;
	while (a < -kPi) a += 2.0f * kPi;
	return a;
}

static float TargetAngle(uint32_t flags)
{
	if (g.analog) {
		// Controller: exact stick angle relative to the forward or backward animation.
		if (g.analogBack && !g_cfg.backDiagonals) return 0.0f;
		float t = g.analogBack ? WrapPi(g.analogAngle - kPi) : g.analogAngle;
		const float lim = 125.0f * kDegToRad;
		return t > lim ? lim : (t < -lim ? -lim : t);
	}
	const float a = g_cfg.maxAngle * kDegToRad;
	const bool f = flags & kMove_Forward, bk = flags & kMove_Backward;
	const bool l = flags & kMove_Left, r = flags & kMove_Right;
	if (l != r && !f && !bk && g.strafeKeys)       // pure strafe played as a forward run
		return (l ? 1.0f : -1.0f) * g_cfg.strafeAngle * kDegToRad;
	if (l == r || f == bk) return 0.0f;           // not a diagonal
	if (f) return l ? a : -a;                      // forward run: turn toward the side
	if (!g_cfg.backDiagonals) return 0.0f;
	return l ? -a : a;                             // backward run: turn away from the side
}

// Exponential approach toward target; framerate independent and safe to call several times per
// frame (dt is then just smaller).
static float Approach(float cur, float target, double dt, float timeConstant)
{
	if (timeConstant <= 0.0f) return target;
	const float k = 1.0f - (float)std::exp(-dt / timeConstant);
	return cur + (target - cur) * k;
}

// Critically damped spring toward target (velocity kept in *vel): starts gently, accelerates, and
// settles without overshoot. smoothTime is roughly the time to get there. Framerate independent.
static float SmoothDamp(float cur, float target, float* vel, double dt, float smoothTime)
{
	if (smoothTime <= 0.0f) { *vel = 0.0f; return target; }
	if (dt <= 0.0) return cur;
	const float omega = 2.0f / smoothTime;
	const float x = omega * (float)dt;
	const float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
	const float change = cur - target;
	const float temp = (*vel + omega * change) * (float)dt;
	*vel = (*vel - omega * temp) * e;
	return target + (change + temp) * e;
}

// Turn in place: while standing still, the legs keep their world facing when the body turns.
// The heading delta is added to the twist (body turning right = heading up = legs relatively
// left = positive twist). Past the threshold, or after a pause, the legs pivot back to zero.
static void UpdateTwist(uint32_t flags, double dt, float dHeading)
{
	const TurnConfig& tc = g_cfg.turn;
	const MoveConfig& mc = g_cfg.move;

	const bool moving = flags & kMove_Direction;
	if (moving) {
		// Moving: the legs trail a little behind when you turn (up to fLegLagMax), then spring
		// back in line. A larger twist left over from turning in place lines up quickly.
		if (mc.legLag && !g.torsoLag) {  // with torso lag the legs trail along with the body
			const float limit = std::fabs(g.twist) > mc.legLagMax ? std::fabs(g.twist) : mc.legLagMax;
			g.twist += dHeading;
			if (g.twist > limit) g.twist = limit;
			if (g.twist < -limit) g.twist = -limit;
		}
		const bool leftover = !mc.legLag || std::fabs(g.twist) > mc.legLagMax;
		g.twist = Approach(g.twist, 0.0f, dt, leftover ? tc.moveAlignTime : mc.legLagTime);
		if (std::fabs(g.twist) < 1e-4f) g.twist = 0.0f;
		g.catchingUp = false;
		g.stillTime = 0.0;
		return;
	}
	if (!tc.enabled) {
		g.twist = Approach(g.twist, 0.0f, dt, tc.moveAlignTime);
		if (std::fabs(g.twist) < 1e-4f) g.twist = 0.0f;
		return;
	}

	g.twist += dHeading;
	g.stillTime = (dHeading == 0.0f) ? g.stillTime + dt : 0.0;

	const bool settle = tc.settleDelay > 0.0f && g.stillTime >= tc.settleDelay && g.twist != 0.0f;
	if (!g.catchingUp && (std::fabs(g.twist) >= tc.threshold || settle)) {
		g.catchingUp = true;
		g.catchVel = 0.0f;
		if (g_cfg.debug)
			Log("turn in place: legs catch up from %.1f deg (%s)", g.twist * kRadToDeg,
			    settle ? "settle" : "threshold");
	}
	if (g.catchingUp) {
		// Speed profile: accelerate from rest at fCatchUpAccel, cruise at fCatchUpSpeed, and
		// decelerate at the same rate so the legs come to rest exactly in line.
		const float remaining = std::fabs(g.twist);
		const float brake = std::sqrt(2.0f * tc.catchUpAccel * remaining);
		float v = g.catchVel + tc.catchUpAccel * (float)dt;
		if (v > tc.catchUpSpeed) v = tc.catchUpSpeed;
		if (v > brake) v = brake;
		g.catchVel = v;
		const float step = v * (float)dt;
		if (remaining <= step || remaining < 0.2f * kDegToRad) {
			g.twist = 0.0f;
			g.catchingUp = false;
			g.catchVel = 0.0f;
		} else {
			g.twist -= (g.twist > 0.0f ? step : -step);
		}
	}
	// Never twist further than this, even if the body turns faster than the legs catch up.
	if (g.twist > tc.maxTwist) g.twist = tc.maxTwist;
	if (g.twist < -tc.maxTwist) g.twist = -tc.maxTwist;
}

// Free look: with the weapon holstered and standing still, the game still turns the player (so
// the camera, aiming and the W direction are vanilla), but the whole skeleton is turned back by
// the same amount so the body stays where it was. Otherwise the offset decays and the body turns
// to face the camera; while standing, the legs stay planted during that turn (the offset moves
// into the turn-in-place twist, which then catches up with the turn animation).
static const char kBlockMoving[] = "moving";

static const char* FreeLookBlocker(uint32_t flags)
{
	if (!g_cfg.look.enabled) return "disabled";
	if (!g_stateChecksOk) return "game check failed";
	if (!g.body) return "no skeleton";
	if (!InThirdPerson()) return "first person";
	if (*(bool*)addr::kVanityCamera) return "vanity camera";
	if (PlayerWeaponOut()) return "weapon out";
	if (PlayerSitSleepState()) return "sitting/sleeping";
	const uint32_t knock = PlayerKnockState();
	if (knock >= 1 && knock <= 4) return "knocked down";
	// Anything other than turn/walk/run/sneak bits (and 0x40, always set): jumping, falling,
	// swimming...
	if (flags & ~(uint32_t)(kMove_Direction | kMove_Turn | 0x40 | 0x100 | 0x200 | 0x400)) return "movement state";
	if (flags & kMove_Direction) return kBlockMoving;  // checked last: torso lag uses it
	return nullptr;
}

static void UpdateFreeLook(uint32_t flags, double dt, float dHeading)
{
	const char* blocker = FreeLookBlocker(flags);
	const bool on = blocker == nullptr;
	if (g_cfg.debug && (on != g.freeLook || (!on && strcmp(blocker, g.freeLookBlocker) != 0)))
		Log("free look %s%s%s", on ? "on" : "off", on ? "" : ": ", on ? "" : blocker);
	g.freeLook = on;
	g.freeLookBlocker = on ? "" : blocker;

	g.holsteredMoving = blocker == kBlockMoving;
	if (on) {
		g.torsoLag = false;
		g.bodyYaw = WrapPi(g.bodyYaw + dHeading);
		return;
	}

	// Moving with the weapon holstered: the whole body (legs and torso) trails behind turns like
	// the legs do, while the head keeps looking where the camera points (see UpdateHead).
	const MoveConfig& mc = g_cfg.move;
	g.torsoLag = g.holsteredMoving && mc.torsoLag;
	if (g.torsoLag) {
		const float limit = std::fabs(g.bodyYaw) > mc.legLagMax ? std::fabs(g.bodyYaw) : mc.legLagMax;
		g.bodyYaw += dHeading;
		if (g.bodyYaw > limit) g.bodyYaw = limit;
		if (g.bodyYaw < -limit) g.bodyYaw = -limit;
		const bool leftover = std::fabs(g.bodyYaw) > mc.legLagMax;  // from free look: line up quickly
		g.bodyYaw = Approach(g.bodyYaw, 0.0f, dt, leftover ? g_cfg.look.alignTime : mc.legLagTime);
		if (std::fabs(g.bodyYaw) < 1e-4f) g.bodyYaw = 0.0f;
		return;
	}
	if (g.bodyYaw == 0.0f) return;
	const float before = g.bodyYaw;
	g.bodyYaw = Approach(g.bodyYaw, 0.0f, dt, g_cfg.look.alignTime);
	if (std::fabs(g.bodyYaw) < 1e-4f) g.bodyYaw = 0.0f;
	// Standing (e.g. drawing the weapon): the torso turns to the camera, the feet stay planted.
	if (g_cfg.turn.enabled && !(flags & kMove_Direction) && g.body && g.legs)
		g.twist += before - g.bodyYaw;
}

// Head look: while free looking, the head follows the camera (relative yaw = -bodyYaw, pitch from
// the player's look pitch) until the camera is more than fHeadMaxYaw to the side; then it looks
// straight ahead again until the camera comes back within 5 degrees of that limit.
static void UpdateHead(double dt)
{
	const FreeLookConfig& lc = g_cfg.look;
	const bool active = g.freeLook || g.holsteredMoving;
	const float rel = active ? -BodyYawTotal() : 0.0f;  // camera direction relative to the body
	if (g.headLooking && std::fabs(rel) > lc.headMaxYaw)
		g.headLooking = false;
	else if (!g.headLooking && std::fabs(rel) < lc.headMaxYaw - 5.0f * kDegToRad)
		g.headLooking = true;

	// The head angle itself is smoothed (fHeadTurnTimeMs). On fast turns this lets the head be
	// carried a little with the body before it settles on the crosshair, which looks more natural
	// than locking it exactly (tried, 2026-10-09).
	float yaw = 0.0f, pitch = 0.0f;
	if (active && g.headLooking) {
		yaw = rel;
		// rotX is positive when looking down.
		pitch = -PlayerPitch();
		if (pitch > lc.headMaxUp) pitch = lc.headMaxUp;
		if (pitch < -lc.headMaxDown) pitch = -lc.headMaxDown;
	}
	g.headYaw = Approach(g.headYaw, yaw, dt, lc.headTime);
	g.headPitch = Approach(g.headPitch, pitch, dt, lc.headTime);
	if (std::fabs(g.headYaw) < 1e-4f && yaw == 0.0f) g.headYaw = 0.0f;
	if (std::fabs(g.headPitch) < 1e-4f && pitch == 0.0f) g.headPitch = 0.0f;
}

// Lean into turns while running: roll the whole body toward the side you turn to, in proportion
// to the (smoothed) turn speed.
static void UpdateLean(uint32_t flags, double dt, float dHeading)
{
	const MoveConfig& mc = g_cfg.move;
	float target = 0.0f;
	const bool running = (flags & kMove_Direction) && (flags & 0x200) && !(flags & 0x400);
	if (mc.lean && running && g.body) {
		if (dt > 0.001) {
			// Turn speed in rad/s, smoothed so a jittery mouse gives a steady lean.
			g.turnRate = Approach(g.turnRate, dHeading / (float)dt, dt, 0.08f);
		}
		target = g.turnRate * mc.leanPerTurn;   // + = turning right = lean right
		if (target > mc.leanMax) target = mc.leanMax;
		if (target < -mc.leanMax) target = -mc.leanMax;
	} else {
		g.turnRate = 0.0f;
	}
	g.lean = Approach(g.lean, target, dt, mc.leanTime);
	if (std::fabs(g.lean) < 1e-4f && target == 0.0f) g.lean = 0.0f;
}

// Optional (bDisableFootIKWhenTurned): pause the game's foot IK while the body or legs are turned,
// restoring the player's own setting afterwards.
static void UpdateFootIK()
{
	uint8_t* pc = (uint8_t*)Player();
	uint8_t* rc = pc ? *(uint8_t**)(pc + off::kRagdollCtrl) : nullptr;
	if (!rc || *(uintptr_t*)rc != addr::kRagdollCtrlVtbl) { g.footIKCtrl = nullptr; return; }
	if (rc != g.footIKCtrl) { g.footIKCtrl = rc; g.footIKPaused = false; }  // rebuilt
	const float limit = 2.0f * kDegToRad;
	const bool turned = g_cfg.footIKOff &&
	                    (std::fabs(BodyYawTotal()) > limit || std::fabs(g.angle) > limit);
	bool& enabled = *(bool*)(rc + off::kFootIKEnabled);
	if (turned && !g.footIKPaused) {
		g.footIKSaved = enabled;
		enabled = false;
		g.footIKPaused = true;
	} else if (!turned && g.footIKPaused) {
		enabled = g.footIKSaved;
		g.footIKPaused = false;
	}
}

static void UpdateAngle()
{
	LARGE_INTEGER li;
	QueryPerformanceCounter(&li);
	double dt = g.lastQpc ? (double)(li.QuadPart - g.lastQpc) / g_qpcFreq : 0.0;
	g.lastQpc = li.QuadPart;
	if (dt > 0.25) dt = 0.25;

	// Ragdoll: drop every turn so all Apply* functions leave the bones alone, and start fresh
	// (no carried-over heading change) once the player is back on their feet.
	const char* ragdoll = PlayerRagdollReason();
	if (ragdoll || g.ragdoll) {
		const bool on = ragdoll != nullptr;
		if (g_cfg.debug && on != g.ragdoll) {
			if (on) Log("ragdoll (%s): skeleton left alone", ragdoll);
			else Log("ragdoll over");
		}
		g.ragdoll = on;
		if (on) {
			g.diagonal = g.diagonalVel = g.twist = g.angle = g.applied = 0.0f;
			g.bodyYaw = g.bodyDiag = g.bodyDiagVel = 0.0f;
			g.headYaw = g.headPitch = g.lean = g.turnRate = 0.0f;
			g.catchingUp = false;
			g.catchVel = 0.0f;
			g.stillTime = 0.0;
			g.freeLook = g.torsoLag = g.holsteredMoving = false;
			g.freeLookBlocker = "";
			g.haveHeading = false;
			if (g.footIKPaused) UpdateFootIK();  // give the game its foot IK setting back
			return;
		}
	}

	uint32_t flags = PlayerMoveFlags();
	if (g.strafeKeys && (flags & kMove_Direction) == kMove_Forward)
		flags = (flags & ~kMove_Direction) | g.strafeKeys;  // the real keys (see Hook_SetMovementFlags)
	if (g_cfg.debug && flags != g.lastFlags)
		Log("move flags %08X -> %08X", g.lastFlags, flags);
	g.lastFlags = flags;

	// Heading change since the last update. Body turning right = heading up; keeping a bone's
	// world facing then needs a positive (left) yaw of the same size.
	float dHeading = 0.0f;
	const float heading = PlayerHeading();
	if (std::isfinite(heading)) {
		dHeading = g.haveHeading ? WrapPi(heading - g.lastHeading) : 0.0f;
		g.lastHeading = heading;
		g.haveHeading = true;
		if (dHeading != 0.0f) g.lastDHeading = dHeading;
	} else {
		g.haveHeading = false;
	}
	// A large jump in one frame is a teleport, load, furniture or VATS snap, not a turn.
	if (std::fabs(dHeading) > 90.0f * kDegToRad) {
		dHeading = 0.0f;
		g.twist = g.bodyYaw = 0.0f;
		g.catchingUp = false;
	}

	UpdateFreeLook(flags, dt, dHeading);

	// Diagonals. Weapon holstered: the whole body turns (the head keeps looking at the camera).
	// Forward diagonals face the direction of travel; backward diagonals turn the other way so the
	// backward walk/run carries you back-left / back-right. With the weapon drawn only the legs
	// turn, so aim is unaffected. Both are springs, so switching between them blends smoothly.
	const float diagTarget = g_cfg.diagonals ? TargetAngle(flags) : 0.0f;
	const bool wholeBody = g_cfg.wholeBodyDiagonal && g.holsteredMoving && diagTarget != 0.0f;
	const float legsTarget = wholeBody ? 0.0f : diagTarget;
	const float bodyTarget = wholeBody ? diagTarget : 0.0f;
	g.diagonal = SmoothDamp(g.diagonal, legsTarget, &g.diagonalVel, dt, g_cfg.turnTime);
	if (std::fabs(g.diagonal) < 1e-4f && legsTarget == 0.0f && std::fabs(g.diagonalVel) < 1e-3f)
		g.diagonal = g.diagonalVel = 0.0f;
	if (g.freeLook) {
		// Stopped: keep facing where the body ended up (free look holds it there).
		g.bodyYaw = WrapPi(g.bodyYaw + g.bodyDiag);
		g.bodyDiag = g.bodyDiagVel = 0.0f;
	} else {
		g.bodyDiag = SmoothDamp(g.bodyDiag, bodyTarget, &g.bodyDiagVel, dt, g_cfg.turnTime);
		if (std::fabs(g.bodyDiag) < 1e-4f && bodyTarget == 0.0f && std::fabs(g.bodyDiagVel) < 1e-3f)
			g.bodyDiag = g.bodyDiagVel = 0.0f;
	}

	UpdateTwist(flags, dt, g.freeLook ? 0.0f : dHeading);
	UpdateHead(dt);
	UpdateLean(flags, dt, dHeading);

	g.angle = g.diagonal + g.twist;
	if (g_cfg.footIKOff || g.footIKPaused) UpdateFootIK();

	if (g_cfg.debug && (g.freeLook || g.bodyYaw != 0.0f || g.lean != 0.0f || g.twist != 0.0f)) {
		g.debugTimer += dt;
		if (g.debugTimer >= 0.5) {
			g.debugTimer = 0.0;
			Log("body %+.1f head yaw %+.1f pitch %+.1f (look pitch %+.1f) legs %+.1f (twist %+.1f) "
			    "lean %+.1f turn %+.0f deg/s",
			    g.bodyYaw * kRadToDeg, g.headYaw * kRadToDeg, g.headPitch * kRadToDeg,
			    PlayerPitch() * kRadToDeg, g.angle * kRadToDeg, g.twist * kRadToDeg,
			    g.lean * kRadToDeg, g.turnRate * kRadToDeg);
		}
	}
}

// out = Rz(a) * in, rotating about the world up axis (Z). Positive a turns left (counter-clockwise
// seen from above).
static void RotateMatrix(NiMatrix3& m, float c, float s)
{
	for (int col = 0; col < 3; col++) {
		const float x = m.m[0][col], y = m.m[1][col];
		m.m[0][col] = c * x - s * y;
		m.m[1][col] = s * x + c * y;
	}
}

static void RotatePointAbout(NiPoint3& p, const NiPoint3& pivot, float c, float s)
{
	const float x = p.x - pivot.x, y = p.y - pivot.y;
	p.x = pivot.x + c * x - s * y;
	p.y = pivot.y + s * x + c * y;
}

// m = R * m for a 3x3 rotation R.
static void PreMultiply(NiMatrix3& m, const float r[3][3])
{
	NiMatrix3 o = m;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			m.m[i][j] = r[i][0] * o.m[0][j] + r[i][1] * o.m[1][j] + r[i][2] * o.m[2][j];
}

// Rotation by angle a about the unit axis (x, y, z) (Rodrigues).
static void AxisAngle(float r[3][3], float x, float y, float z, float a)
{
	const float c = std::cos(a), s = std::sin(a), t = 1.0f - c;
	r[0][0] = c + x * x * t;     r[0][1] = x * y * t - z * s; r[0][2] = x * z * t + y * s;
	r[1][0] = y * x * t + z * s; r[1][1] = c + y * y * t;     r[1][2] = y * z * t - x * s;
	r[2][0] = z * x * t - y * s; r[2][1] = z * y * t + x * s; r[2][2] = c + z * z * t;
}

static void ApplyBody(NiAVObject* body)
{
	UpdateAngle();
	const float yaw = BodyYawTotal();
	if (yaw != 0.0f)
		RotateMatrix(body->world.rot, std::cos(yaw), std::sin(yaw));
	if (g.lean != 0.0f) {
		// Roll about the body's forward axis through the player's position (the feet), so the
		// whole body tilts instead of the feet swinging out.
		uint8_t* pc = (uint8_t*)Player();
		if (!pc) return;
		const NiPoint3 pivot = *(NiPoint3*)(pc + off::kRefrPos);
		const float h = PlayerHeading() - yaw;
		float r[3][3];
		AxisAngle(r, std::sin(h), std::cos(h), 0.0f, g.lean);  // + about forward = lean right
		PreMultiply(body->world.rot, r);
		NiPoint3& p = body->world.pos;
		const float x = p.x - pivot.x, y = p.y - pivot.y, z = p.z - pivot.z;
		p.x = pivot.x + r[0][0] * x + r[0][1] * y + r[0][2] * z;
		p.y = pivot.y + r[1][0] * x + r[1][1] * y + r[1][2] * z;
		p.z = pivot.z + r[2][0] * x + r[2][1] * y + r[2][2] * z;
	}
}

// Turns a neck/head bone by `share` of the head look, about its own position: yaw about the
// world up axis, then pitch about the horizontal axis to the right of the look direction.
static void ApplyHead(NiAVObject* bone, float share)
{
	if (g.headYaw == 0.0f && g.headPitch == 0.0f) return;
	if (g.headYaw != 0.0f) {
		const float a = g.headYaw * share;
		RotateMatrix(bone->world.rot, std::cos(a), std::sin(a));
	}
	if (g.headPitch != 0.0f) {
		// Look direction: heading h faces (sin h, cos h) in world space (0 = +Y, turning right
		// = toward +X), turned left by the body and head yaw.
		const float h = PlayerHeading() - (BodyYawTotal() + g.headYaw);
		const float fx = std::sin(h), fy = std::cos(h);
		float r[3][3];
		AxisAngle(r, fy, -fx, 0.0f, g.headPitch * share);  // right = (fy, -fx, 0); + = look up
		PreMultiply(bone->world.rot, r);
	}
}

static void ApplyLegs(NiAVObject* legs)
{
	if (!g.body) UpdateAngle();  // otherwise already updated by ApplyBody in this pass
	g.applied = g.angle;
	if (g.applied == 0.0f) return;
	RotateMatrix(legs->world.rot, std::cos(g.applied), std::sin(g.applied));
	g.pivot = legs->world.pos;
}

// Waist twist shares (vanilla skeleton, spine beside the legs): of the legs' turn, Bip01 Spine
// takes fWaistTwist, Spine1 half of that, Spine2 (chest, arms, weapon) only fUpperBodyTurn. So the
// twist is spread over the belly instead of happening all at the hips.
static float WaistShare(int bone)  // 0 = Spine, 1 = Spine1, 2 = Spine2
{
	const float ut = g_cfg.upperBodyTurn, w = g_cfg.waistTwist;
	float s = bone == 0 ? w : bone == 1 ? w * 0.5f : 0.0f;
	return s > ut ? s : ut;
}

static void ApplyUpper(NiAVObject* upper)
{
	if (g.applied == 0.0f) return;
	if (g.upperIsChild) {
		// Below the legs it inherited the full turn: undo all but the fUpperBodyTurn share.
		const float a = -g.applied * (1.0f - g_cfg.upperBodyTurn);
		const float c = std::cos(a), s = std::sin(a);
		RotateMatrix(upper->world.rot, c, s);
		RotatePointAbout(upper->world.pos, g.pivot, c, s);
		return;
	}
	// Beside the legs: turn by its share about its own position.
	const float a = g.applied * WaistShare(0);
	if (a != 0.0f) RotateMatrix(upper->world.rot, std::cos(a), std::sin(a));
}

// Spine1 / Spine2: they inherited the previous bone's share; add the difference to their own.
static void ApplySpine(NiAVObject* bone, int index)
{
	if (g.applied == 0.0f) return;
	const float a = g.applied * (WaistShare(index) - WaistShare(index - 1));
	if (a != 0.0f) RotateMatrix(bone->world.rot, std::cos(a), std::sin(a));
}

// ---------------------------------------------------------------------------------------------
// SetMovementFlags hook (PlayerMover vtable slot)
//
// Vanilla: any mouse movement while standing still sets a turn bit, which swaps the idle for the
// turn animation for at least 0.1 s, so the pose pops back and forth as you look around. With
// turn in place the feet are planted while you turn, so the turn bits are removed; while the legs
// catch up, the bit for their direction is set instead, so the game plays its turn animation
// (stepping feet) while they pivot.

typedef void(__thiscall* SetMovementFlags_t)(void*, uint32_t);
static SetMovementFlags_t g_origSetMovementFlags;
static bool g_strafeMoveHooked;  // Hook_AnimMoveVector installed (strafe-as-run needs it)

// Acceleration: the movement vector handed to Actor::Move (player's local frame, x right,
// y forward). Turned into a velocity with the game's frame delta and eased toward the
// target with a critically damped spring, so starting, stopping and walk/run changes take about
// fAccelerationMs / fDecelerationMs instead of being instant. Works in first and third person.
static struct MoveSmooth {
	NiPoint3 vel = {}, rate = {};
} mv;

static void SmoothMovement(NiPoint3& d)
{
	const MoveConfig& mc = g_cfg.move;
	const float dt = *(float*)addr::kFrameDelta;
	if (!(dt > 0.0f && dt < 0.25f) || g.ragdoll) {  // paused, hitch/load or ragdoll: start fresh
		mv = MoveSmooth();
		return;
	}
	const float tx = d.x / dt, ty = d.y / dt;
	const float target = std::sqrt(tx * tx + ty * ty);
	const float current = std::sqrt(mv.vel.x * mv.vel.x + mv.vel.y * mv.vel.y);
	const float time = target < current ? mc.decelTime : mc.accelTime;
	mv.vel.x = SmoothDamp(mv.vel.x, tx, &mv.rate.x, dt, time);
	mv.vel.y = SmoothDamp(mv.vel.y, ty, &mv.rate.y, dt, time);
	if (target == 0.0f && mv.vel.x * mv.vel.x + mv.vel.y * mv.vel.y < 1.0f) mv = MoveSmooth();
	d.x = mv.vel.x * dt;
	d.y = mv.vel.y * dt;  // z (root motion height) is left as is
}

// The forward run's root motion points forward; while a strafe is played as a forward run, turn
// it 90 degrees so you still move sideways (at the forward walk/run speed).
typedef bool(__thiscall* AnimMoveVector_t)(void*, NiPoint3*, void*, int, int);
static bool __fastcall Hook_AnimMoveVector(void* animData, void* /*edx*/, NiPoint3* out, void* actor,
                                           int a, int b)
{
	const bool r = ((AnimMoveVector_t)addr::kAnimMoveVector)(animData, out, actor, a, b);
	if (g.strafeKeys && out) {
		const float x = out->x, y = out->y;
		if (g.strafeKeys == kMove_Left) { out->x = -y; out->y = x; }   // forward -> left (-x)
		else                            { out->x = y;  out->y = -x; }  // forward -> right (+x)
	}
	return r;
}

// Actor::Move (0x709320, vtable +0x250, thiscall (float, NiPoint3* move, UInt32 flags), ret 0Ch):
// the final hand-off of the player's movement, after PlayerMover's diagonal correction (which
// rebuilds the vector from scratch, so smoothing earlier missed diagonals). Patched in the
// PlayerCharacter vtable only, and only the call from PlayerMover's update is smoothed.
typedef uint32_t(__thiscall* ActorMove_t)(void*, float, NiPoint3*, uint32_t);
static uint32_t __fastcall Hook_PlayerMove(void* actor, void* /*edx*/, float f, NiPoint3* move, uint32_t flags)
{
	if (g_cfg.move.accel && move && __builtin_return_address(0) == (void*)addr::kPlayerMoverMoveRet)
		SmoothMovement(*move);
	return ((ActorMove_t)addr::kActorMove)(actor, f, move, flags);
}

static void __fastcall Hook_SetMovementFlags(void* mover, void* /*edx*/, uint32_t flags)
{
	// Analog stick: with movement flag 0x40 clear, PlayerMover moves along its input vector
	// (+0x88 x right, +0x8C y forward) instead of the animation's direction, and the direction
	// flags only pick the animation (forward/back/left/right by the dominant axis). So the exact
	// stick angle is read here, the forward or backward animation is chosen (forward up to 125
	// degrees, back to forward below 100, so straight sideways is always forward and hovering
	// doesn't flicker), and the legs/body are turned by the exact angle (TargetAngle). Keyboard
	// (0x40 set) keeps its 8 directions.
	if (PlayerRagdollReason()) {  // dead/knocked down: the game's own flags, untouched
		g.analog = false;
		g.strafeKeys = 0;
		g_origSetMovementFlags(mover, flags);
		return;
	}
	const uint32_t dirIn = flags & kMove_Direction;
	const bool wasAnalog = g.analog;
	g.analog = false;
	if (g_cfg.analog360 && g.legs && dirIn && !(flags & kMove_Keyboard) && InThirdPerson()) {
		const float x = *(float*)((uint8_t*)mover + off::kMoverInputX);
		const float y = *(float*)((uint8_t*)mover + off::kMoverInputY);
		if (x * x + y * y > 0.0025f) {
			const float phi = std::atan2(-x, y);  // + = left
			const float a = std::fabs(phi);
			// Forward zone extends well behind the side (straight left/right must stay forward).
			if (!g.analogBack && a > 125.0f * kDegToRad) g.analogBack = true;
			else if (g.analogBack && a < 100.0f * kDegToRad) g.analogBack = false;
			g.analog = true;
			g.analogAngle = phi;
			flags = (flags & ~kMove_Direction) | (g.analogBack ? kMove_Backward : kMove_Forward);
		}
		if (g_cfg.debug) {
			static int64_t lastLog;
			const int64_t now = QpcNow();
			if ((double)(now - lastLog) / g_qpcFreq > 0.25) {
				lastLog = now;
				Log("analog: input (%.2f, %.2f) flags %X -> %X angle %+.0f %s", x, y, dirIn,
				    flags & kMove_Direction, g.analogAngle * kRadToDeg, g.analogBack ? "back" : "fwd");
			}
		}
	}
	if (wasAnalog && !g.analog && g_cfg.debug) Log("analog: off (flags %08X)", flags);

	// Weapon drawn, pure strafe: tell the game "forward" so it plays the forward walk/run instead
	// of the strafe animation; the legs are then turned 90 degrees (TargetAngle) and the movement
	// vector is turned to match (Hook_AnimMoveVector). The real keys are kept in g.strafeKeys so
	// the rest of STRIDE still sees a strafe.
	const uint32_t dir = flags & kMove_Direction;
	const bool pureStrafe = (dir == kMove_Left || dir == kMove_Right);
	g.strafeKeys = 0;
	if (!g.analog && g_cfg.strafeAsRun && g_strafeMoveHooked && pureStrafe && g.legs && g_stateChecksOk &&
	    InThirdPerson() && PlayerWeaponOut()) {
		g.strafeKeys = dir;
		flags = (flags & ~kMove_Direction) | kMove_Forward;
	}

	const TurnConfig& tc = g_cfg.turn;
	const bool turnAnim = tc.enabled && tc.turnAnim && g.legs;
	if ((turnAnim || g.freeLook) && !(flags & kMove_Direction) && InThirdPerson()) {
		// Free look: the body does not turn at all, so no turn animation either.
		const uint32_t in = flags & kMove_Turn;
		flags &= ~kMove_Turn;
		if (turnAnim && g.catchingUp && g.twist != 0.0f) {
			// twist > 0: legs are left of the body and pivot right to catch up.
			flags |= (g.twist > 0.0f) ? kMove_TurnRight : kMove_TurnLeft;
		}
		if (g_cfg.debug && (in | (flags & kMove_Turn)) != g.lastTurnBits) {
			Log("turn bits: game %02X -> sent %02X (twist %.1f deg, heading delta %+.2f deg)", in,
			    flags & kMove_Turn, g.twist * kRadToDeg, g.lastDHeading * kRadToDeg);
			g.lastTurnBits = in | (flags & kMove_Turn);
		}
	}
	g_origSetMovementFlags(mover, flags);
}

// ---------------------------------------------------------------------------------------------
// UpdateWorldData hook (NiNode vtable slot)

typedef void(__thiscall* UpdateWorldData_t)(NiAVObject*, void*);
static UpdateWorldData_t g_origUpdateWorldData;

// ---------------------------------------------------------------------------------------------
// First person gun sway: the arms and gun lag a little behind the view when looking around and
// float back (as in Half-Life 2). The first person camera belongs to the first person skeleton
// (player+0x5F0, a node at the feet turned with the heading), so turning its root would turn the
// view too. Instead both arms (the clavicles; the gun is in the right hand) get the same small
// rotation about the head right after the game places them each frame, so they move as one piece
// and the view stays put. Purely visual: shots fly along the crosshair line. Fades out when aiming down the
// sights (the view zooms in), in menus and VATS, and with the weapon holstered.

// Aiming down the sights: the player's process keeps it at +0x228 (HighProcess: SetAiming 0x3F8,
// which refuses in VATS, stores it via 0x3FC; IsAiming 0x400 = 0x73B3D0 reads it).
static bool PlayerAiming()
{
	void* proc = PlayerProcess();
	return proc && VCall<bool>(proc, vslot::kProcessIsAiming);
}

static struct SwayState {
	bool    init = false;
	float   lastH = 0.0f, lastP = 0.0f;   // view heading/pitch at the last frame
	float   lagH = 0.0f, lagP = 0.0f;      // how far the gun trails the view (radians)
	float   velH = 0.0f, velP = 0.0f;
	float   weight = 0.0f;                 // 0..1: faded out when aiming, in menus...
	int64_t lastQpc = 0;
	bool    seen = false;
	float   oh = 0.0f, op = 0.0f;          // the offset applied this frame (radians)
	float   pitch = 0.0f;                  // view pitch this frame (+ = down)
	float   dip = 0.0f, dipVel = 0.0f;     // jump/land dip: units down, springing back
	int     airState = 0;                  // character controller: 0 ground, 1 jumping, 2 in the air
	float   airTime = 0.0f;                // seconds off the ground
	NiAVObject* root = nullptr;            // first person skeleton the nodes below were found in
	NiAVObject* head = nullptr;            // pivot
	NiAVObject* arms[2] = {};              // left and right clavicle
} g_sway;
static bool g_swayChecksOk;  // menu mode and VATS state functions matched at load

static NiAVObject* FirstPersonRoot()
{
	uint8_t* pc = (uint8_t*)Player();
	return pc ? *(NiAVObject**)(pc + off::kFirstPersonNode) : nullptr;
}

// Rotation by angle (radians) about a unit axis.
static NiMatrix3 AxisAngle(const NiPoint3& a, float angle)
{
	const float c = std::cos(angle), s = std::sin(angle), t = 1.0f - c;
	NiMatrix3 r;
	r.m[0][0] = c + t * a.x * a.x;       r.m[0][1] = t * a.x * a.y - s * a.z; r.m[0][2] = t * a.x * a.z + s * a.y;
	r.m[1][0] = t * a.x * a.y + s * a.z; r.m[1][1] = c + t * a.y * a.y;       r.m[1][2] = t * a.y * a.z - s * a.x;
	r.m[2][0] = t * a.x * a.z - s * a.y; r.m[2][1] = t * a.y * a.z + s * a.x; r.m[2][2] = c + t * a.z * a.z;
	return r;
}

static NiMatrix3 MatMul(const NiMatrix3& a, const NiMatrix3& b)
{
	NiMatrix3 r;
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
	return r;
}

// Finds the arms and the head in the first person skeleton (again when it is rebuilt).
static bool FindSwayNodes(NiAVObject* root)
{
	if (root == g_sway.root) return g_sway.arms[0] && g_sway.arms[1] && g_sway.head;
	g_sway.root = root;
	g_sway.head = FindByName(root, "Bip01 Head");
	g_sway.arms[0] = FindByName(root, "Bip01 L Clavicle");
	g_sway.arms[1] = FindByName(root, "Bip01 R Clavicle");
	Log("gun sway: first person skeleton %p: head %p, clavicles %p %p", (void*)root, (void*)g_sway.head,
	    (void*)g_sway.arms[0], (void*)g_sway.arms[1]);
	if (g_cfg.debug && root && !g_sway.seen) {
		g_sway.seen = true;
		Log("first person skeleton:");
		DumpTree(root, 1);
	}
	return g_sway.arms[0] && g_sway.arms[1] && g_sway.head;
}

// Advances the sway once a frame (the nodes may be updated more than once a frame).
static void UpdateGunSway()
{
	uint8_t* pc = (uint8_t*)Player();
	if (!pc) return;
	// Vanilla: the first person arms follow only fFirstPersonHandFollowMult (0.85) of the view's
	// pitch (player function 0x7757E0), so the gun shifts when looking up or down. Full follow:
	// the gun stays put on screen. Set every frame: the game setting may be reloaded.
	static const bool handFollowOk = *(const char* const*)0x00F5EA50 &&
	                                 strcmp(*(const char* const*)0x00F5EA50, "fFirstPersonHandFollowMult") == 0;
	if (g_cfg.sway.lockPitch && handFollowOk) *(float*)0x00F5EA4C = 1.0f;
	const GunSwayConfig& c = g_cfg.sway;
	const int64_t now = QpcNow();
	double dt = g_sway.lastQpc ? (double)(now - g_sway.lastQpc) / g_qpcFreq : 0.0;
	const float h = *(float*)(pc + off::kRefrRotZ), p = *(float*)(pc + off::kRefrRotX);
	if (!g_sway.init || dt > 0.0005) {
		g_sway.lastQpc = now;
		// Not updated for a while (third person, menus, loading): start fresh.
		if (dt > 0.25) g_sway.init = false;
		if (dt > 0.1) dt = 0.1;
		if (!g_sway.init) {
			g_sway.lastH = h;
			g_sway.lastP = p;
			g_sway.lagH = g_sway.lagP = g_sway.velH = g_sway.velP = 0.0f;
			g_sway.dip = g_sway.dipVel = 0.0f;
			g_sway.init = true;
		}
		const float dH = WrapPi(h - g_sway.lastH), dP = p - g_sway.lastP;
		g_sway.lastH = h;
		g_sway.lastP = p;
		if (std::fabs(dH) > 1.5f || std::fabs(dP) > 1.5f) {  // teleport, load, view snap: start fresh
			g_sway.lagH = g_sway.lagP = g_sway.velH = g_sway.velP = 0.0f;
		} else {
			// The view turned, the gun hasn't yet. The lag is the gun's offset itself; the further
			// out it already is, the less more turning adds (none at the limit), so it eases into
			// the limit and starts back as soon as the turn ends instead of resting against it.
			const float mx = c.maxDeg * kDegToRad;
			auto push = [mx](float lag, float in) {
				if (mx <= 0.0f) return 0.0f;
				if (lag * in > 0.0f) {  // outward
					const float room = std::fmax(0.0f, 1.0f - std::fabs(lag) / mx);
					in *= room * room;
				}
				return lag + in;
			};
			g_sway.lagH = push(g_sway.lagH, -dH * c.amount);
			g_sway.lagP = push(g_sway.lagP, -dP * c.amount);
		}
		g_sway.lagH = SmoothDamp(g_sway.lagH, 0.0f, &g_sway.velH, dt, c.time);
		g_sway.lagP = SmoothDamp(g_sway.lagP, 0.0f, &g_sway.velP, dt, c.time);

		// Jump and land: the arms dip and spring back (instead of the first person jump animations).
		if (c.smoothJump) {
			void* proc = PlayerProcess();
			uint8_t* cc = proc ? VCall<uint8_t*>(proc, vslot::kProcessCharController) : nullptr;
			const int state = cc ? (int)*(uint32_t*)(cc + 0x270) : 0;
			constexpr float kW = 2.0f * kPi / 0.45f;  // spring: about 0.45 s per bounce
			if (state != g_sway.airState) {
				if (g_cfg.debug) Log("gun sway: jump state %d -> %d (%.2f s in the air)", g_sway.airState, state, g_sway.airTime);
				if (g_sway.airState == 0 && state == 1)          // took off
					g_sway.dipVel += c.jumpDip * kW * 1.6f;
				else if (g_sway.airState != 0 && state == 0)     // landed: harder the longer the fall
					g_sway.dipVel += c.landDip * kW * 1.6f * std::fmin(1.5f, std::fmax(0.3f, g_sway.airTime));
				if (state == 0) g_sway.airTime = 0.0f;
				g_sway.airState = state;
			}
			if (state != 0) g_sway.airTime += (float)dt;
			const float acc = -kW * kW * g_sway.dip - 2.0f * 0.55f * kW * g_sway.dipVel;
			g_sway.dipVel += acc * (float)dt;
			g_sway.dip += g_sway.dipVel * (float)dt;
		}

		// Fade: first person with the weapon out, not aiming down the sights, not in a menu or VATS.
		const bool active = !InThirdPerson() && PlayerWeaponOut() && g_swayChecksOk && !PlayerAiming() &&
		                    !((bool(__cdecl*)())addr::kIsMenuMode)() && *(int*)addr::kVATSMode == 0 &&
		                    !PlayerRagdollReason();
		g_sway.weight = Approach(g_sway.weight, active ? 1.0f : 0.0f, dt, 0.1f);

		g_sway.oh = g_sway.lagH * g_sway.weight;  // + = gun points right of the view
		g_sway.op = g_sway.lagP * g_sway.weight;  // + = gun points below the view
		g_sway.pitch = p;
		if (g_cfg.debug) {
			static int64_t lastLog;
			if ((double)(now - lastLog) / g_qpcFreq > 2.0) {
				lastLog = now;
				Log("gun sway: offset %+.2f right %+.2f down deg (weight %.2f)", g_sway.oh * kRadToDeg,
				    g_sway.op * kRadToDeg, g_sway.weight);
			}
		}
	}
}

// Turns one arm (with everything on it) about the head by this frame's offset, about the view's
// own up and right axes (the skeleton root's frame, +Y heading, +Z up, +X right, tilted by the
// view's pitch), so both arms move as one piece and the sway looks the same at any pitch.
static void ApplyGunSwayTo(NiAVObject* node)
{
	const float dip = g_sway.dip * g_sway.weight;
	const float oh = g_sway.oh, op = g_sway.op + dip * 0.6f * kDegToRad;  // dipping tilts the gun down a bit
	if (std::fabs(oh) < 1e-5f && std::fabs(op) < 1e-5f && std::fabs(dip) < 1e-3f) return;
	const NiMatrix3& B = g_sway.root->world.rot;
	const NiPoint3 bodyUp = { B.m[0][2], B.m[1][2], B.m[2][2] }, fwd = { B.m[0][1], B.m[1][1], B.m[2][1] };
	const NiPoint3 right = { B.m[0][0], B.m[1][0], B.m[2][0] };
	// looking down tips the view's up forward
	const float cp = std::cos(g_sway.pitch), sp = std::sin(g_sway.pitch);
	const NiPoint3 up = { bodyUp.x * cp + fwd.x * sp, bodyUp.y * cp + fwd.y * sp, bodyUp.z * cp + fwd.z * sp };
	// Heading to the right is clockwise about up; pitch down is about -right.
	const NiMatrix3 R = MatMul(AxisAngle(up, -oh), AxisAngle(right, -op));
	const NiPoint3 c = g_sway.head->world.pos;
	const NiPoint3 d = { node->world.pos.x - c.x, node->world.pos.y - c.y, node->world.pos.z - c.z };
	node->world.pos = { c.x + R.m[0][0] * d.x + R.m[0][1] * d.y + R.m[0][2] * d.z,
	                    c.y + R.m[1][0] * d.x + R.m[1][1] * d.y + R.m[1][2] * d.z,
	                    c.z + R.m[2][0] * d.x + R.m[2][1] * d.y + R.m[2][2] * d.z };
	node->world.rot = MatMul(R, node->world.rot);
	node->world.pos = { node->world.pos.x - up.x * dip, node->world.pos.y - up.y * dip, node->world.pos.z - up.z * dip };
}

// First person jump/land animations are skipped (the dip above replaces their shake): the hook
// at AnimData::PlayAnimGroup returns "nothing played" for jump groups on the first person
// skeleton's animation data.
extern "C" bool __cdecl STRIDE_SkipAnimGroup(uint8_t* animData, uint32_t groupId)
{
	if (!g_cfg.sway.enabled || !g_cfg.sway.smoothJump || !animData) return false;
	const uint32_t g = groupId & 0xFF;
	const bool jump = (g >= 168 && g <= 170) || (g >= 174 && g <= 177) || (g >= 179 && g <= 182);
	if (!jump) return false;
	NiAVObject* fp = FirstPersonRoot();
	const bool first = fp && *(NiAVObject**)(animData + 8) == fp;
	if (g_cfg.debug) {
		static int n;
		if (n++ < 12)
			Log("gun sway: jump animation group %u requested on %s skeleton%s", g,
			    first ? "the first person" : "another", first ? " - skipped" : "");
	}
	return first;
}

extern "C" void STRIDE_PlayAnimGroupStub();
__asm__(
	".text\n"
	".globl _STRIDE_PlayAnimGroupStub\n"
	"_STRIDE_PlayAnimGroupStub:\n"
	"	pushl %ecx\n"
	"	pushl 8(%esp)\n"            // group id
	"	pushl %ecx\n"               // animation data
	"	call _STRIDE_SkipAnimGroup\n"
	"	addl $8, %esp\n"
	"	popl %ecx\n"
	"	testb %al, %al\n"
	"	jnz 1f\n"
	"	pushl %ebx\n"               // the overwritten instructions
	"	movl 8(%esp), %ebx\n"
	"	pushl $0x00461695\n"
	"	ret\n"
	"1:\n"
	"	xorl %eax, %eax\n"          // nothing played
	"	ret $0x10\n");

static bool PatchVtableSlot(uintptr_t vtbl, uintptr_t slot, uintptr_t expect, void* fn);

static void __fastcall Hook_UpdateWorldData(NiAVObject* self, void* /*edx*/, void* updateData)
{
	g_origUpdateWorldData(self, updateData);

	if (g_cfg.sway.enabled) {
		NiAVObject* fp = FirstPersonRoot();
		if (fp && self == fp) {  // updated before its children: advance the sway for this frame
			if (FindSwayNodes(fp)) UpdateGunSway();
			return;
		}
		if (fp && fp == g_sway.root && (self == g_sway.arms[0] || self == g_sway.arms[1])) {
			ApplyGunSwayTo(self);
			return;
		}
		// The first person root's class may not be one of the hooked node classes: hook it too
		// (once a second check, same verification as the others).
		static int64_t lastCheck;
		static uintptr_t patchedVtbl;
		const int64_t now = QpcNow();
		if (fp && (double)(now - lastCheck) / g_qpcFreq > 1.0) {
			lastCheck = now;
			const uintptr_t vt = (uintptr_t)fp->vtbl;
			if (vt != addr::kNiNodeVtbl && vt != addr::kRootNodeVtbl && vt != patchedVtbl) {
				patchedVtbl = vt;
				if (PatchVtableSlot(vt, addr::kSlotUpdateWorldData, addr::kUpdateWorldData, (void*)&Hook_UpdateWorldData))
					Log("gun sway: hooked the first person root's UpdateWorldData (vtable %08X)", (unsigned)vt);
				else
					Log("WARNING: gun sway: first person root (vtable %08X) not hookable; no gun sway", (unsigned)vt);
			}
		}
	}

	if (self == g.body || self == g.legs || self == g.upper || self == g.neck || self == g.head ||
	    self == g.spine1 || self == g.spine2) {
		// If the player's 3D was rebuilt these pointers may be stale (and reused); skip until
		// the rescan below has run.
		if (PlayerRoot() != g.root) return;
		if (self == g.body) ApplyBody(self);
		else if (self == g.legs) ApplyLegs(self);
		else if (self == g.upper) ApplyUpper(self);
		else if (self == g.spine1) ApplySpine(self, 1);
		else if (self == g.spine2) ApplySpine(self, 2);
		else if (self == g.neck) ApplyHead(self, g_cfg.look.neckShare);
		else ApplyHead(self, 1.0f - g_cfg.look.neckShare);
	} else {
		// Notice when the player's 3D is (re)built: rescan from inside an update pass over that
		// skeleton, so the nodes are complete and it happens on the thread that animates it.
		NiAVObject* root = PlayerRoot();
		if (root && root != g.root) {
			int depth = 0;
			for (NiAVObject* p = self->parent; p && depth < 8; p = p->parent, depth++)
				if (p == root) { Rescan(root); break; }
		}
	}
}

// ---------------------------------------------------------------------------------------------
// Third person camera (hooked at the camera node's update call, after the game has computed and
// written the camera position, before it is finalized)
//
//   Follow smoothing: the camera position is split into the player's position (the point it
//   follows) plus the orbit offset. Only the followed point's height is smoothed, so stair/landing bobs
//   are softened while mouse rotation stays exact.
//   Shoulder swap: a key eases fOverShoulderPosX and the aim point beside the player
//   (fOverShoulderOffsetPointX) over to the other side.

struct GameSettingF { void* vtbl; float value; const char* name; };

static struct CamState {
	bool          init = false;
	NiPoint3      follow = {}, followVel = {};
	float         normalLat = 0.0f;   // player's sideways view offset in normal play (vanity fix)
	bool          haveLat = false;
	bool          parentChecked = false, parentOk = false;
	GameSettingF* shoulderX = nullptr;
	GameSettingF* aimX = nullptr;
	bool          settingsChecked = false;
	float         baseShoulderX = 0.0f, baseAimX = 0.0f;
	float         shoulder = 1.0f, shoulderVel = 0.0f, shoulderTarget = 1.0f;
	bool          keyDown = false;
	int64_t       lastQpc = 0;
} cam;

static GameSettingF* FindSetting(uintptr_t at, const char* name)
{
	GameSettingF* s = (GameSettingF*)at;
	const size_t len = strlen(name) + 1;
	if (!IsBadReadPtr(s->name, len) && memcmp(s->name, name, len) == 0) return s;
	Log("WARNING: setting %s not found at %08X; shoulder swap disabled", name, (unsigned)at);
	return nullptr;
}

static bool GameHasFocus()
{
	DWORD pid = 0;
	GetWindowThreadProcessId(GetForegroundWindow(), &pid);
	return pid == GetCurrentProcessId();
}

static void UpdateShoulder(double dt)
{
	const CameraConfig& cc = g_cfg.cam;
	if (!cam.settingsChecked) {
		// Game settings are built by static initializers after plugins load; check on first use.
		cam.settingsChecked = true;
		cam.shoulderX = FindSetting(addr::kSettingShoulderX, "fOverShoulderPosX");
		cam.aimX = cam.shoulderX ? FindSetting(addr::kSettingAimPointX, "fOverShoulderOffsetPointX") : nullptr;
		if (!cam.aimX) cam.shoulderX = nullptr;
		if (cam.shoulderX) {
			cam.baseShoulderX = cam.shoulderX->value;
			cam.baseAimX = cam.aimX->value;
			Log("shoulder swap ready (fOverShoulderPosX %.1f, fOverShoulderOffsetPointX %.1f)",
			    cam.baseShoulderX, cam.baseAimX);
		}
	}
	if (!cam.shoulderX) return;

	const bool down = cc.shoulderKey > 0 && cc.shoulderKey < 256 && GameHasFocus() &&
	                  (GetAsyncKeyState(cc.shoulderKey) & 0x8000);
	if (down && !cam.keyDown) {
		cam.shoulderTarget = -cam.shoulderTarget;
		if (g_cfg.debug) Log("shoulder swap -> %s", cam.shoulderTarget > 0 ? "right" : "left");
	}
	cam.keyDown = down;
	cam.shoulder = SmoothDamp(cam.shoulder, cam.shoulderTarget, &cam.shoulderVel, dt, cc.shoulderTime);
	cam.shoulderX->value = cam.baseShoulderX * cam.shoulder;
	cam.aimX->value = cam.baseAimX * cam.shoulder;
}

static float Length(const NiPoint3& v) { return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z); }

static void SmoothCamera(NiAVObject* node, double dt)
{
	const CameraConfig& cc = g_cfg.cam;
	if (!cam.parentChecked) {
		// The node's local position must be in world space (parent at the origin, unrotated).
		cam.parentChecked = true;
		const NiAVObject* p = node->parent;
		const NiTransform& w = p ? p->world : node->local;
		bool ok = !p || (std::fabs(w.scale - 1.0f) < 1e-3f && std::fabs(w.pos.x) < 1e-2f &&
		                 std::fabs(w.pos.y) < 1e-2f && std::fabs(w.pos.z) < 1e-2f);
		for (int i = 0; ok && p && i < 3; i++)
			for (int j = 0; j < 3; j++)
				if (std::fabs(w.rot.m[i][j] - (i == j ? 1.0f : 0.0f)) > 1e-3f) ok = false;
		cam.parentOk = ok;
		Log(ok ? "camera node %p: world space, smoothing active"
		       : "WARNING: camera node %p is not in world space; smoothing disabled", (void*)node);
	}
	if (!cam.parentOk) return;

	uint8_t* pc = (uint8_t*)Player();
	if (!pc) return;
	const NiPoint3 P = *(NiPoint3*)(pc + off::kRefrPos);
	NiPoint3& T = node->local.pos;

	const NiPoint3 lag = { cam.follow.x - P.x, cam.follow.y - P.y, cam.follow.z - P.z };
	if (!cam.init || Length(lag) > 300.0f) {  // first frame, teleport, load
		cam.init = true;
		cam.follow = P;
		cam.followVel = {};
	}
	// Only height is smoothed; horizontally the camera follows exactly (horizontal smoothing was
	// tried at 30 and 100 ms and dropped both times, 2026-10-09).
	cam.follow.x = P.x;
	cam.follow.y = P.y;
	cam.follow.z = SmoothDamp(cam.follow.z, P.z, &cam.followVel.z, dt, cc.pivotSmoothZ);
	NiPoint3 S = { T.x + cam.follow.x - P.x, T.y + cam.follow.y - P.y, T.z + cam.follow.z - P.z };

	// (Easing the camera back out after walls was tried three ways and dropped, 2026-10-10: vanilla
	// already slides it out linearly in ~0.1 s, and slowing that down didn't feel better.)

	// Vanity camera (hold POV key / idle): vanilla orbits around the over-the-shoulder point beside
	// the player, so after half a turn the player is on the other side of the crosshair. Keep the
	// player at the same sideways screen offset as in normal play by sliding the camera along its
	// own horizontal right axis (rotation untouched).
	if (cc.vanityFix) {
		NiPoint3 toP = { P.x - S.x, P.y - S.y, 0.0f };
		// Horizontal view direction: the camera rotation's column that best points at the player.
		const NiMatrix3& R = node->local.rot;
		float vx = 0.0f, vy = 0.0f, best = 0.0f;
		for (int col = 0; col < 3; col++) {
			const float cx = R.m[0][col], cy = R.m[1][col];
			const float dot = cx * toP.x + cy * toP.y;
			if (std::fabs(dot) > std::fabs(best)) { best = dot; vx = dot > 0 ? cx : -cx; vy = dot > 0 ? cy : -cy; }
		}
		const float len = std::sqrt(vx * vx + vy * vy);
		if (len > 0.1f) {
			const float rx = vy / len, ry = -vx / len;    // horizontal right of the view direction
			const float lat = toP.x * rx + toP.y * ry;   // player's sideways offset in view space
			if (!*(bool*)addr::kVanityCamera) {
				cam.normalLat = lat;
				cam.haveLat = true;
			} else if (cam.haveLat) {
				const float shift = lat - cam.normalLat;
				S.x += rx * shift;
				S.y += ry * shift;
			}
		}
	}
	T = S;
}

typedef void(__thiscall* NiAVObjectUpdate_t)(NiAVObject*, void*);
static void __fastcall Hook_CameraUpdate(NiAVObject* node, void* /*edx*/, void* updateData)
{
	LARGE_INTEGER li;
	QueryPerformanceCounter(&li);
	double dt = cam.lastQpc ? (double)(li.QuadPart - cam.lastQpc) / g_qpcFreq : 0.0;
	cam.lastQpc = li.QuadPart;
	if (dt > 0.25) { dt = 0.25; cam.init = false; }  // after a pause/load: start fresh

	if (g_cfg.cam.shoulderSwap) UpdateShoulder(dt);
	// Death/knockdown camera stays vanilla (it follows the ragdoll).
	if (node && InThirdPerson() && !PlayerRagdollReason()) SmoothCamera(node, dt);
	else cam.init = false;
	((NiAVObjectUpdate_t)addr::kNiAVObjectUpdate)(node, updateData);
}

// ---------------------------------------------------------------------------------------------
// Crosshair: four lines whose gap shows the gun's real spread cone (weapon spread and min spread,
// times the game's own spread multiplier: condition, skill, crippled limbs, iron sights, stance,
// movement and perks). Drawn with Direct3D just before each frame is presented; the vanilla
// crosshair is hidden while it shows (and restored to the player's setting otherwise).

static struct XhState {
	bool     visible = false;
	float    gap = 0.0f, gapVel = 0.0f;   // smoothed, pixels
	int64_t  lastQpc = 0;
	int      frustumOff = -1;             // NiCamera frustum offset, found at runtime (-2 = not found)
	bool     vanillaHidden = false;
	IDirect3DStateBlock9* sb = nullptr;
} xh;

struct WeapData {                         // TESObjectWEAP (FOSE GameForms.h)
	uint8_t pad[0xF4];
	uint8_t type;                         // F4  3..9 = guns
	uint8_t padF5[0x100 - 0xF5];
	uint8_t flags1;                       // 100 0x4 = has scope
	uint8_t pad101[0x104 - 0x101];
	float   minSpread;                    // 104 degrees
	float   spread;                       // 108 degrees
	uint32_t pad10C;
	float   sightFOV;                     // 110
};

static WeapData* PlayerGun()
{
	void* proc = PlayerProcess();
	if (!proc) return nullptr;
	uint8_t* entry = VCall<uint8_t*>(proc, 0x148);
	if (!entry) return nullptr;
	uint8_t* form = *(uint8_t**)(entry + 8);
	if (!form || form[4] != 0x28) return nullptr;  // kFormType_Weapon
	WeapData* w = (WeapData*)form;
	return (w->type >= 3 && w->type <= 9) ? w : nullptr;
}

// Right edge of the camera's view frustum (= tan of half the horizontal FOV), or 0 if unknown.
static float CameraFrustumRight()
{
	uint8_t* root = *(uint8_t**)addr::kCameraRoot;
	uint8_t* camera = root ? *(uint8_t**)(root + 0xAC) : nullptr;
	if (!camera || xh.frustumOff == -2) return 0.0f;
	auto plausible = [](const float* f) {
		// left, right, top, bottom, near, far
		return f[1] > 0.2f && f[1] < 4.0f && std::fabs(f[0] + f[1]) < 1e-3f && f[2] > 0.1f &&
		       f[2] < 4.0f && std::fabs(f[3] + f[2]) < 1e-3f && f[4] > 0.0f && f[5] > f[4];
	};
	if (xh.frustumOff < 0) {
		for (int off = 0x9C; off <= 0x140; off += 4)
			if (plausible((const float*)(camera + off))) { xh.frustumOff = off; break; }
		const float* f = xh.frustumOff >= 0 ? (const float*)(camera + xh.frustumOff) : nullptr;
		if (f) Log("crosshair: camera frustum at +0x%X (right %.3f = %.1f deg FOV)", xh.frustumOff, f[1],
		           2.0f * std::atan(f[1]) * kRadToDeg);
		else { Log("WARNING: camera frustum not found; crosshair uses a 75 degree FOV"); xh.frustumOff = -2; return 0.0f; }
	}
	const float* f = (const float*)(camera + xh.frustumOff);
	return plausible(f) ? f[1] : 0.0f;
}

// The vanilla crosshair is the HUD's crosshair tile (HUDMainMenu+0x12C). The game sets its
// "visible" value only when the HUD mode changes (closing a menu etc.), from the mode's element
// mask (+0x18C, bit 0x40) combined with the player's setting. While ours shows, the tile is kept
// hidden every frame; afterwards it gets back the visibility the game itself last asked for.
static void SetVanillaCrosshair(bool hide)
{
	uint8_t* hud = *(uint8_t**)addr::kHUDMainMenu;
	void* tile = hud ? *(void**)(hud + 0x12C) : nullptr;
	if (!tile) return;
	const float want = hide ? 0.0f : ((*(uint32_t*)(hud + 0x18C) & 0x40) ? 1.0f : 0.0f);
	const float cur = ((float(__thiscall*)(void*, uint32_t))addr::kTileGetFloat)(tile, kTileVisible);
	if (cur != want)
		((void(__thiscall*)(void*, uint32_t, float, bool))addr::kTileSetFloat)(tile, kTileVisible, want, true);
	xh.vanillaHidden = hide;
}

static float NonAttackWobbleMult()
{
	const GameSettingF* g = (const GameSettingF*)addr::kSettingNonAttackWobble;
	static int ok = -1;  // checked once: the setting object is where expected
	if (ok < 0) {
		ok = !IsBadReadPtr(g->name, 24) && memcmp(g->name, "fNonAttackGunWobbleMult", 24) == 0;
		Log("crosshair: fNonAttackGunWobbleMult %s %.3f", ok ? "=" : "NOT FOUND, using", ok ? g->value : 0.5f);
	}
	return ok ? g->value : 0.5f;
}

static const char* CrosshairHideReason(WeapData** wOut)
{
	if (!Player() || !PlayerRoot()) return "no player";
	if (!g_stateChecksOk) return "state checks failed";
	if (((bool(__cdecl*)())addr::kIsMenuMode)()) return "menu mode";
	if (!PlayerWeaponOut()) return "weapon not out";
	*wOut = PlayerGun();
	if (!*wOut) return "not a gun";
	return nullptr;
}

// Works out whether to draw and the target gap in pixels.
static bool CrosshairTarget(float width, float* gapOut)
{
	WeapData* w = nullptr;
	const char* why = CrosshairHideReason(&w);
	static const char* lastWhy = "start";
	if (g_cfg.debug && why != lastWhy) {
		Log("crosshair: %s", why ? why : "shown");
		lastWhy = why;
	}
	if (why) return false;
	float right = CameraFrustumRight();
	if (right <= 0.0f) right = std::tan(37.5f * kDegToRad);
	if (g_cfg.xh.hideScoped && (w->flags1 & 0x4) && w->sightFOV > 0.0f) {
		const float fov = 2.0f * std::atan(right) * kRadToDeg;
		if (std::fabs(fov - w->sightFOV) < 3.0f) return false;  // looking through the scope
	}
	const float mult = ((float(__thiscall*)(void*, int))addr::kGetWeaponStat)(Player(), 1);
	// The gun's sway: its wobble multiplier (skill, aiming, crouching, moving, condition, perks)
	// at the between-shots level (fNonAttackGunWobbleMult, as 0x778CC6 applies it), times how far
	// the gun sways at full wobble. The game's smoothed wobble (0xF6EA18) jumps to the full value
	// for a moment after each shot, which doesn't widen where the bullets go, so it isn't used; nor
	// is the barrel's position, which also follows the look up/down lag and reload animations.
	const float wobbleMult = ((float(__thiscall*)(void*, int))addr::kGetWeaponStat)(Player(), 2);
	const float sway = std::fmax(0.0f, wobbleMult * NonAttackWobbleMult()) * g_cfg.xh.wobbleDeg;
	float deg = w->spread * mult + w->minSpread + sway;
	if (!std::isfinite(deg) || deg < 0.0f) deg = 0.0f;
	if (deg > 45.0f) deg = 45.0f;
	*gapOut = std::tan(deg * kDegToRad) / right * (width * 0.5f) * g_cfg.xh.scale;
	if (g_cfg.debug) {
		static int64_t lastLog;
		const int64_t now = QpcNow();
		if ((double)(now - lastLog) / g_qpcFreq > 0.5) {
			lastLog = now;
			Log("crosshair: spread %.2f x condition %.3f + min %.2f + sway %.2f (wobble mult %.3f) = %.2f deg, "
			    "FOV %.1f, gap %.1f px", w->spread, mult, w->minSpread, sway, wobbleMult, deg,
			    2.0f * std::atan(right) * kRadToDeg, *gapOut);
		}
	}
	return true;
}

struct XhVertex { float x, y, z, rhw; D3DCOLOR c; };

static void FillRect(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1, D3DCOLOR c)
{
	x0 -= 0.5f; y0 -= 0.5f; x1 -= 0.5f; y1 -= 0.5f;  // pixel centres
	const XhVertex v[4] = { { x0, y0, 0, 1, c }, { x1, y0, 0, 1, c }, { x0, y1, 0, 1, c }, { x1, y1, 0, 1, c } };
	dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(XhVertex));
}

// ---------------------------------------------------------------------------------------------
// The game's own crosshair image (style 0), read from the interface texture atlas: loose files
// first (so UI mods' crosshairs are used), then Fallout - Textures.bsa.

// Minimal zlib inflate (RFC 1950/1951), after zlib's puff.c.
namespace inflate {
struct State {
	const uint8_t* src; size_t len, pos = 0;
	uint32_t buf = 0; int cnt = 0;
	std::vector<uint8_t>* out;
	bool err = false;
};
struct Huff { uint16_t count[16]; uint16_t symbol[288]; };

static int Bits(State& s, int n)
{
	while (s.cnt < n) {
		if (s.pos >= s.len) { s.err = true; return 0; }
		s.buf |= (uint32_t)s.src[s.pos++] << s.cnt;
		s.cnt += 8;
	}
	const int v = (int)(s.buf & ((1u << n) - 1));
	s.buf >>= n;
	s.cnt -= n;
	return v;
}

static void Build(Huff& h, const uint8_t* lens, int n)
{
	memset(h.count, 0, sizeof(h.count));
	for (int i = 0; i < n; ++i) h.count[lens[i]]++;
	h.count[0] = 0;
	uint16_t offs[16] = {};
	for (int i = 1; i < 15; ++i) offs[i + 1] = offs[i] + h.count[i];
	for (int i = 0; i < n; ++i)
		if (lens[i]) h.symbol[offs[lens[i]]++] = (uint16_t)i;
}

static int Decode(State& s, const Huff& h)
{
	int code = 0, first = 0, index = 0;
	for (int len = 1; len < 16; ++len) {
		code |= Bits(s, 1);
		const int count = h.count[len];
		if (code - count < first) return h.symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
		if (s.err) break;
	}
	s.err = true;
	return 0;
}

static void Codes(State& s, const Huff& lit, const Huff& dist)
{
	static const uint16_t lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51,
	                                    59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
	static const uint8_t lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4,
	                                  5, 5, 5, 5, 0 };
	static const uint16_t dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
	                                    513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
	static const uint8_t dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10,
	                                  10, 11, 11, 12, 12, 13, 13 };
	std::vector<uint8_t>& out = *s.out;
	while (!s.err) {
		int sym = Decode(s, lit);
		if (sym < 256) { out.push_back((uint8_t)sym); continue; }
		if (sym == 256) return;
		sym -= 257;
		if (sym >= 29) { s.err = true; return; }
		const int len = lbase[sym] + Bits(s, lext[sym]);
		const int dsym = Decode(s, dist);
		if (dsym >= 30) { s.err = true; return; }
		const size_t d = (size_t)dbase[dsym] + Bits(s, dext[dsym]);
		if (d > out.size()) { s.err = true; return; }
		for (int i = 0; i < len; ++i) out.push_back(out[out.size() - d]);
	}
}

// zlib stream -> out; false on malformed data.
static bool Zlib(const uint8_t* src, size_t len, std::vector<uint8_t>& out)
{
	if (len < 2 || (src[0] & 0x0F) != 8 || (src[1] & 0x20)) return false;
	State s;
	s.src = src; s.len = len; s.pos = 2; s.out = &out;
	int last;
	do {
		last = Bits(s, 1);
		const int type = Bits(s, 2);
		if (type == 0) {
			s.buf = 0; s.cnt = 0;
			if (s.pos + 4 > s.len) return false;
			const unsigned n = s.src[s.pos] | s.src[s.pos + 1] << 8;
			s.pos += 4;
			if (s.pos + n > s.len) return false;
			out.insert(out.end(), s.src + s.pos, s.src + s.pos + n);
			s.pos += n;
		} else if (type == 1) {
			static Huff lit, dist;
			static bool built;
			if (!built) {
				uint8_t l[288];
				for (int i = 0; i < 288; ++i) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
				Build(lit, l, 288);
				for (int i = 0; i < 30; ++i) l[i] = 5;
				Build(dist, l, 30);
				built = true;
			}
			Codes(s, lit, dist);
		} else if (type == 2) {
			static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
			const int nlen = Bits(s, 5) + 257, ndist = Bits(s, 5) + 1, ncode = Bits(s, 4) + 4;
			if (nlen > 286 || ndist > 30) return false;
			uint8_t lens[320] = {};
			for (int i = 0; i < ncode; ++i) lens[order[i]] = (uint8_t)Bits(s, 3);
			Huff lc, lit, dist;
			Build(lc, lens, 19);
			int i = 0;
			while (i < nlen + ndist && !s.err) {
				int sym = Decode(s, lc);
				if (sym < 16) { lens[i++] = (uint8_t)sym; continue; }
				int val = 0, rep;
				if (sym == 16) {
					if (i == 0) return false;
					val = lens[i - 1];
					rep = 3 + Bits(s, 2);
				} else if (sym == 17) rep = 3 + Bits(s, 3);
				else rep = 11 + Bits(s, 7);
				if (i + rep > nlen + ndist) return false;
				while (rep--) lens[i++] = (uint8_t)val;
			}
			Build(lit, lens, nlen);
			Build(dist, lens + nlen, ndist);
			Codes(s, lit, dist);
		} else {
			return false;
		}
		if (s.err) return false;
	} while (!last);
	return true;
}
}  // namespace inflate

static bool ReadWholeFile(const char* path, std::vector<uint8_t>& out)
{
	FILE* f = fopen(path, "rb");
	if (!f) return false;
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	out.resize(n > 0 ? (size_t)n : 0);
	const bool ok = n > 0 && fread(out.data(), 1, (size_t)n, f) == (size_t)n;
	fclose(f);
	return ok;
}

// A file from a version 104 BSA (Fallout 3). rel = "textures\\interface\\x.dds" (any case).
static bool ReadFromBsa(const char* bsaPath, const char* rel, std::vector<uint8_t>& out)
{
	FILE* f = fopen(bsaPath, "rb");
	if (!f) return false;
	bool ok = false;
	uint32_t h[9];
	if (fread(h, 4, 9, f) == 9 && h[0] == 0x00415342 && h[1] == 104 && (h[3] & 3) == 3) {
		const uint32_t aflags = h[3], nfold = h[4], nfile = h[5], lfold = h[6], lfile = h[7];
		std::vector<uint8_t> recs((size_t)nfold * 16 + nfold + lfold + (size_t)nfile * 16 + lfile);
		fseek(f, (long)h[2], SEEK_SET);
		if (fread(recs.data(), 1, recs.size(), f) == recs.size()) {
			char dir[MAX_PATH];
			const char* slash = strrchr(rel, '\\');
			const size_t dlen = slash ? (size_t)(slash - rel) : 0;
			memcpy(dir, rel, dlen);
			dir[dlen] = 0;
			const char* name = slash ? slash + 1 : rel;
			// file records (per folder: name bstring + count records), then the file name block
			const uint8_t* p = recs.data() + (size_t)nfold * 16;
			const char* nm = (const char*)recs.data() + recs.size() - lfile;
			for (uint32_t fi = 0; fi < nfold && !ok; ++fi) {
				const uint32_t count = *(const uint32_t*)(recs.data() + fi * 16 + 8);
				const uint8_t nlen = *p++;
				const bool dirMatch = _stricmp((const char*)p, dir) == 0;
				p += nlen;
				for (uint32_t i = 0; i < count; ++i, p += 16) {
					const bool hit = dirMatch && _stricmp(nm, name) == 0;
					nm += strlen(nm) + 1;
					if (!hit) continue;
					uint32_t size = *(const uint32_t*)(p + 8);
					const uint32_t off = *(const uint32_t*)(p + 12);
					const bool comp = ((aflags & 4) != 0) != ((size & 0x40000000) != 0);
					size &= 0x3FFFFFFF;
					std::vector<uint8_t> raw(size);
					fseek(f, (long)off, SEEK_SET);
					if (fread(raw.data(), 1, size, f) != size) break;
					size_t at = 0;
					if (aflags & 0x100) at = 1 + (size_t)raw[0];  // embedded file name
					if (comp) {
						if (at + 4 > raw.size()) break;
						const uint32_t orig = *(const uint32_t*)(raw.data() + at);
						out.clear();
						out.reserve(orig);
						ok = inflate::Zlib(raw.data() + at + 4, raw.size() - at - 4, out) && out.size() == orig;
					} else {
						out.assign(raw.begin() + at, raw.end());
						ok = true;
					}
					break;
				}
			}
		}
	}
	fclose(f);
	return ok;
}

static bool ReadGameFile(const char* rel, std::vector<uint8_t>& out)
{
	char data[MAX_PATH], path[MAX_PATH];
	GetModuleFileNameA(nullptr, data, sizeof(data));
	char* slash = strrchr(data, '\\');
	if (slash) slash[1] = 0;
	snprintf(path, sizeof(path), "%sData\\%s", data, rel);
	if (ReadWholeFile(path, out)) return true;
	snprintf(path, sizeof(path), "%sData\\Fallout - Textures.bsa", data);
	return ReadFromBsa(path, rel, out);
}

// Decodes a rectangle of a DDS (DXT1/3/5 or 32-bit ARGB) into A8R8G8B8 pixels.
static bool DecodeDds(const std::vector<uint8_t>& dds, int x0, int y0, int w, int h, std::vector<uint32_t>& px)
{
	if (dds.size() < 128 || memcmp(dds.data(), "DDS ", 4) != 0) return false;
	const int H = *(const int*)(dds.data() + 12), W = *(const int*)(dds.data() + 16);
	const uint32_t pfFlags = *(const uint32_t*)(dds.data() + 80), fourcc = *(const uint32_t*)(dds.data() + 84);
	if (x0 < 0 || y0 < 0 || w <= 0 || h <= 0 || x0 + w > W || y0 + h > H) return false;
	const uint8_t* d = dds.data() + 128;
	px.assign((size_t)w * h, 0);
	if (pfFlags & 4) {  // compressed
		const int kind = fourcc == MAKEFOURCC('D', 'X', 'T', '1') ? 1 : fourcc == MAKEFOURCC('D', 'X', 'T', '3') ? 3 :
		                 fourcc == MAKEFOURCC('D', 'X', 'T', '5') ? 5 : 0;
		if (!kind) return false;
		const int bsize = kind == 1 ? 8 : 16, bw = (W + 3) / 4;
		if (dds.size() < 128 + (size_t)bw * ((H + 3) / 4) * bsize) return false;
		auto ch = [](uint32_t v, int k) -> uint32_t {  // 565 channel k (0 = r) expanded to 8 bits
			return k == 0 ? (v >> 11 & 31) * 255 / 31 : k == 1 ? (v >> 5 & 63) * 255 / 63 : (v & 31) * 255 / 31;
		};
		for (int y = y0; y < y0 + h; ++y)
			for (int x = x0; x < x0 + w; ++x) {
				const uint8_t* b = d + ((size_t)(y / 4) * bw + x / 4) * bsize;
				const int i = (y & 3) * 4 + (x & 3);
				uint32_t a = 255;
				if (kind == 3) {
					a = ((b[i / 2] >> ((i & 1) * 4)) & 15) * 17;
				} else if (kind == 5) {
					const uint32_t a0 = b[0], a1 = b[1];
					uint64_t bits = 0;
					for (int k = 0; k < 6; ++k) bits |= (uint64_t)b[2 + k] << (8 * k);
					const uint32_t ai = (uint32_t)(bits >> (3 * i)) & 7;
					if (ai == 0) a = a0;
					else if (ai == 1) a = a1;
					else if (a0 > a1) a = ((8 - ai) * a0 + (ai - 1) * a1) / 7;
					else a = ai == 6 ? 0 : ai == 7 ? 255 : ((6 - ai) * a0 + (ai - 1) * a1) / 5;
				}
				const uint8_t* c = b + (kind == 1 ? 0 : 8);
				const uint32_t c0 = c[0] | c[1] << 8, c1 = c[2] | c[3] << 8;
				const uint32_t ci = (c[4 + (i >> 2)] >> ((i & 3) * 2)) & 3;
				uint32_t rgb = 0;
				for (int k = 0; k < 3; ++k) {
					const uint32_t e0 = ch(c0, k), e1 = ch(c1, k);
					uint32_t v;
					if (ci == 0) v = e0;
					else if (ci == 1) v = e1;
					else if (kind != 1 || c0 > c1) v = ci == 2 ? (2 * e0 + e1) / 3 : (e0 + 2 * e1) / 3;
					else if (ci == 2) v = (e0 + e1) / 2;
					else { v = 0; a = 0; }
					rgb = rgb << 8 | v;
				}
				px[(size_t)(y - y0) * w + (x - x0)] = a << 24 | rgb;
			}
		return true;
	}
	if (*(const uint32_t*)(dds.data() + 88) != 32 || dds.size() < 128 + (size_t)W * H * 4) return false;
	for (int y = 0; y < h; ++y)
		memcpy(&px[(size_t)y * w], d + ((size_t)(y0 + y) * W + x0) * 4, (size_t)w * 4);
	return true;
}

struct XhTex {
	IDirect3DTexture9* tex = nullptr;
	int w = 0, h = 0;
	float centreY = 0.0f;               // texels: vertical centre of the drawn shape
	float tip = 0.0f;                   // texels: from the middle to the inner tip of each half
};

static struct XhImage {
	bool tried = false;
	XhTex img[2];                       // the game's crosshair: [0] normal, [1] filled (aiming at someone)
	IDirect3DTexture9* dot = nullptr;   // round dot with a dark soft rim
} xhImg;

constexpr float kDotCore = 0.45f;       // share of the dot texture's radius that is the bright core

static IDirect3DTexture9* MakeTexture(IDirect3DDevice9* dev, const std::vector<uint32_t>& px, int w, int h)
{
	IDirect3DTexture9* t = nullptr;
	if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) || !t) return nullptr;
	D3DLOCKED_RECT lr;
	if (FAILED(t->LockRect(0, &lr, nullptr, 0))) { t->Release(); return nullptr; }
	for (int y = 0; y < h; ++y) memcpy((uint8_t*)lr.pBits + (size_t)y * lr.Pitch, &px[(size_t)y * w], (size_t)w * 4);
	t->UnlockRect(0);
	return t;
}

// Loads one image of the interface atlas (as listed in interfaceshared.tai).
static bool LoadAtlasImage(IDirect3DDevice9* dev, const std::vector<uint8_t>& taiFile, const char* want, XhTex& out)
{
	std::vector<char> tai(taiFile.begin(), taiFile.end());
	tai.push_back(0);
	char atlas[MAX_PATH] = "";
	float u = 0, v = 0, uw = 0, vh = 0;
	for (char* line = tai.data(); line && *line;) {
		char* next = strpbrk(line, "\r\n");
		if (next) *next++ = 0;
		while (next && (*next == '\r' || *next == '\n')) ++next;
		char name[MAX_PATH], file[MAX_PATH], kind[8];
		int idx;
		float d;
		if (sscanf(line, "%259s %259[^,], %d, %7[^,], %f, %f, %f, %f, %f", name, file, &idx, kind, &u, &v, &d, &uw,
		           &vh) == 9 && _stricmp(name, want) == 0) {
			snprintf(atlas, sizeof(atlas), "%s", file);
			break;
		}
		line = next;
	}
	if (!*atlas) {
		Log("WARNING: crosshair: %s not in interfaceshared.tai", want);
		return false;
	}
	char rel[MAX_PATH];
	snprintf(rel, sizeof(rel), "textures\\interface\\%s", atlas);
	std::vector<uint8_t> dds;
	if (!ReadGameFile(rel, dds) || dds.size() < 128) {
		Log("WARNING: crosshair: %s not found", rel);
		return false;
	}
	const int H = *(const int*)(dds.data() + 12), W = *(const int*)(dds.data() + 16);
	// atlas coordinates are inset by half a texel
	const int x0 = (int)std::lround(u * W - 0.5f), y0 = (int)std::lround(v * H - 0.5f);
	const int x1 = (int)std::lround((u + uw) * W + 0.5f), y1 = (int)std::lround((v + vh) * H + 0.5f);
	std::vector<uint32_t> px;
	if (!DecodeDds(dds, x0, y0, x1 - x0, y1 - y0, px)) {
		Log("WARNING: crosshair: could not decode %s", rel);
		return false;
	}
	const int w = x1 - x0, h = y1 - y0, mid = w / 2;
	int top = h, bottom = -1, leftTip = -1, rightTip = w;
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x)
			if ((px[(size_t)y * w + x] >> 24) > 40) {
				if (y < top) top = y;
				if (y > bottom) bottom = y;
				if (x < mid && x > leftTip) leftTip = x;
				if (x >= mid && x < rightTip) rightTip = x;
			}
	if (bottom < 0) {
		Log("WARNING: crosshair: %s is empty", want);
		return false;
	}
	out.w = w;
	out.h = h;
	out.centreY = (top + bottom + 1) * 0.5f;
	out.tip = std::fmin(leftTip >= 0 ? (float)(mid - (leftTip + 1)) : 0.0f,
	                    rightTip < w ? (float)(rightTip - mid) : 0.0f);
	out.tex = MakeTexture(dev, px, w, h);
	Log("crosshair: %s %dx%d from %s (%d,%d), tips %.1f texels from the middle%s", want, w, h, rel, x0, y0, out.tip,
	    out.tex ? "" : " - TEXTURE CREATION FAILED");
	return out.tex != nullptr;
}

static void LoadCrosshairImage(IDirect3DDevice9* dev)
{
	xhImg.tried = true;
	std::vector<uint8_t> tai;
	if (!ReadGameFile("textures\\interface\\interfaceshared.tai", tai)) {
		Log("WARNING: crosshair: interfaceshared.tai not found; using the four line crosshair");
		return;
	}
	if (!LoadAtlasImage(dev, tai, "glow_crosshair.dds", xhImg.img[0])) {
		Log("WARNING: crosshair: using the four line crosshair");
		return;
	}
	LoadAtlasImage(dev, tai, "glow_crosshair_filled.dds", xhImg.img[1]);

	// Dot: bright core, then a dark rim fading out (like the soft dark edge of the game's crosshair).
	const int ds = 64;
	std::vector<uint32_t> dot((size_t)ds * ds);
	for (int y = 0; y < ds; ++y)
		for (int x = 0; x < ds; ++x) {
			const float r = std::hypot(x + 0.5f - ds * 0.5f, y + 0.5f - ds * 0.5f) / (ds * 0.5f);
			const float edge = ds * 0.5f * kDotCore * 0.25f;  // core edge softness
			const float core = std::fmax(0.0f, std::fmin(1.0f, (kDotCore - r) / kDotCore * edge + 0.5f));
			// rim fades out along a smoothstep curve: no hard outer edge, like the crosshair's glow
			const float sr = std::fmax(0.0f, std::fmin(1.0f, (1.0f - r) / (1.0f - kDotCore)));
			const float rim = sr * sr * (3.0f - 2.0f * sr) * 0.75f;
			const float a = core + rim * (1.0f - core);
			const float lum = a > 0.0f ? core / a : 0.0f;  // white core over a dark rim
			const uint32_t c = (uint32_t)(40.0f + lum * 215.0f + 0.5f);
			dot[(size_t)y * ds + x] = (uint32_t)(a * 255.0f + 0.5f) << 24 | c << 16 | c << 8 | c;
		}
	xhImg.dot = MakeTexture(dev, dot, ds, ds);
}

struct XhTexVertex { float x, y, z, rhw; D3DCOLOR c; float u, v; };

static void TexRect(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1, float u0, float u1, D3DCOLOR c)
{
	x0 -= 0.5f; y0 -= 0.5f; x1 -= 0.5f; y1 -= 0.5f;
	const XhTexVertex v[4] = { { x0, y0, 0, 1, c, u0, 0 }, { x1, y0, 0, 1, c, u1, 0 },
	                           { x0, y1, 0, 1, c, u0, 1 }, { x1, y1, 0, 1, c, u1, 1 } };
	dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(XhTexVertex));
}

// Style 0: the halves of the game's crosshair move apart so their tips sit on the spread cone.
static void DrawSplitCrosshair(IDirect3DDevice9* dev, const XhTex& img, float W, float H, D3DCOLOR color)
{
	const CrosshairConfig& cc = g_cfg.xh;
	const float s = cc.size * H / 960.0f;  // interface units (960 = screen height) -> pixels
	const float cx = W * 0.5f, cy = H * 0.5f;
	const float off = std::fmax(0.0f, xh.gap - img.tip * s);
	const float half = img.w * 0.5f * s;
	const float yt = cy - img.centreY * s, yb = yt + img.h * s;
	dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);
	dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_MODULATE);
	dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
	dev->SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
	dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
	dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
	dev->SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);
	dev->SetSamplerState(0, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
	dev->SetSamplerState(0, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
	dev->SetSamplerState(0, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
	dev->SetSamplerState(0, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
	dev->SetSamplerState(0, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
	dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
	dev->SetTexture(0, img.tex);
	TexRect(dev, cx - off - half, yt, cx - off, yb, 0.0f, 0.5f, color);
	TexRect(dev, cx + off, yt, cx + off + half, yb, 0.5f, 1.0f, color);
	if (cc.dotSize > 0.0f && xhImg.dot) {
		const float r = cc.dotSize * H / 1080.0f * 0.5f / kDotCore;  // fDotSize = the bright core
		dev->SetTexture(0, xhImg.dot);
		TexRect(dev, cx - r, cy - r, cx + r, cy + r, 0.0f, 1.0f, color);
	}
	dev->SetTexture(0, nullptr);
}

// What the game's own crosshair image currently shows: red (system colour 2, "HUDAlt") when
// aiming at someone hostile, and the filled version (brackets) when aiming at an actor.
static void VanillaCrosshairState(bool* hostile, bool* filled)
{
	*hostile = *filled = false;
	uint8_t* hud = *(uint8_t**)addr::kHUDMainMenu;
	void* img = hud ? *(void**)(hud + 0xF4) : nullptr;
	if (!img) return;
	*hostile = ((float(__thiscall*)(void*, uint32_t))addr::kTileGetFloat)(img, kTileSystemColor) == 2.0f;
	auto str = [img](uint32_t id) { return ((const char*(__thiscall*)(void*, uint32_t))addr::kTileGetString)(img, id); };
	const char* file = str(kTileFilename);
	const char* user1 = str(kTileUser1);
	*filled = file && user1 && *user1 && _stricmp(file, user1) == 0;
}

static void DrawCrosshair(IDirect3DDevice9* dev)
{
	const CrosshairConfig& cc = g_cfg.xh;
	IDirect3DSurface9* bb = nullptr;
	if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
	D3DSURFACE_DESC desc;
	bb->GetDesc(&desc);
	const float W = (float)desc.Width, H = (float)desc.Height;

	const int64_t now = QpcNow();
	const double dt = xh.lastQpc ? (double)(now - xh.lastQpc) / g_qpcFreq : 0.0;
	xh.lastQpc = now;

	float target = 0.0f;
	const bool show = CrosshairTarget(W, &target);
	if (show || xh.vanillaHidden) SetVanillaCrosshair(show);
	if (!show) {
		xh.visible = false;
		bb->Release();
		return;
	}
	const float k = H / 1080.0f;  // resolution scale
	if (cc.style == 0 && !xhImg.tried) LoadCrosshairImage(dev);
	bool hostile, filled;
	VanillaCrosshairState(&hostile, &filled);
	const XhTex& img = xhImg.img[filled && xhImg.img[1].tex ? 1 : 0];
	const bool split = cc.style == 0 && img.tex;
	const float minGap = split ? 0.0f : cc.minGap * k;
	if (target < minGap) target = minGap;
	if (target > H * 0.4f) target = H * 0.4f;
	if (!xh.visible) { xh.gap = target; xh.gapVel = 0.0f; }  // appear at the right size
	else xh.gap = SmoothDamp(xh.gap, target, &xh.gapVel, dt > 0.1 ? 0.1 : dt, cc.smoothTime);
	xh.visible = true;

	uint32_t rgb = (uint32_t)((cc.r & 255) << 16 | (cc.gr & 255) << 8 | (cc.b & 255));
	if (hostile) {
		rgb = 0xFF432A;  // HUDAlt, as the game's crosshair over enemies
	} else if (cc.hudColor) {
		const GameSettingF* s = (const GameSettingF*)addr::kSettingHUDColor;
		if (!IsBadReadPtr(s->name, 20) && memcmp(s->name, "uHUDColor:Interface", 20) == 0) {
			const uint32_t v = *(const uint32_t*)&s->value;  // R G B A, R in the high byte
			rgb = v >> 8;
		}
	}
	const uint32_t a = (uint32_t)(cc.opacity * 255.0f + 0.5f);
	const D3DCOLOR color = (a << 24) | rgb;
	const D3DCOLOR shadow = ((uint32_t)(cc.opacity * 0.6f * 255.0f + 0.5f) << 24);

	if (!xh.sb) dev->CreateStateBlock(D3DSBT_ALL, &xh.sb);
	if (xh.sb) xh.sb->Capture();
	IDirect3DSurface9* oldRT = nullptr;
	dev->GetRenderTarget(0, &oldRT);
	dev->SetRenderTarget(0, bb);
	D3DVIEWPORT9 vp = { 0, 0, desc.Width, desc.Height, 0.0f, 1.0f };
	dev->SetViewport(&vp);
	dev->SetVertexShader(nullptr);
	dev->SetPixelShader(nullptr);
	dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_DIFFUSE);
	dev->SetTexture(0, nullptr);
	dev->SetRenderState(D3DRS_ZENABLE, FALSE);
	dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
	dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
	dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
	dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
	dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
	dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
	dev->SetRenderState(D3DRS_LIGHTING, FALSE);
	dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
	dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
	dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
	dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
	dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
	dev->SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_SELECTARG1);
	dev->SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_DIFFUSE);
	dev->SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1);
	dev->SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE);
	dev->SetTextureStageState(1, D3DTSS_COLOROP, D3DTOP_DISABLE);
	dev->SetTextureStageState(1, D3DTSS_ALPHAOP, D3DTOP_DISABLE);

	if (SUCCEEDED(dev->BeginScene())) {
		const float cx = std::floor(W * 0.5f), cy = std::floor(H * 0.5f);
		const float g = std::floor(xh.gap + 0.5f);
		const float len = std::fmax(2.0f, std::floor(cc.lineLength * k + 0.5f));
		const float t = std::fmax(1.0f, std::floor(cc.thickness * k + 0.5f));
		const float h0 = std::floor(t * 0.5f), h1 = t - h0;  // thickness split around the centre
		auto lines = [&](float o, D3DCOLOR c) {
			FillRect(dev, cx - h0 - o, cy - g - len - o, cx + h1 + o, cy - g + o, c);  // top
			FillRect(dev, cx - h0 - o, cy + g - o, cx + h1 + o, cy + g + len + o, c);  // bottom
			FillRect(dev, cx - g - len - o, cy - h0 - o, cx - g + o, cy + h1 + o, c);  // left
			FillRect(dev, cx + g - o, cy - h0 - o, cx + g + len + o, cy + h1 + o, c);  // right
		};
		if (split) {
			DrawSplitCrosshair(dev, img, W, H, color);
		} else {
			if (cc.outline) lines(1.0f, shadow);
			lines(0.0f, color);
		}
		dev->EndScene();
	}

	dev->SetRenderTarget(0, oldRT);
	if (oldRT) oldRT->Release();
	if (xh.sb) xh.sb->Apply();
	bb->Release();
}

// ---------------------------------------------------------------------------------------------
// Aim along the crosshair line: bullets start at the gun's muzzle and fly along the player's aim,
// parallel to the camera's line through the crosshair, so in third person they miss what the
// crosshair is on up close, and in both views anything in front of the barrel (a railing, a
// corner the eyes see past) blocks them. Each shot is moved onto the crosshair line
// instead: it starts on that line level with the gun and flies straight along it, so it hits
// exactly what the crosshair shows. Any offset the game already gave the shot (shotgun pellets,
// worn weapons) is kept, and the gun's sway is applied by the game afterwards as before.

static bool g_vatsOk;  // the VATS mode global matched at load

// Moves a player shot (start position, heading/pitch in radians) onto the crosshair line.
static bool AimShotAlongCrosshair(NiPoint3* start, float* heading, float* pitch)
{
	uint8_t* pc = (uint8_t*)Player();
	if (!pc || !g_vatsOk || *(int*)addr::kVATSMode != 0 || *(bool*)addr::kVanityCamera) return false;
	if (!(InThirdPerson() ? g_cfg.xh.converge : g_cfg.xh.convergeFirst)) return false;
	const float h = *(float*)(pc + off::kRefrRotZ), pt = *(float*)(pc + off::kRefrRotX);
	// The game's own offset from the aim (pellets, worn weapons); shots far off the view are
	// aimed by the game itself, leave those alone.
	const float dh = WrapPi(*heading - h), dp = *pitch - pt;
	if (std::fabs(dh) > 0.35f || std::fabs(dp) > 0.35f) return false;
	uint8_t* camRoot = *(uint8_t**)addr::kCameraRoot;
	NiAVObject* cam = camRoot ? *(NiAVObject**)(camRoot + 0xAC) : nullptr;
	if (!cam) return false;
	// The crosshair line is the camera's view axis: of the camera's axes, the one closest to the aim.
	const NiPoint3 aim = { std::sin(h) * std::cos(pt), std::cos(h) * std::cos(pt), -std::sin(pt) };
	const NiMatrix3& R = cam->world.rot;
	NiPoint3 fwd = aim;
	float bestDot = 0.0f;
	for (int k = 0; k < 3; ++k) {
		const NiPoint3 c = { R.m[0][k], R.m[1][k], R.m[2][k] };
		const float d = c.x * aim.x + c.y * aim.y + c.z * aim.z;
		if (std::fabs(d) > std::fabs(bestDot)) { bestDot = d; fwd = d < 0.0f ? NiPoint3{ -c.x, -c.y, -c.z } : c; }
	}
	if (std::fabs(bestDot) < 0.95f) return false;  // camera not looking along the aim
	const NiPoint3& cp = cam->world.pos;
	// Level with the gun: as far along the crosshair line as the muzzle is in front of the camera.
	const float depth = std::fmax(0.0f, (start->x - cp.x) * fwd.x + (start->y - cp.y) * fwd.y +
	                                      (start->z - cp.z) * fwd.z);
	const NiPoint3 from = { cp.x + fwd.x * depth, cp.y + fwd.y * depth, cp.z + fwd.z * depth };
	float nh = std::atan2(fwd.x, fwd.y) + dh;
	if (nh >= 2.0f * kPi) nh -= 2.0f * kPi;
	if (nh < 0.0f) nh += 2.0f * kPi;
	const float np = -std::asin(std::fmax(-1.0f, std::fmin(1.0f, fwd.z))) + dp;
	if (g_cfg.debug)
		Log("aim line: shot moved %.0f units (from the muzzle to the crosshair line), turned %+.2f deg right, "
		    "%+.2f deg down", std::sqrt((from.x - start->x) * (from.x - start->x) +
		    (from.y - start->y) * (from.y - start->y) + (from.z - start->z) * (from.z - start->z)),
		    WrapPi(nh - *heading) * kRadToDeg, (np - *pitch) * kRadToDeg);
	*start = from;
	*heading = nh;
	*pitch = np;
	return true;
}

// A shot on the crosshair line starts beside the camera and flies straight away from it, so its
// tracer appears to come out of the camera (and from the wrong side while strafing): show the
// plain bullet instead. Bullets (missile projectiles) only. The game rolls the tracer at the end
// of creation into flag 0x10 at +0xC0 and, when the model is set up (0x7C4878), shows child 1
// (tracer) and hides child 0 (bullet) if it is set, or the other way round. Clear the flag, and if
// the model already exists, show/hide its children the same way.
static void HideTracer(uint8_t* proj, uint8_t* form)
{
	if ((*(uint32_t*)(form + 0x60) & 0xF0000) != 0x10000) return;  // not a bullet
	uint32_t& flags = *(uint32_t*)(proj + 0xC0);
	if (!(flags & 0x10)) return;
	flags &= ~0x10u;
	if (*(float*)(form + 0x78) == 0.0f) return;  // no tracer chance: the model has no tracer part
	NiNode* root = AsNode(VCall<NiAVObject*>(proj, 0x1D0));  // Get3D
	if (!root || root->firstFree < 2) return;
	if (root->children[0]) root->children[0]->flags &= ~1u;  // bullet shown
	if (root->children[1]) root->children[1]->flags |= 1u;   // tracer culled
}

typedef void*(__cdecl* CreateProjectile_t)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                           uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                           uint32_t, uint32_t);

static void* __cdecl Hook_CreateProjectile(uint32_t a0, uint32_t shooter, uint32_t a2, uint32_t a3, float x, float y,
                                           float z, float heading, float pitch, uint32_t a9, uint32_t a10, uint32_t a11,
                                           uint32_t a12, uint32_t a13, uint32_t a14, uint32_t a15)
{
	bool moved = false;
	if ((g_cfg.xh.converge || g_cfg.xh.convergeFirst) && shooter == (uint32_t)(uintptr_t)Player() &&
	    !PlayerRagdollReason()) {
		NiPoint3 p = { x, y, z };
		if (AimShotAlongCrosshair(&p, &heading, &pitch)) { x = p.x; y = p.y; z = p.z; moved = true; }
	}
	auto bits = [](float f) { uint32_t u; memcpy(&u, &f, 4); return u; };
	void* proj = ((CreateProjectile_t)addr::kCreateProjectile)(a0, shooter, a2, a3, bits(x), bits(y), bits(z),
	                                                           bits(heading), bits(pitch), a9, a10, a11, a12, a13,
	                                                           a14, a15);
	if (moved && proj && g_cfg.xh.hideTracers) HideTracer((uint8_t*)proj, (uint8_t*)(uintptr_t)a0);
	return proj;
}

// The HUD mode change (aiming, menus closing...) sets the vanilla crosshair visible again; while
// ours shows, keep it hidden right there, before the HUD draws it for a frame.
static void __fastcall Hook_HudCrosshairVisible(void* tile, void*, uint32_t id, float value, bool propagate)
{
	if (xh.visible && id == kTileVisible && value != 0.0f) {
		value = 0.0f;
		if (g_cfg.debug) {
			static int n;
			if (n++ < 5) Log("crosshair: kept the vanilla crosshair hidden on a HUD mode change");
		}
	}
	((void(__thiscall*)(void*, uint32_t, float, bool))addr::kTileSetFloat)(tile, id, value, propagate);
}

typedef HRESULT(__stdcall* D3DPresent_t)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
typedef HRESULT(__stdcall* D3DReset_t)(IDirect3DDevice9*, D3DPRESENT_PARAMETERS*);
static D3DPresent_t g_origPresent;
static D3DReset_t   g_origReset;

typedef HRESULT(__stdcall* SCPresent_t)(IDirect3DSwapChain9*, const RECT*, const RECT*, HWND, const RGNDATA*,
                                         DWORD);
typedef HRESULT(__stdcall* D3DCreateDevice_t)(IDirect3D9*, UINT, D3DDEVTYPE, HWND, DWORD,
                                              D3DPRESENT_PARAMETERS*, IDirect3DDevice9**);
static SCPresent_t       g_origSCPresent;
static D3DCreateDevice_t g_origCreateDevice;
static IDirect3DDevice9* g_gameDevice;
static bool              g_inPresent;
static int               g_presentLogs;

static HRESULT __stdcall Hook_Present(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, HWND wnd,
                                      const RGNDATA* dirty)
{
	if (g_cfg.debug && g_presentLogs < 1) { ++g_presentLogs; Log("crosshair: device Present called"); }
	g_inPresent = true;
	DrawCrosshair(dev);
	const HRESULT hr = g_origPresent(dev, src, dst, wnd, dirty);
	g_inPresent = false;
	return hr;
}

// Some renderers present through the swap chain instead of the device.
static HRESULT __stdcall Hook_SCPresent(IDirect3DSwapChain9* sc, const RECT* src, const RECT* dst, HWND wnd,
                                        const RGNDATA* dirty, DWORD flags)
{
	if (!g_inPresent && g_gameDevice) {
		if (g_cfg.debug && g_presentLogs < 2) { g_presentLogs = 2; Log("crosshair: swap chain Present called"); }
		DrawCrosshair(g_gameDevice);
	}
	return g_origSCPresent(sc, src, dst, wnd, dirty, flags);
}

static HRESULT __stdcall Hook_Reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp)
{
	if (xh.sb) { xh.sb->Release(); xh.sb = nullptr; }
	return g_origReset(dev, pp);
}

static bool PatchPtr(void** p, void* fn, void** orig)
{
	if (*p == fn) return true;  // already ours
	DWORD old;
	if (!VirtualProtect(p, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
	*orig = *p;
	*p = fn;
	VirtualProtect(p, sizeof(void*), old, &old);
	return true;
}

// Hooks the game's own device (its class depends on the creation flags, so it can't be found
// with a probe device).
static HRESULT __stdcall Hook_CreateDevice(IDirect3D9* d3d, UINT adapter, D3DDEVTYPE type, HWND wnd, DWORD flags,
                                           D3DPRESENT_PARAMETERS* pp, IDirect3DDevice9** out)
{
	const HRESULT hr = g_origCreateDevice(d3d, adapter, type, wnd, flags, pp, out);
	if (FAILED(hr) || !out || !*out) {
		Log("crosshair: CreateDevice failed (%08X)", (unsigned)hr);
		return hr;
	}
	IDirect3DDevice9* dev = *out;
	g_gameDevice = dev;
	void** vt = *(void***)dev;
	const bool ok = PatchPtr(&vt[16], (void*)&Hook_Reset, (void**)&g_origReset) &&
	                PatchPtr(&vt[17], (void*)&Hook_Present, (void**)&g_origPresent);
	bool scOk = false;
	IDirect3DSwapChain9* sc = nullptr;
	if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
		scOk = PatchPtr(&(*(void***)sc)[3], (void*)&Hook_SCPresent, (void**)&g_origSCPresent);
		sc->Release();
	}
	Log("crosshair: game created its Direct3D device (flags %08X, vtable %p); hooked Present %s, swap chain %s",
	    (unsigned)flags, (void*)vt, ok ? "yes" : "NO", scOk ? "yes" : "NO");
	return hr;
}

// Direct3D isn't imported by the game (it is loaded at runtime): hook IDirect3D9::CreateDevice
// (shared by every IDirect3D9 object) so the game's device can be hooked when it is created.
static bool HookD3D9()
{
	IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
	if (!d3d) return false;
	void** vt = *(void***)d3d;
	const bool ok = PatchPtr(&vt[16], (void*)&Hook_CreateDevice, (void**)&g_origCreateDevice);
	d3d->Release();
	return ok;
}

static bool PatchVtableSlot(uintptr_t vtbl, uintptr_t slot, uintptr_t expect, void* fn)
{
	uintptr_t* p = (uintptr_t*)(vtbl + slot);
	if (*p != expect) {
		Log("ERROR: vtable %08X+%X is %08X, expected %08X (wrong Fallout3.exe version, or another "
		    "plugin hooks it)", (unsigned)vtbl, (unsigned)slot, (unsigned)*p, (unsigned)expect);
		return false;
	}
	DWORD old;
	if (!VirtualProtect(p, 4, PAGE_READWRITE, &old)) return false;
	*p = (uintptr_t)fn;
	VirtualProtect(p, 4, old, &old);
	return true;
}

static bool VerifyGame()
{
	// mov eax,[ecx+1C] ; test eax,eax ; je +0C ; mov edx,[eax] ; mov edx,[edx+90]
	const uint8_t uwd[] = { 0x8B, 0x41, 0x1C, 0x85, 0xC0, 0x74, 0x0C, 0x8B, 0x10, 0x8B, 0x92, 0x90, 0x00, 0x00, 0x00 };
	if (memcmp((const void*)addr::kUpdateWorldData, uwd, sizeof(uwd)) != 0) {
		Log("ERROR: NiAVObject::UpdateWorldData does not match (wrong Fallout3.exe version?)");
		return false;
	}
	// PlayerMover::GetMovementFlags: mov eax,[ecx+94] ; ret
	const uint8_t getFlags[] = { 0x8B, 0x81, 0x94, 0x00, 0x00, 0x00, 0xC3 };
	const uintptr_t fn = *(uintptr_t*)(addr::kPlayerMoverVtbl + 0x1C);
	if (fn != 0x007E8DC0 || memcmp((const void*)fn, getFlags, sizeof(getFlags)) != 0) {
		Log("ERROR: PlayerMover layout does not match");
		return false;
	}
	// TESObjectREFR::Get3D tail: mov eax,[ecx+5C] ; test eax,eax ; je ; mov eax,[eax+14]
	const uint8_t get3d[] = { 0x8B, 0x41, 0x5C, 0x85, 0xC0, 0x74, 0x04, 0x8B, 0x40, 0x14 };
	if (memcmp((const void*)0x00424ECE, get3d, sizeof(get3d)) != 0) {
		Log("ERROR: TESObjectREFR 3D layout does not match");
		return false;
	}
	return true;
}

// ---------------------------------------------------------------------------------------------
// FOSE entry points

static void InitPaths()
{
	HMODULE self = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   (LPCSTR)&InitPaths, &self);
	GetModuleFileNameA(self, g_dir, sizeof(g_dir));
	char* slash = strrchr(g_dir, '\\');
	if (slash) slash[1] = '\0';

	char logPath[MAX_PATH];
	snprintf(logPath, sizeof(logPath), "%s" PLUGIN_NAME ".log", g_dir);
	g_log = fopen(logPath, "w");
}

extern "C" {

__declspec(dllexport) bool FOSEPlugin_Query(const FOSEInterface* fose, PluginInfo* info)
{
	info->infoVersion = 1;
	info->name        = PLUGIN_NAME;
	info->version     = PLUGIN_VERSION;

	InitPaths();
	Log(PLUGIN_NAME " " PLUGIN_VERSION_STR ": FOSE %08X, runtime %08X, editor=%u",
	    fose->foseVersion, fose->runtimeVersion, fose->isEditor);
	return !fose->isEditor;
}

__declspec(dllexport) bool FOSEPlugin_Load(const FOSEInterface* fose)
{
	(void)fose;
	LoadConfig();
	if (!g_cfg.enabled) {
		Log("disabled in ini; no changes made.");
		return true;
	}
	if (!VerifyGame()) {
		Log("Plugin disabled; no changes made.");
		return false;
	}

	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	g_qpcFreq = (double)freq.QuadPart;

	g_origUpdateWorldData = (UpdateWorldData_t)addr::kUpdateWorldData;
	if (!PatchVtableSlot(addr::kNiNodeVtbl, addr::kSlotUpdateWorldData, addr::kUpdateWorldData,
	                     (void*)&Hook_UpdateWorldData)) {
		Log("Plugin disabled; no changes made.");
		return false;
	}
	Log("hooked NiNode::UpdateWorldData (vtable %08X+%X)", (unsigned)addr::kNiNodeVtbl,
	    (unsigned)addr::kSlotUpdateWorldData);
	// The actor 3D root's class, for sBodyNode=Scene Root (its own vtable, same function).
	if (PatchVtableSlot(addr::kRootNodeVtbl, addr::kSlotUpdateWorldData, addr::kUpdateWorldData,
	                    (void*)&Hook_UpdateWorldData))
		Log("hooked root node UpdateWorldData (vtable %08X+%X)", (unsigned)addr::kRootNodeVtbl,
		    (unsigned)addr::kSlotUpdateWorldData);
	else
		Log("WARNING: root node vtable not hooked; use sBodyNode=Bip01");

	// Free look: weapon/sitting/knocked-down checks call these game functions; verify they are
	// the expected virtual calls.
	{
		// cmp [ecx+60],0 ; je ; mov ecx,[ecx+60] ; mov eax,[ecx] ; mov edx,[eax+450]
		const uint8_t weapon[] = { 0x83, 0x79, 0x60, 0x00, 0x74, 0x0D, 0x8B, 0x49, 0x60, 0x8B, 0x01, 0x8B, 0x90, 0x50, 0x04, 0x00, 0x00 };
		// mov eax,[edi] ; mov edx,[eax+214]
		const uint8_t sit[] = { 0x8B, 0x07, 0x8B, 0x90, 0x14, 0x02, 0x00, 0x00 };
		// mov edx,[eax] ; mov ecx,eax ; mov eax,[edx+408]
		const uint8_t knock[] = { 0x8B, 0x10, 0x8B, 0xC8, 0x8B, 0x82, 0x08, 0x04, 0x00, 0x00 };
		// gun sway: IsMenuMode (mov eax,[menu stack] ; test eax,eax) and GetVATSMode (fild [VATS mode])
		const uint8_t isMenu[] = { 0xA1, 0x24, 0x5B, 0x07, 0x01, 0x85, 0xC0 };
		const uint8_t vats[] = { 0xDB, 0x05, 0xA0, 0xD0, 0x08, 0x01 };
		// and HighProcess::IsAiming: mov al,[ecx+228] ; ret
		const uint8_t aiming[] = { 0x8A, 0x81, 0x28, 0x02, 0x00, 0x00, 0xC3 };
		g_swayChecksOk = memcmp((const void*)addr::kIsMenuMode, isMenu, sizeof(isMenu)) == 0 &&
		                 memcmp((const void*)0x005122DF, vats, sizeof(vats)) == 0 &&
		                 *(uintptr_t*)(0x00E15224 + vslot::kProcessIsAiming) == 0x0073B3D0 &&
		                 memcmp((const void*)0x0073B3D0, aiming, sizeof(aiming)) == 0;
		if (!g_swayChecksOk && g_cfg.sway.enabled) Log("WARNING: menu/VATS state functions do not match; no gun sway");
		// push ebx ; mov ebx,[esp+8] ; push ebp ; push esi ; push edi ; push ebx
		const uint8_t playGroup[] = { 0x53, 0x8B, 0x5C, 0x24, 0x08, 0x55, 0x56, 0x57, 0x53 };
		if (g_cfg.sway.enabled && g_cfg.sway.smoothJump && g_swayChecksOk &&
		    memcmp((const void*)addr::kPlayAnimGroup, playGroup, sizeof(playGroup)) == 0) {
			const uintptr_t at = addr::kPlayAnimGroup;
			DWORD old;
			VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old);
			*(uint8_t*)at = 0xE9;
			*(int32_t*)(at + 1) = (int32_t)((uintptr_t)&STRIDE_PlayAnimGroupStub - (at + 5));
			VirtualProtect((void*)at, 5, old, &old);
			FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
			Log("hooked AnimData::PlayAnimGroup at %08X (first person jump animations replaced by a dip)", (unsigned)at);
		} else if (g_cfg.sway.enabled && g_cfg.sway.smoothJump) {
			Log("WARNING: PlayAnimGroup does not match; first person jump animations stay vanilla");
		}
		g_stateChecksOk = memcmp((const void*)addr::kIsWeaponOut, weapon, sizeof(weapon)) == 0 &&
		                  memcmp((const void*)addr::kGetSittingUse, sit, sizeof(sit)) == 0 &&
		                  memcmp((const void*)addr::kGetKnockedUse, knock, sizeof(knock)) == 0;
		if (!g_stateChecksOk && g_cfg.look.enabled)
			Log("WARNING: game state functions do not match; free look disabled");

		// cmp byte [esp+4],0 ; mov eax,[ecx+FC]  (Actor::IsDead reading the life state)
		const uint8_t dead[] = { 0x80, 0x7C, 0x24, 0x04, 0x00, 0x8B, 0x81, 0xFC, 0x00, 0x00, 0x00 };
		g_lifeStateOk = memcmp((const void*)addr::kIsDead, dead, sizeof(dead)) == 0;
		if (!g_lifeStateOk)
			Log("WARNING: Actor::IsDead does not match; STRIDE cannot tell when the player dies");
	}

	if (g_cfg.xh.enabled) {
		// The crosshair uses these game functions/globals; check the code that references them.
		const uint8_t isMenu[] = { 0xA1, 0x24, 0x5B, 0x07, 0x01, 0x85, 0xC0 };
		const uint8_t stat[] = { 0x83, 0xEC, 0x10, 0x56, 0x8B, 0xF1, 0x8B, 0x4E, 0x60, 0x85, 0xC9 };
		const uint8_t statWeap[] = { 0x8B, 0x01, 0x8B, 0x90, 0x48, 0x01, 0x00, 0x00 };
		// mov ecx,[eax+18C] ; and ecx,40 ... mov ecx,[eax+12C] ... push FA3 (HUD mode -> crosshair tile)
		const uint8_t hudBit[] = { 0x8B, 0x88, 0x8C, 0x01, 0x00, 0x00, 0x83, 0xE1, 0x40 };
		const uint8_t hudTile[] = { 0x8B, 0x88, 0x2C, 0x01, 0x00, 0x00 };
		const uint8_t hudVis[] = { 0x68, 0xA3, 0x0F, 0x00, 0x00 };
		// Tile::GetValue / Tile::SetFloat: mov eax,[esp+4] ; push eax ; call (value lookup)
		const uint8_t tileFn[] = { 0x8B, 0x44, 0x24, 0x04, 0x50, 0xE8 };
		const uint8_t camRoot[] = { 0xA1, 0x24, 0xA2, 0x07, 0x01 };
		const uint8_t spread[] = { 0xD9, 0x86, 0x04, 0x01, 0x00, 0x00 };  // fld [esi+104h] (minSpread)
		const bool match = memcmp((const void*)addr::kIsMenuMode, isMenu, sizeof(isMenu)) == 0 &&
		                   memcmp((const void*)addr::kGetWeaponStat, stat, sizeof(stat)) == 0 &&
		                   memcmp((const void*)0x00701C71, statWeap, sizeof(statWeap)) == 0 &&
		                   memcmp((const void*)0x0064E39C, hudBit, sizeof(hudBit)) == 0 &&
		                   memcmp((const void*)0x0064E3AF, hudTile, sizeof(hudTile)) == 0 &&
		                   memcmp((const void*)0x0064E3B8, hudVis, sizeof(hudVis)) == 0 &&
		                   memcmp((const void*)addr::kTileGetFloat, tileFn, sizeof(tileFn)) == 0 &&
		                   memcmp((const void*)addr::kTileSetFloat, tileFn, sizeof(tileFn)) == 0 &&
		                   memcmp((const void*)0x0077D6C8, camRoot, sizeof(camRoot)) == 0 &&
		                   memcmp((const void*)0x004BE71D, spread, sizeof(spread)) == 0;
		if (!match)
			Log("WARNING: crosshair code does not match; crosshair disabled");
		else if (HookD3D9()) {
			Log("hooked IDirect3D9::CreateDevice for the crosshair");
			const uint8_t vats[] = { 0xDB, 0x05, 0xA0, 0xD0, 0x08, 0x01 };  // fild [VATS mode] in GetVATSMode
			g_vatsOk = memcmp((const void*)0x005122DF, vats, sizeof(vats)) == 0;
			const uintptr_t cp = addr::kCallCreateProjectile;
			const bool aimLine = g_cfg.xh.converge || g_cfg.xh.convergeFirst;
			if (aimLine && g_vatsOk && *(uint8_t*)cp == 0xE8 &&
			    cp + 5 + *(int32_t*)(cp + 1) == addr::kCreateProjectile) {
				DWORD old;
				VirtualProtect((void*)cp, 5, PAGE_EXECUTE_READWRITE, &old);
				*(int32_t*)(cp + 1) = (int32_t)((uintptr_t)&Hook_CreateProjectile - (cp + 5));
				VirtualProtect((void*)cp, 5, old, &old);
				FlushInstructionCache(GetCurrentProcess(), (void*)cp, 5);
				Log("hooked projectile creation at %08X (shots fly along the crosshair line: third person %d, "
				    "first person %d)", (unsigned)cp, g_cfg.xh.converge, g_cfg.xh.convergeFirst);
			} else if (aimLine) {
				Log("WARNING: projectile creation or VATS state do not match; third person shots stay vanilla");
			}
			const uintptr_t at = addr::kCallHudCrosshairVisible;
			if (*(uint8_t*)at == 0xE8 && at + 5 + *(int32_t*)(at + 1) == addr::kTileSetFloat) {
				DWORD old;
				VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old);
				*(int32_t*)(at + 1) = (int32_t)((uintptr_t)&Hook_HudCrosshairVisible - (at + 5));
				VirtualProtect((void*)at, 5, old, &old);
				FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
				Log("hooked HUD crosshair visibility at %08X", (unsigned)at);
			} else {
				Log("WARNING: HUD crosshair visibility call not found; the vanilla crosshair may flash");
			}
		}
		else
			Log("WARNING: could not hook Direct3D 9; crosshair disabled");
	}

	if (g_cfg.cam.enabled) {
		// call rel32 -> NiAVObject::Update for the third person camera node
		const uintptr_t at = addr::kCallCameraUpdate;
		if (*(uint8_t*)at == 0xE8 && at + 5 + *(int32_t*)(at + 1) == addr::kNiAVObjectUpdate) {
			DWORD old;
			VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old);
			*(int32_t*)(at + 1) = (int32_t)((uintptr_t)&Hook_CameraUpdate - (at + 5));
			VirtualProtect((void*)at, 5, old, &old);
			FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
			Log("hooked third person camera update at %08X", (unsigned)at);
		} else {
			Log("WARNING: camera update call not found; camera features disabled");
		}
	}

	if (g_cfg.move.accel) {
		const uint8_t callEdx[] = { 0xFF, 0xD2 };
		if (memcmp((const void*)(addr::kPlayerMoverMoveRet - 2), callEdx, 2) == 0 &&
		    PatchVtableSlot(addr::kPlayerVtbl, addr::kSlotActorMove, addr::kActorMove, (void*)&Hook_PlayerMove))
			Log("hooked PlayerCharacter::Move (vtable %08X+%X)", (unsigned)addr::kPlayerVtbl,
			    (unsigned)addr::kSlotActorMove);
		else
			Log("WARNING: Actor::Move not hooked; acceleration disabled");
	}

	if (g_cfg.strafeAsRun) {
		// call rel32 -> 0x45D370 at the PlayerMover call site
		const uintptr_t at = addr::kCallAnimMoveVector;
		if (*(uint8_t*)at == 0xE8 && at + 5 + *(int32_t*)(at + 1) == addr::kAnimMoveVector) {
			DWORD old;
			VirtualProtect((void*)at, 5, PAGE_EXECUTE_READWRITE, &old);
			*(int32_t*)(at + 1) = (int32_t)((uintptr_t)&Hook_AnimMoveVector - (at + 5));
			VirtualProtect((void*)at, 5, old, &old);
			FlushInstructionCache(GetCurrentProcess(), (void*)at, 5);
			g_strafeMoveHooked = true;
			Log("hooked PlayerMover movement vector call at %08X", (unsigned)at);
		} else {
			Log("WARNING: movement vector call not found; strafe-as-run disabled");
		}
	}

	if ((g_cfg.turn.enabled && g_cfg.turn.turnAnim) || g_cfg.look.enabled || g_cfg.strafeAsRun) {
		// mov edx,[ecx+94] ; push esi ; mov esi,[esp+8] ; mov eax,esi ; shr eax,0A
		const uint8_t smf[] = { 0x8B, 0x91, 0x94, 0x00, 0x00, 0x00, 0x56, 0x8B, 0x74, 0x24, 0x08, 0x8B, 0xC6, 0xC1, 0xE8, 0x0A };
		g_origSetMovementFlags = (SetMovementFlags_t)addr::kSetMovementFlags;
		if (memcmp((const void*)addr::kSetMovementFlags, smf, sizeof(smf)) == 0 &&
		    PatchVtableSlot(addr::kPlayerMoverVtbl, addr::kSlotSetMovementFlags, addr::kSetMovementFlags,
		                    (void*)&Hook_SetMovementFlags))
			Log("hooked PlayerMover::SetMovementFlags (vtable %08X+%X)", (unsigned)addr::kPlayerMoverVtbl,
			    (unsigned)addr::kSlotSetMovementFlags);
		else
			Log("WARNING: PlayerMover::SetMovementFlags not hooked; turn animation control disabled");
	}
	return true;
}

}  // extern "C"
