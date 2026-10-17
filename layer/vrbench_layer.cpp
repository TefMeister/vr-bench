// vrbench_layer.cpp -- VR Bench: an OpenXR API layer that gives a game pretend hands, buttons and sticks from a file.
//
// Why (Tefa, 2026-10-17): a VR game should be testable and learnable with nobody in the headset, in the background,
// on a PC that has no headset at all. The OpenXR Simulator plays the headset; this layer plays the hands.
//
// How: the layer sits between the game and the runtime (the simulator or a real headset), registered by the Lanes
// plugin per run and only for the game's bitness. At every xrSyncActions it checks the switch
// (%LOCALAPPDATA%\VRBench\enabled exists = on) and re-reads %LOCALAPPDATA%\VRBench\input.json when its time changed
// (vb_input.h: version 1, both hands' poses, triggers, squeezes, sticks and buttons, each optional). Then:
// - xrSuggestInteractionProfileBindings is remembered, so each action's meaning (its binding path) is known.
// - xrGetActionStateBoolean / Float / Vector2f: the runtime answers first; when the file has a value for the action's
//   binding on the asked hand, SET replaces the answer and ADD puts the file on top (vb::mix_*). isActive becomes
//   true, changedSinceLastSync is true for the whole sync period after the value changed.
// - xrGetActionStatePose: a grip or aim pose set in the file makes the hand active.
// - xrLocateSpace / xrLocateSpaces: a controller space whose action is bound to grip/pose or aim/pose with the pose
//   set is answered as (base relative to our LOCAL "room" space, live)^-1 x (file pose x the space's own offset), the
//   way Hand Freeze holds a pose: the file pose is fixed in the room, so a game asking relative to the head sees it
//   stay put while the simulator's head moves. Velocities are zeroed.
// - xrApplyHapticFeedback is logged (rate-limited per hand) and passed on.
// Anything the file does not mention, and everything while the switch is off, is the runtime's own answer, untouched.
// The head is never touched: that is the simulator's job (a layer cannot move the runtime's views).
#define XR_NO_PROTOTYPES
#include <windows.h>
#include <openxr/openxr.h>
#include <openxr/openxr_loader_negotiation.h>

#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <type_traits>
#include <vector>

#include "vb_input.h"
#include "vb_log.h"

namespace {

PFN_xrGetInstanceProcAddr g_next_gipa = nullptr;
PFN_xrCreateActionSpace g_next_create_action_space = nullptr;
PFN_xrCreateReferenceSpace g_next_create_reference_space = nullptr;
PFN_xrDestroySpace g_next_destroy_space = nullptr;
PFN_xrDestroySession g_next_destroy_session = nullptr;
PFN_xrLocateSpace g_next_locate_space = nullptr;
PFN_xrLocateSpaces g_next_locate_spaces = nullptr;
PFN_xrLocateSpacesKHR g_next_locate_spaces_khr = nullptr;
PFN_xrPathToString g_next_path_to_string = nullptr;
PFN_xrStringToPath g_next_string_to_path = nullptr;
PFN_xrGetCurrentInteractionProfile g_next_current_profile = nullptr;
PFN_xrSuggestInteractionProfileBindings g_next_suggest_bindings = nullptr;
PFN_xrSyncActions g_next_sync_actions = nullptr;
PFN_xrGetActionStateBoolean g_next_state_bool = nullptr;
PFN_xrGetActionStateFloat g_next_state_float = nullptr;
PFN_xrGetActionStateVector2f g_next_state_vec2 = nullptr;
PFN_xrGetActionStatePose g_next_state_pose = nullptr;
PFN_xrApplyHapticFeedback g_next_haptic = nullptr;
XrInstance g_instance = XR_NULL_HANDLE;

struct Binding { XrPath profile; std::string path; };
struct Controller { XrSession session; XrAction action; XrPath subaction; XrPosef offset; };   // offset: poseInActionSpace
struct Last { bool have = false; bool b = false; float f = 0; float vx = 0, vy = 0; unsigned changed_at_sync = 0; };

std::recursive_mutex g_mtx;
std::map<XrAction, std::vector<Binding>> g_bindings;        // every binding the game suggested, per action
std::map<XrSpace, Controller> g_controllers;                // every space made by xrCreateActionSpace
std::map<XrSession, XrSpace> g_room;                        // our own LOCAL space per session: the frame file poses live in
std::map<std::pair<XrAction, XrPath>, Last> g_last;         // the last answer given per (action, subaction path)
XrPath g_profile[2] = {};                                   // the interaction profile in use per hand, asked at syncs
XrPath g_hand_path[2] = {};                                 // /user/hand/left, /user/hand/right
vb::Input g_input{};                                        // the file as last read; valid == false until a good read
bool g_enabled = false;
unsigned g_sync_count = 0;
FILETIME g_file_time{};
bool g_file_time_known = false;
bool g_logged_bad = false;
int g_logged_poses = -1, g_logged_buttons = -1;             // the modes last logged, so re-reads log only on a change
XrTime g_last_time = 0;                                     // the newest time a locate call used: "now" for lastChangeTime
ULONGLONG g_last_haptic_ms[2] = {};

constexpr unsigned kProfileAskEverySyncs = 30;              // how often the profile in use is asked for again
constexpr ULONGLONG kHapticLogGapMs = 250;                  // at most one haptic log line per hand this often
constexpr XrSpaceLocationFlags kValid = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
constexpr XrSpaceLocationFlags kTracked = kValid | XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
const char* const kModeName[] = { "pass", "set", "add" };

// ---- pose maths (unit quaternions, metres; Hand Freeze's) -----------------------------------------------------
XrQuaternionf qmul(XrQuaternionf a, XrQuaternionf b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
XrQuaternionf qinv(XrQuaternionf q) { return {-q.x, -q.y, -q.z, q.w}; }
XrVector3f rot(XrQuaternionf q, XrVector3f v) {
    const XrQuaternionf r = qmul(qmul(q, {v.x, v.y, v.z, 0}), qinv(q));
    return {r.x, r.y, r.z};
}
// pose of B in A's frame, given both in the room: inverse(a) x b
XrPosef relative(const XrPosef& a, const XrPosef& b) {
    const XrQuaternionf ia = qinv(a.orientation);
    const XrVector3f d{b.position.x - a.position.x, b.position.y - a.position.y, b.position.z - a.position.z};
    return {qmul(ia, b.orientation), rot(ia, d)};
}
// `b` placed inside `a`'s frame: a x b (the action space's own offset inside the hand pose)
XrPosef compose(const XrPosef& a, const XrPosef& b) {
    const XrVector3f p = rot(a.orientation, b.position);
    return {qmul(a.orientation, b.orientation), {a.position.x + p.x, a.position.y + p.y, a.position.z + p.z}};
}

// ---- the file and the switch (all under g_mtx) ---------------------------------------------------------------
bool enabled_now() {
    wchar_t path[MAX_PATH]{};
    return vb::state_path(L"enabled", path, MAX_PATH) && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

void read_file_if_changed() {
    wchar_t path[MAX_PATH]{};
    if (!vb::state_path(L"input.json", path, MAX_PATH)) return;
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path, GetFileExInfoStandard, &a)) return;   // no file yet: keep what we have
    if (g_file_time_known && CompareFileTime(&a.ftLastWriteTime, &g_file_time) == 0) return;
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;   // mid-replace: next sync
    std::string text;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(h, buf, sizeof buf, &n, nullptr) && n > 0) text.append(buf, n);
    CloseHandle(h);
    g_file_time = a.ftLastWriteTime;
    g_file_time_known = true;
    if (!vb::parse(text.data(), text.size(), g_input)) {
        if (!g_logged_bad) vb::vb_log("input.json could not be read (bad json or not v1): keeping the last good one");
        g_logged_bad = true;
        return;
    }
    g_logged_bad = false;
    if (g_logged_poses != g_input.poses || g_logged_buttons != g_input.buttons) {
        vb::vb_log("input.json v%d: poses %s, buttons %s", g_input.version, kModeName[g_input.poses], kModeName[g_input.buttons]);
        g_logged_poses = g_input.poses;
        g_logged_buttons = g_input.buttons;
    }
}

void log_game() {
    wchar_t path[MAX_PATH]{};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    const wchar_t* name = wcsrchr(path, L'\\');
    vb::vb_log("on, game %ls, %d-bit", name ? name + 1 : path, static_cast<int>(sizeof(void*) * 8));
}

// ---- bindings and hands ---------------------------------------------------------------------------------------
// 0 left, 1 right, -1 cannot tell
int hand_of_path(XrPath path) {
    if (path == XR_NULL_PATH || g_next_path_to_string == nullptr) return -1;
    char text[XR_MAX_PATH_LENGTH]{};
    uint32_t n = 0;
    if (XR_FAILED(g_next_path_to_string(g_instance, path, XR_MAX_PATH_LENGTH, &n, text))) return -1;
    if (std::strncmp(text, "/user/hand/left", 15) == 0) return 0;
    if (std::strncmp(text, "/user/hand/right", 16) == 0) return 1;
    return -1;
}

// the binding path of `action` on hand `h`, preferring the profile in use for that hand; nullptr if none
const std::string* binding_for(XrAction action, int h) {
    auto it = g_bindings.find(action);
    if (it == g_bindings.end()) return nullptr;
    const char* prefix = h == 0 ? "/user/hand/left/" : "/user/hand/right/";
    const size_t n = std::strlen(prefix);
    const std::string* any = nullptr;
    for (const auto& b : it->second) {
        if (b.path.compare(0, n, prefix) != 0) continue;
        if (b.profile == g_profile[h]) return &b.path;
        any = &b.path;
    }
    return any;
}

// the file's value for `action` asked with `subaction`: the asked hand, or either hand when none was asked
vb::Kind file_value(XrAction action, XrPath subaction, int& hand, float& f, bool& b, float& vx, float& vy) {
    const int want = hand_of_path(subaction);
    for (int h = 0; h < 2; ++h) {
        if (want >= 0 && h != want) continue;
        const std::string* p = binding_for(action, h);
        if (p == nullptr) continue;
        const vb::Kind k = vb::lookup(g_input, p->c_str(), hand, f, b, vx, vy);
        if (k != vb::NONE) return k;
    }
    return vb::NONE;
}

bool buttons_on() { return g_enabled && g_input.valid && g_input.buttons != vb::Input::PASS; }
bool poses_on() { return g_enabled && g_input.valid && g_input.poses == vb::Input::SET; }

// true for the whole sync period after the answer changed
template <class V>
bool note_change(const XrActionStateGetInfo* info, const V& value, V Last::*slot) {
    Last& last = g_last[{info->action, info->subactionPath}];
    if (!last.have || last.*slot != value) last.changed_at_sync = g_sync_count;
    last.have = true;
    last.*slot = value;
    return last.changed_at_sync == g_sync_count;
}

// why a controller pose was or was not answered from the file; logged only when it changes, so a run that never
// takes effect says why in one line instead of leaving the log silent
enum PoseWhy { WHY_ANSWERED, WHY_NO_ROOM, WHY_NO_BINDING, WHY_NOT_SET, WHY_BASE_LOST, WHY_COUNT };
const char* const kWhyText[] = { "answered from the file", "no room space for the session",
                                 "the action has no binding we know", "the file sets no pose for that hand",
                                 "the base space could not be located" };
int g_last_why[2] = { -1, -1 };   // per hand slot (0 left, 1 right / unknown)

void note_why(XrAction action, int hand, int why) {
    const int slot = hand == 0 ? 0 : 1;
    if (g_last_why[slot] == why) return;
    g_last_why[slot] = why;
    size_t bound = 0;
    auto it = g_bindings.find(action);
    if (it != g_bindings.end()) bound = it->second.size();
    vb::vb_log("pose %s: %s (%zu bindings for this action, %zu actions bound, %zu controller spaces)",
               hand == 0 ? "left" : hand == 1 ? "right" : "unknown hand", kWhyText[why], bound, g_bindings.size(),
               g_controllers.size());
}

// the file pose for a controller space relative to `base`; false = answer live
bool file_pose(XrSpace space, XrSpace base, XrTime time, XrPosef& out) {
    if (!poses_on()) return false;
    auto c = g_controllers.find(space);
    if (c == g_controllers.end() || g_controllers.count(base) != 0) return false;
    int hand = -1; float f = 0, vx = 0, vy = 0; bool b = false;
    auto r = g_room.find(c->second.session);
    if (r == g_room.end()) { note_why(c->second.action, hand, WHY_NO_ROOM); return false; }
    const vb::Kind k = file_value(c->second.action, c->second.subaction, hand, f, b, vx, vy);
    if (k != vb::POSE_GRIP && k != vb::POSE_AIM) {
        const bool known = binding_for(c->second.action, 0) != nullptr || binding_for(c->second.action, 1) != nullptr;
        if (!known) note_why(c->second.action, hand, WHY_NO_BINDING);
        return false;   // a bound hand the file leaves alone is normal (the other hand), so it is not logged
    }
    const vb::Pose& p = k == vb::POSE_GRIP ? g_input.hand[hand].grip : g_input.hand[hand].aim;
    const XrPosef hand_in_room{{p.qx, p.qy, p.qz, p.qw}, {p.px, p.py, p.pz}};
    const XrPosef space_in_room = compose(hand_in_room, c->second.offset);
    XrSpaceLocation bl{XR_TYPE_SPACE_LOCATION};   // where the base is in the room right now (the head keeps moving)
    if (XR_FAILED(g_next_locate_space(base, r->second, time, &bl)) || (bl.locationFlags & kValid) != kValid) {
        note_why(c->second.action, hand, WHY_BASE_LOST);
        return false;
    }
    note_why(c->second.action, hand, WHY_ANSWERED);
    out = relative(bl.pose, space_in_room);
    return true;
}

void zero_velocity(XrSpaceVelocity* v) { v->linearVelocity = {0, 0, 0}; v->angularVelocity = {0, 0, 0}; }

// ---- the hooked functions -------------------------------------------------------------------------------------
XRAPI_ATTR XrResult XRAPI_CALL vbl_xrSuggestInteractionProfileBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding* suggested) {
    const XrResult r = g_next_suggest_bindings(instance, suggested);
    if (XR_FAILED(r) || suggested == nullptr || suggested->suggestedBindings == nullptr || g_next_path_to_string == nullptr) return r;
    std::lock_guard _{g_mtx};
    for (uint32_t i = 0; i < suggested->countSuggestedBindings; ++i) {
        char text[XR_MAX_PATH_LENGTH]{};
        uint32_t n = 0;
        if (XR_FAILED(g_next_path_to_string(instance, suggested->suggestedBindings[i].binding, XR_MAX_PATH_LENGTH, &n, text))) continue;
        g_bindings[suggested->suggestedBindings[i].action].push_back({suggested->interactionProfile, text});
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL vbl_xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo* info, XrSpace* space) {
    const XrResult r = g_next_create_action_space(session, info, space);
    if (XR_FAILED(r) || space == nullptr) return r;
    std::lock_guard _{g_mtx};
    Controller c{};
    c.session = session;
    c.action = info != nullptr ? info->action : XR_NULL_HANDLE;
    c.subaction = info != nullptr ? info->subactionPath : XR_NULL_PATH;
    c.offset = info != nullptr ? info->poseInActionSpace : XrPosef{{0, 0, 0, 1}, {0, 0, 0}};
    g_controllers[*space] = c;
    if (g_room.count(session) == 0 && g_next_create_reference_space != nullptr) {
        XrReferenceSpaceCreateInfo ci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        ci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        ci.poseInReferenceSpace.orientation.w = 1.0f;
        XrSpace room = XR_NULL_HANDLE;
        if (XR_SUCCEEDED(g_next_create_reference_space(session, &ci, &room))) g_room[session] = room;
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL vbl_xrDestroySpace(XrSpace space) {
    {
        std::lock_guard _{g_mtx};
        g_controllers.erase(space);
    }
    return g_next_destroy_space(space);
}

XRAPI_ATTR XrResult XRAPI_CALL vbl_xrDestroySession(XrSession session) {
    {
        std::lock_guard _{g_mtx};
        g_room.erase(session);   // the runtime destroys the session's spaces with it
        for (auto it = g_controllers.begin(); it != g_controllers.end();) {
            if (it->second.session == session) it = g_controllers.erase(it); else ++it;
        }
        g_last.clear();
    }
    return g_next_destroy_session(session);
}

XRAPI_ATTR XrResult XRAPI_CALL vbl_xrSyncActions(XrSession session, const XrActionsSyncInfo* info) {
    const XrResult r = g_next_sync_actions(session, info);
    std::lock_guard _{g_mtx};
    ++g_sync_count;
    const bool on = enabled_now();
    if (on != g_enabled) {
        g_enabled = on;
        if (on) { log_game(); g_file_time_known = false; } else { vb::vb_log("off"); g_last.clear(); }
    }
    if (!g_enabled) return r;
    read_file_if_changed();
    if (g_next_current_profile != nullptr && (g_sync_count % kProfileAskEverySyncs) == 1) {
        for (int h = 0; h < 2; ++h) {
            if (g_hand_path[h] == XR_NULL_PATH) continue;
            XrInteractionProfileState st{XR_TYPE_INTERACTION_PROFILE_STATE};
            if (XR_SUCCEEDED(g_next_current_profile(session, g_hand_path[h], &st))) g_profile[h] = st.interactionProfile;
        }
    }
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL vbl_xrGetActionStateBoolean(XrSession session, const XrActionStateGetInfo* info, XrActionStateBoolean* state) {
    const XrResult r = g_next_state_bool(session, info, state);
    if (XR_FAILED(r) || state == nullptr || info == nullptr) return r;
    std::lock_guard _{g_mtx};
    if (!buttons_on()) return r;
    int hand = -1; float f = 0, vx = 0, vy = 0; bool b = false;
    const vb::Kind k = file_value(info->action, info->subactionPath, hand, f, b, vx, vy);
    if (k != vb::BOOL && k != vb::FLOAT) return r;
    const bool value = vb::mix_bool(g_input.buttons, state->isActive == XR_TRUE, state->currentState == XR_TRUE, vb::as_bool(k, f, b));
    const bool changed = note_change(info, value, &Last::b);
    state->isActive = XR_TRUE;
    state->currentState = value ? XR_TRUE : XR_FALSE;
    state->changedSinceLastSync = changed ? XR_TRUE : XR_FALSE;
    if (changed && g_last_time != 0) state->lastChangeTime = g_last_time;
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL vbl_xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo* info, XrActionStateFloat* state) {
    const XrResult r = g_next_state_float(session, info, state);
    if (XR_FAILED(r) || state == nullptr || info == nullptr) return r;
    std::lock_guard _{g_mtx};
    if (!buttons_on()) return r;
    int hand = -1; float f = 0, vx = 0, vy = 0; bool b = false;
    const vb::Kind k = file_value(info->action, info->subactionPath, hand, f, b, vx, vy);
    if (k != vb::BOOL && k != vb::FLOAT) return r;
    const float value = vb::mix_float(g_input.buttons, state->isActive == XR_TRUE, state->currentState, vb::as_float(k, f, b));
    const bool changed = note_change(info, value, &Last::f);
    state->isActive = XR_TRUE;
    state->currentState = value;
    state->changedSinceLastSync = changed ? XR_TRUE : XR_FALSE;
    if (changed && g_last_time != 0) state->lastChangeTime = g_last_time;
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL vbl_xrGetActionStateVector2f(XrSession session, const XrActionStateGetInfo* info, XrActionStateVector2f* state) {
    const XrResult r = g_next_state_vec2(session, info, state);
    if (XR_FAILED(r) || state == nullptr || info == nullptr) return r;
    std::lock_guard _{g_mtx};
    if (!buttons_on()) return r;
    int hand = -1; float f = 0, vx = 0, vy = 0; bool b = false;
    if (file_value(info->action, info->subactionPath, hand, f, b, vx, vy) != vb::VEC2) return r;
    float ox = 0, oy = 0;
    vb::mix_vec2(g_input.buttons, state->isActive == XR_TRUE, state->currentState.x, state->currentState.y, vx, vy, ox, oy);
    const bool cx = note_change(info, ox, &Last::vx);
    const bool cy = note_change(info, oy, &Last::vy);
    state->isActive = XR_TRUE;
    state->currentState = {ox, oy};
    state->changedSinceLastSync = (cx || cy) ? XR_TRUE : XR_FALSE;
    if ((cx || cy) && g_last_time != 0) state->lastChangeTime = g_last_time;
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL vbl_xrGetActionStatePose(XrSession session, const XrActionStateGetInfo* info, XrActionStatePose* state) {
    const XrResult r = g_next_state_pose(session, info, state);
    if (XR_FAILED(r) || state == nullptr || info == nullptr) return r;
    std::lock_guard _{g_mtx};
    if (!poses_on()) return r;
    int hand = -1; float f = 0, vx = 0, vy = 0; bool b = false;
    const vb::Kind k = file_value(info->action, info->subactionPath, hand, f, b, vx, vy);
    if (k == vb::POSE_GRIP || k == vb::POSE_AIM) state->isActive = XR_TRUE;
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL vbl_xrLocateSpace(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* location) {
    const XrResult r = g_next_locate_space(space, base, time, location);
    std::lock_guard _{g_mtx};
    static bool logged_first = false;
    if (!logged_first && g_enabled && g_controllers.count(space) != 0) {
        logged_first = true;
        vb::vb_log("first controller locate: runtime said %d, flags 0x%llx", static_cast<int>(r),
                   location ? static_cast<unsigned long long>(location->locationFlags) : 0ull);
    }
    if (XR_FAILED(r) || location == nullptr) return r;
    if (time > g_last_time) g_last_time = time;
    XrPosef pose{};
    if (!file_pose(space, base, time, pose)) return r;
    location->pose = pose;
    location->locationFlags |= kTracked;
    for (auto* n = static_cast<XrBaseOutStructure*>(location->next); n != nullptr; n = n->next) {
        if (n->type == XR_TYPE_SPACE_VELOCITY) zero_velocity(reinterpret_cast<XrSpaceVelocity*>(n));
    }
    return r;
}

XrResult locate_spaces(PFN_xrLocateSpaces next, XrSession session, const XrSpacesLocateInfo* info, XrSpaceLocations* out) {
    const XrResult r = next(session, info, out);
    if (XR_FAILED(r) || info == nullptr || out == nullptr || out->locations == nullptr) return r;
    std::lock_guard _{g_mtx};
    if (info->time > g_last_time) g_last_time = info->time;
    if (!poses_on()) return r;
    XrSpaceVelocities* vel = nullptr;
    for (auto* n = static_cast<XrBaseOutStructure*>(out->next); n != nullptr; n = n->next) {
        if (n->type == XR_TYPE_SPACE_VELOCITIES) vel = reinterpret_cast<XrSpaceVelocities*>(n);
    }
    const uint32_t count = info->spaceCount < out->locationCount ? info->spaceCount : out->locationCount;
    for (uint32_t i = 0; i < count; ++i) {
        XrPosef pose{};
        if (!file_pose(info->spaces[i], info->baseSpace, info->time, pose)) continue;
        out->locations[i].pose = pose;
        out->locations[i].locationFlags |= kTracked;
        if (vel != nullptr && vel->velocities != nullptr && i < vel->velocityCount) {
            vel->velocities[i].linearVelocity = {0, 0, 0};
            vel->velocities[i].angularVelocity = {0, 0, 0};
        }
    }
    return r;
}
XRAPI_ATTR XrResult XRAPI_CALL vbl_xrLocateSpaces(XrSession s, const XrSpacesLocateInfo* i, XrSpaceLocations* o) { return locate_spaces(g_next_locate_spaces, s, i, o); }
XRAPI_ATTR XrResult XRAPI_CALL vbl_xrLocateSpacesKHR(XrSession s, const XrSpacesLocateInfo* i, XrSpaceLocations* o) { return locate_spaces(g_next_locate_spaces_khr, s, i, o); }

XRAPI_ATTR XrResult XRAPI_CALL vbl_xrApplyHapticFeedback(XrSession session, const XrHapticActionInfo* info, const XrHapticBaseHeader* haptic) {
    const XrResult r = g_next_haptic(session, info, haptic);
    if (info == nullptr) return r;
    std::lock_guard _{g_mtx};
    if (!g_enabled) return r;
    int hand = hand_of_path(info->subactionPath);
    if (hand < 0) {
        for (int h = 0; h < 2 && hand < 0; ++h) if (binding_for(info->action, h) != nullptr) hand = h;
    }
    const int slot = hand < 0 ? 0 : hand;
    const ULONGLONG now = GetTickCount64();
    if (now - g_last_haptic_ms[slot] < kHapticLogGapMs) return r;
    g_last_haptic_ms[slot] = now;
    long long ms = -1;
    if (haptic != nullptr && haptic->type == XR_TYPE_HAPTIC_VIBRATION) {
        const auto* v = reinterpret_cast<const XrHapticVibration*>(haptic);
        ms = v->duration < 0 ? 0 : static_cast<long long>(v->duration / 1000000);   // XR_MIN_HAPTIC_DURATION = the shortest
    }
    vb::vb_log("haptic %s %lld ms", hand == 0 ? "left" : hand == 1 ? "right" : "unknown-hand", ms);
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL vbl_xrGetInstanceProcAddr(XrInstance instance, const char* name, PFN_xrVoidFunction* function) {
    const XrResult r = g_next_gipa(instance, name, function);
    if (XR_FAILED(r) || function == nullptr || *function == nullptr || name == nullptr) return r;
    // ours only for functions the runtime really has, so a missing extension stays missing
    struct Hook { const char* name; PFN_xrVoidFunction ours; PFN_xrVoidFunction* next; };
    static const Hook hooks[] = {
        {"xrSuggestInteractionProfileBindings", (PFN_xrVoidFunction)vbl_xrSuggestInteractionProfileBindings, (PFN_xrVoidFunction*)&g_next_suggest_bindings},
        {"xrCreateActionSpace", (PFN_xrVoidFunction)vbl_xrCreateActionSpace, (PFN_xrVoidFunction*)&g_next_create_action_space},
        {"xrDestroySpace", (PFN_xrVoidFunction)vbl_xrDestroySpace, (PFN_xrVoidFunction*)&g_next_destroy_space},
        {"xrDestroySession", (PFN_xrVoidFunction)vbl_xrDestroySession, (PFN_xrVoidFunction*)&g_next_destroy_session},
        {"xrSyncActions", (PFN_xrVoidFunction)vbl_xrSyncActions, (PFN_xrVoidFunction*)&g_next_sync_actions},
        {"xrGetActionStateBoolean", (PFN_xrVoidFunction)vbl_xrGetActionStateBoolean, (PFN_xrVoidFunction*)&g_next_state_bool},
        {"xrGetActionStateFloat", (PFN_xrVoidFunction)vbl_xrGetActionStateFloat, (PFN_xrVoidFunction*)&g_next_state_float},
        {"xrGetActionStateVector2f", (PFN_xrVoidFunction)vbl_xrGetActionStateVector2f, (PFN_xrVoidFunction*)&g_next_state_vec2},
        {"xrGetActionStatePose", (PFN_xrVoidFunction)vbl_xrGetActionStatePose, (PFN_xrVoidFunction*)&g_next_state_pose},
        {"xrLocateSpace", (PFN_xrVoidFunction)vbl_xrLocateSpace, (PFN_xrVoidFunction*)&g_next_locate_space},
        {"xrLocateSpaces", (PFN_xrVoidFunction)vbl_xrLocateSpaces, (PFN_xrVoidFunction*)&g_next_locate_spaces},
        {"xrLocateSpacesKHR", (PFN_xrVoidFunction)vbl_xrLocateSpacesKHR, (PFN_xrVoidFunction*)&g_next_locate_spaces_khr},
        {"xrApplyHapticFeedback", (PFN_xrVoidFunction)vbl_xrApplyHapticFeedback, (PFN_xrVoidFunction*)&g_next_haptic},
    };
    for (const auto& h : hooks) {
        if (std::strcmp(name, h.name) == 0) { *h.next = *function; *function = h.ours; break; }
    }
    return r;
}

// the functions the layer itself calls, fetched once the instance exists (the game may never ask for some of them)
void fetch_next(XrInstance instance) {
    auto get = [&](const char* n, auto& slot) {
        PFN_xrVoidFunction f = nullptr;
        if (slot == nullptr && XR_SUCCEEDED(g_next_gipa(instance, n, &f)) && f != nullptr) slot = reinterpret_cast<std::remove_reference_t<decltype(slot)>>(f);
    };
    get("xrCreateReferenceSpace", g_next_create_reference_space);
    get("xrLocateSpace", g_next_locate_space);
    get("xrCreateActionSpace", g_next_create_action_space);
    get("xrDestroySpace", g_next_destroy_space);
    get("xrDestroySession", g_next_destroy_session);
    get("xrPathToString", g_next_path_to_string);
    get("xrStringToPath", g_next_string_to_path);
    get("xrGetCurrentInteractionProfile", g_next_current_profile);
    get("xrSuggestInteractionProfileBindings", g_next_suggest_bindings);
    get("xrSyncActions", g_next_sync_actions);
    get("xrGetActionStateBoolean", g_next_state_bool);
    get("xrGetActionStateFloat", g_next_state_float);
    get("xrGetActionStateVector2f", g_next_state_vec2);
    get("xrGetActionStatePose", g_next_state_pose);
    get("xrApplyHapticFeedback", g_next_haptic);
    g_instance = instance;
    if (g_next_string_to_path != nullptr) {
        g_next_string_to_path(instance, "/user/hand/left", &g_hand_path[0]);
        g_next_string_to_path(instance, "/user/hand/right", &g_hand_path[1]);
    }
}

XRAPI_ATTR XrResult XRAPI_CALL vbl_xrCreateApiLayerInstance(const XrInstanceCreateInfo* info, const XrApiLayerCreateInfo* layer_info, XrInstance* instance) {
    if (layer_info == nullptr || layer_info->nextInfo == nullptr) return XR_ERROR_INITIALIZATION_FAILED;
    g_next_gipa = layer_info->nextInfo->nextGetInstanceProcAddr;
    XrApiLayerCreateInfo next_info = *layer_info;   // the next layer down gets the chain one step further on
    next_info.nextInfo = layer_info->nextInfo->next;
    const XrResult r = layer_info->nextInfo->nextCreateApiLayerInstance(info, &next_info, instance);
    if (XR_SUCCEEDED(r) && instance != nullptr) fetch_next(*instance);
    return r;
}

} // namespace

// 32-bit build: XRAPI_CALL is __stdcall there, which decorates the export to _xrNegotiate...@12, and the loader then
// says "negotiation function xrNegotiateLoaderApiLayerInterface was not found" (Hand Freeze, 2026-10-14). Export the
// plain name too.
#if defined(_M_IX86)
#pragma comment(linker, "/EXPORT:xrNegotiateLoaderApiLayerInterface=_xrNegotiateLoaderApiLayerInterface@12")
#endif
extern "C" __declspec(dllexport) XrResult XRAPI_CALL xrNegotiateLoaderApiLayerInterface(const XrNegotiateLoaderInfo* loader_info, const char* /*layer_name*/, XrNegotiateApiLayerRequest* request) {
    if (loader_info == nullptr || request == nullptr || loader_info->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        request->structType != XR_LOADER_INTERFACE_STRUCT_API_LAYER_REQUEST ||
        loader_info->minInterfaceVersion > XR_CURRENT_LOADER_API_LAYER_VERSION || loader_info->maxInterfaceVersion < XR_CURRENT_LOADER_API_LAYER_VERSION) {
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    request->layerInterfaceVersion = XR_CURRENT_LOADER_API_LAYER_VERSION;
    request->layerApiVersion = loader_info->maxApiVersion < XR_CURRENT_API_VERSION ? loader_info->maxApiVersion : XR_CURRENT_API_VERSION;
    request->getInstanceProcAddr = vbl_xrGetInstanceProcAddr;
    request->createApiLayerInstance = vbl_xrCreateApiLayerInstance;
    return XR_SUCCESS;
}
