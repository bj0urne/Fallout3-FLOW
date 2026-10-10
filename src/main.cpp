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
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cstring>

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
	constexpr uintptr_t kSlotSetMovementFlags = 0x0C;
	// Game state checks, taken from the script condition functions IsWeaponOut (0x510E50 ->
	// Actor::IsWeaponOut 0x6F6C70), GetSitting (0x50F9D0) and GetKnockedState (0x510EC0).
	constexpr uintptr_t kIsWeaponOut        = 0x006F6C70;  // process vfunc +0x450
	constexpr uintptr_t kGetSittingUse      = 0x0050F9F2;  // actor vfunc +0x214
	constexpr uintptr_t kGetKnockedUse      = 0x00510F00;  // process vfunc +0x408
	constexpr uintptr_t kVanityCamera       = 0x0107BA5B;  // bool, hold-POV / idle vanity camera
}

namespace vslot {
	constexpr uintptr_t kProcessIsWeaponOut = 0x450;
	constexpr uintptr_t kProcessKnockState  = 0x408;
	constexpr uintptr_t kActorSitSleepState = 0x214;
}

namespace off {
	constexpr uintptr_t kRefrRotX        = 0x20;   // TESObjectREFR pitch, radians (look up/down)
	constexpr uintptr_t kRefrRotZ        = 0x28;   // TESObjectREFR heading, radians
	constexpr uintptr_t kRefrPos         = 0x2C;   // TESObjectREFR position (feet for actors)
	constexpr uintptr_t kActorProcess    = 0x60;   // Actor -> BaseProcess
	constexpr uintptr_t kRefrRenderState = 0x5C;   // TESObjectREFR -> render state
	constexpr uintptr_t kRenderStateNode = 0x14;   // render state -> 3rd person root NiNode
	constexpr uintptr_t kPlayerMover     = 0x184;  // PlayerCharacter -> PlayerMover
	constexpr uintptr_t kMoverFlags      = 0x94;   // PlayerMover -> movement flags
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
	float strafeAngle    = 90.0f;   // degrees the legs turn for that
	TurnConfig turn;
	FreeLookConfig look;
	MoveConfig move;
	CameraConfig cam;
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
	NiPoint3    pivot   = {};       // legs world position when it was applied
	int64_t     lastQpc = 0;
	uint32_t    lastFlags = 0;
} g;

static double g_qpcFreq;

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

static float TargetAngle(uint32_t flags)
{
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

static float WrapPi(float a)
{
	while (a > kPi) a -= 2.0f * kPi;
	while (a < -kPi) a += 2.0f * kPi;
	return a;
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
	if (!(dt > 0.0f && dt < 0.25f)) {  // paused or after a hitch/load: start fresh
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
	// Weapon drawn, pure strafe: tell the game "forward" so it plays the forward walk/run instead
	// of the strafe animation; the legs are then turned 90 degrees (TargetAngle) and the movement
	// vector is turned to match (Hook_AnimMoveVector). The real keys are kept in g.strafeKeys so
	// the rest of STRIDE still sees a strafe.
	const uint32_t dir = flags & kMove_Direction;
	const bool pureStrafe = (dir == kMove_Left || dir == kMove_Right);
	g.strafeKeys = 0;
	if (g_cfg.strafeAsRun && g_strafeMoveHooked && pureStrafe && g.legs && g_stateChecksOk &&
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

static void __fastcall Hook_UpdateWorldData(NiAVObject* self, void* /*edx*/, void* updateData)
{
	g_origUpdateWorldData(self, updateData);

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
	if (node && InThirdPerson()) SmoothCamera(node, dt);
	else cam.init = false;
	((NiAVObjectUpdate_t)addr::kNiAVObjectUpdate)(node, updateData);
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
		g_stateChecksOk = memcmp((const void*)addr::kIsWeaponOut, weapon, sizeof(weapon)) == 0 &&
		                  memcmp((const void*)addr::kGetSittingUse, sit, sizeof(sit)) == 0 &&
		                  memcmp((const void*)addr::kGetKnockedUse, knock, sizeof(knock)) == 0;
		if (!g_stateChecksOk && g_cfg.look.enabled)
			Log("WARNING: game state functions do not match; free look disabled");
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
