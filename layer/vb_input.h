// vb_input.h -- the pure part of the VR Bench layer: read input.json (version 1) and look a binding path up in it.
//
// No Windows, no OpenXR types: test/vb_input_test.cpp builds this on its own and runs it without a runtime or a
// headset. The layer (vrbench_layer.cpp) includes it and does the OpenXR side.
//
// THE FILE (written by the plugin's tools/vrbench/vb_input.py; replaced atomically, re-read when its time changes):
//   {"v": 1,
//    "mode": {"poses": "set" | "pass", "buttons": "set" | "pass" | "add"},
//    "vr": {"left":  {"grip_pose": {"p": [x,y,z], "q": [x,y,z,w]}, "aim_pose": {...},
//                     "trigger": 0..1, "squeeze": 0..1, "stick": [x,y],
//                     "a": bool, "b": bool, "x": bool, "y": bool, "menu": bool, "stick_click": bool},
//           "right": {...}}}
//   Every value is optional: a value that is not in the file is "not set", and the layer passes the runtime's own
//   answer through for it. Poses are in the LOCAL reference space, metres, like the simulator's own command files.
//
// parse() fills `out` only when the whole file is good JSON with v == 1; on anything else it returns false and
// leaves `out` exactly as it was, so a half-written file keeps the last good state (plan 1, review focus 2).
#pragma once
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vb {

struct Pose { float px, py, pz, qx, qy, qz, qw; bool set; };

struct Hand {
    Pose grip, aim;
    float trigger, squeeze;
    float stick_x, stick_y;
    bool a, b, x, y, menu, stick_click;
    bool trigger_set, squeeze_set, stick_set, a_set, b_set, x_set, y_set, menu_set, stick_click_set;
};

struct Input {
    int version;
    bool valid;
    Hand hand[2];                       // 0 = left, 1 = right
    enum Mode { PASS, SET, ADD } poses, buttons;
};

enum Kind { NONE, BOOL, FLOAT, VEC2, POSE_GRIP, POSE_AIM };

constexpr int kVersion = 1;
constexpr float kTriggerClickAt = 0.75f;   // trigger/click is true from this value up
constexpr int kMaxDepth = 8;               // nesting the reader accepts
constexpr int kMaxPath = 96;               // longest "a.b.c[2]" key the reader builds

namespace detail {

// A tiny JSON walker: objects, arrays, numbers, bools, null, strings (no escapes beyond \" and \\ are needed).
// Every leaf is reported to the sink as ("dotted.path[index]", kind, number, bool). Returns false on bad JSON.
struct Walker {
    const char* s; const char* end;
    Input* out;
    char path[kMaxPath];
    int depth;

    void skip_ws() { while (s < end && (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')) ++s; }
    bool take(char c) { skip_ws(); if (s < end && *s == c) { ++s; return true; } return false; }

    bool string(char* buf, int room) {
        skip_ws();
        if (s >= end || *s != '"') return false;
        ++s;
        int n = 0;
        while (s < end && *s != '"') {
            char c = *s++;
            if (c == '\\' && s < end) c = *s++;
            if (n < room - 1) buf[n++] = c;
        }
        if (s >= end) return false;
        ++s;
        buf[n] = 0;
        return true;
    }

    bool number(float& f) {
        skip_ws();
        char* e = nullptr;
        f = std::strtof(s, &e);
        if (e == s || e > end) return false;
        s = e;
        return true;
    }

    bool word(const char* w) {
        const size_t n = std::strlen(w);
        if (static_cast<size_t>(end - s) >= n && std::strncmp(s, w, n) == 0) { s += n; return true; }
        return false;
    }

    void leaf(bool is_num, float f, bool b);   // defined after the struct fillers below

    // appends to `path`; every caller has checked the room just above
    void cat(const char* more) { std::memcpy(path + std::strlen(path), more, std::strlen(more) + 1); }

    bool value() {
        skip_ws();
        if (s >= end) return false;
        if (*s == '{') return object();
        if (*s == '[') return array();
        if (*s == '"') { char tmp[64]; if (!string(tmp, sizeof tmp)) return false; leaf_string(tmp); return true; }
        if (word("true")) { leaf(false, 0, true); return true; }
        if (word("false")) { leaf(false, 0, false); return true; }
        if (word("null")) return true;
        float f; if (!number(f)) return false;
        leaf(true, f, false);
        return true;
    }

    bool object() {
        if (!take('{')) return false;
        if (take('}')) return true;
        if (++depth > kMaxDepth) return false;
        const size_t base = std::strlen(path);
        for (;;) {
            char key[48];
            if (!string(key, sizeof key)) return false;
            if (!take(':')) return false;
            path[base] = 0;
            if (base + 1 + std::strlen(key) >= kMaxPath) return false;
            if (base) cat(".");
            cat(key);
            if (!value()) return false;
            path[base] = 0;
            if (take(',')) continue;
            if (take('}')) break;
            return false;
        }
        --depth;
        return true;
    }

    bool array() {
        if (!take('[')) return false;
        if (take(']')) return true;
        if (++depth > kMaxDepth) return false;
        const size_t base = std::strlen(path);
        for (int i = 0;; ++i) {
            path[base] = 0;
            if (base + 8 >= kMaxPath) return false;
            char idx[8];
            std::snprintf(idx, sizeof idx, "[%d]", i);
            cat(idx);
            if (!value()) return false;
            path[base] = 0;
            if (take(',')) continue;
            if (take(']')) break;
            return false;
        }
        --depth;
        return true;
    }

    // ---- fillers -------------------------------------------------------------------------------------------
    static bool ends(const char* p, const char* suffix) {
        const size_t a = std::strlen(p), b = std::strlen(suffix);
        return a >= b && std::strcmp(p + a - b, suffix) == 0;
    }
    static void mode(Input::Mode& m, const char* text) {
        if (std::strcmp(text, "set") == 0) m = Input::SET;
        else if (std::strcmp(text, "add") == 0) m = Input::ADD;
        else m = Input::PASS;
    }
    void leaf_string(const char* text) {
        if (std::strcmp(path, "mode.poses") == 0) mode(out->poses, text);
        else if (std::strcmp(path, "mode.buttons") == 0) mode(out->buttons, text);
    }
    static void pose_part(Pose& p, const char* rest, float f) {
        if (std::strcmp(rest, "p[0]") == 0) p.px = f; else if (std::strcmp(rest, "p[1]") == 0) p.py = f;
        else if (std::strcmp(rest, "p[2]") == 0) p.pz = f; else if (std::strcmp(rest, "q[0]") == 0) p.qx = f;
        else if (std::strcmp(rest, "q[1]") == 0) p.qy = f; else if (std::strcmp(rest, "q[2]") == 0) p.qz = f;
        else if (std::strcmp(rest, "q[3]") == 0) p.qw = f; else return;
        p.set = true;
    }
    void hand_leaf(Hand& h, const char* rest, bool is_num, float f, bool b) {
        if (std::strncmp(rest, "grip_pose.", 10) == 0) { if (is_num) pose_part(h.grip, rest + 10, f); return; }
        if (std::strncmp(rest, "aim_pose.", 9) == 0) { if (is_num) pose_part(h.aim, rest + 9, f); return; }
        if (is_num) {
            if (std::strcmp(rest, "trigger") == 0) { h.trigger = f; h.trigger_set = true; }
            else if (std::strcmp(rest, "squeeze") == 0) { h.squeeze = f; h.squeeze_set = true; }
            else if (std::strcmp(rest, "stick[0]") == 0) { h.stick_x = f; h.stick_set = true; }
            else if (std::strcmp(rest, "stick[1]") == 0) { h.stick_y = f; h.stick_set = true; }
        } else {
            if (std::strcmp(rest, "a") == 0) { h.a = b; h.a_set = true; }
            else if (std::strcmp(rest, "b") == 0) { h.b = b; h.b_set = true; }
            else if (std::strcmp(rest, "x") == 0) { h.x = b; h.x_set = true; }
            else if (std::strcmp(rest, "y") == 0) { h.y = b; h.y_set = true; }
            else if (std::strcmp(rest, "menu") == 0) { h.menu = b; h.menu_set = true; }
            else if (std::strcmp(rest, "stick_click") == 0) { h.stick_click = b; h.stick_click_set = true; }
        }
    }
};

inline void Walker::leaf(bool is_num, float f, bool b) {
    if (std::strcmp(path, "v") == 0) { if (is_num) out->version = static_cast<int>(f); return; }
    if (std::strncmp(path, "vr.left.", 8) == 0) hand_leaf(out->hand[0], path + 8, is_num, f, b);
    else if (std::strncmp(path, "vr.right.", 9) == 0) hand_leaf(out->hand[1], path + 9, is_num, f, b);
}

} // namespace detail

// Read `n` bytes of JSON. True = `out` now holds the file; false = bad JSON or not version 1, `out` untouched.
inline bool parse(const char* json, size_t n, Input& out) {
    Input tmp{};
    tmp.poses = Input::PASS;
    tmp.buttons = Input::PASS;
    detail::Walker w{json, json + n, &tmp, {0}, 0};
    if (!w.value()) return false;
    w.skip_ws();
    if (w.s != w.end) return false;        // trailing rubbish
    if (tmp.version != kVersion) return false;
    tmp.valid = true;
    out = tmp;
    return true;
}

// Which hand a binding path names: 0 left, 1 right, -1 neither. `rest` gets the part after "/input/".
inline int hand_of_path(const char* binding_path, const char*& rest) {
    static const char* const kPrefix[2] = { "/user/hand/left/input/", "/user/hand/right/input/" };
    for (int h = 0; h < 2; ++h) {
        const size_t n = std::strlen(kPrefix[h]);
        if (std::strncmp(binding_path, kPrefix[h], n) == 0) { rest = binding_path + n; return h; }
    }
    rest = nullptr;
    return -1;
}

// The file's value for one binding path. NONE = not a path we know, or that hand's value is not set in the file.
inline Kind lookup(const Input& in, const char* binding_path, int& hand, float& f, bool& b, float& vx, float& vy) {
    const char* rest = nullptr;
    hand = hand_of_path(binding_path, rest);
    if (hand < 0 || !in.valid) return NONE;
    const Hand& h = in.hand[hand];
    if (std::strcmp(rest, "grip/pose") == 0) return h.grip.set ? POSE_GRIP : NONE;
    if (std::strcmp(rest, "aim/pose") == 0) return h.aim.set ? POSE_AIM : NONE;
    if (std::strcmp(rest, "trigger/value") == 0) { if (!h.trigger_set) return NONE; f = h.trigger; return FLOAT; }
    if (std::strcmp(rest, "trigger/click") == 0) { if (!h.trigger_set) return NONE; b = h.trigger >= kTriggerClickAt; return BOOL; }
    if (std::strcmp(rest, "squeeze/value") == 0 || std::strcmp(rest, "squeeze/force") == 0) { if (!h.squeeze_set) return NONE; f = h.squeeze; return FLOAT; }
    if (std::strcmp(rest, "squeeze/click") == 0) { if (!h.squeeze_set) return NONE; b = h.squeeze >= kTriggerClickAt; return BOOL; }
    if (std::strcmp(rest, "thumbstick") == 0 || std::strcmp(rest, "trackpad") == 0) { if (!h.stick_set) return NONE; vx = h.stick_x; vy = h.stick_y; return VEC2; }
    if (std::strcmp(rest, "thumbstick/x") == 0 || std::strcmp(rest, "trackpad/x") == 0) { if (!h.stick_set) return NONE; f = h.stick_x; return FLOAT; }
    if (std::strcmp(rest, "thumbstick/y") == 0 || std::strcmp(rest, "trackpad/y") == 0) { if (!h.stick_set) return NONE; f = h.stick_y; return FLOAT; }
    if (std::strcmp(rest, "thumbstick/click") == 0 || std::strcmp(rest, "trackpad/click") == 0) { if (!h.stick_click_set) return NONE; b = h.stick_click; return BOOL; }
    if (std::strcmp(rest, "a/click") == 0) { if (!h.a_set) return NONE; b = h.a; return BOOL; }
    if (std::strcmp(rest, "b/click") == 0) { if (!h.b_set) return NONE; b = h.b; return BOOL; }
    if (std::strcmp(rest, "x/click") == 0) { if (!h.x_set) return NONE; b = h.x; return BOOL; }
    if (std::strcmp(rest, "y/click") == 0) { if (!h.y_set) return NONE; b = h.y; return BOOL; }
    if (std::strcmp(rest, "menu/click") == 0 || std::strcmp(rest, "select/click") == 0) { if (!h.menu_set) return NONE; b = h.menu; return BOOL; }
    return NONE;
}

// ---- how a file value meets the runtime's own ("live") answer ---------------------------------------------------
// OpenXR lets a button action sit on a trigger/value binding and a float action on a button, so a value is first
// shaped to what the game asked for; then SET replaces the live answer and ADD puts the file on top of it. A live
// answer that is not active counts as nothing.
inline bool as_bool(Kind k, float f, bool b) { return k == FLOAT ? f >= kTriggerClickAt : b; }
inline float as_float(Kind k, float f, bool b) { return k == BOOL ? (b ? 1.0f : 0.0f) : f; }
inline bool mix_bool(Input::Mode m, bool live_active, bool live, bool file) {
    return m == Input::ADD ? ((live_active && live) || file) : file;
}
inline float mix_float(Input::Mode m, bool live_active, float live, float file) {
    if (m != Input::ADD) return file;
    const float l = live_active ? live : 0.0f;
    return l > file ? l : file;
}
inline float clamp1(float v) { return v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v; }
inline void mix_vec2(Input::Mode m, bool live_active, float lx, float ly, float fx, float fy, float& ox, float& oy) {
    if (m != Input::ADD) { ox = fx; oy = fy; return; }
    ox = clamp1((live_active ? lx : 0.0f) + fx);
    oy = clamp1((live_active ? ly : 0.0f) + fy);
}

} // namespace vb
