// vb_input_test.cpp -- the host test for layer/vb_input.h: no OpenXR runtime, no headset, no Windows API.
//   cl /nologo /EHsc /std:c++17 /I..\layer vb_input_test.cpp && vb_input_test.exe
// Prints "N passed, M failed" and exits 1 on any failure.
#include <cstdio>
#include <cstring>
#include "vb_input.h"

static int g_pass = 0, g_fail = 0;
#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static const char* kFile =
    "{\"v\":1,\"mode\":{\"poses\":\"set\",\"buttons\":\"set\"},"
    " \"vr\":{\"left\":{\"trigger\":0.8,\"stick\":[0.5,-0.25],\"a\":true,"
    "               \"grip_pose\":{\"p\":[-0.2,-0.3,-0.4],\"q\":[0,0,0,1]}},"
    "       \"right\":{\"squeeze\":1.0}}}";

static vb::Input parsed() {
    vb::Input in{};
    CHECK(vb::parse(kFile, std::strlen(kFile), in));
    return in;
}

static void parses_v1() {
    vb::Input in = parsed();
    CHECK(in.valid);
    CHECK(in.version == 1);
    CHECK(in.poses == vb::Input::SET);
    CHECK(in.buttons == vb::Input::SET);
}

static void left_trigger_value() {
    vb::Input in = parsed();
    int h = -1; float f = 0, vx = 0, vy = 0; bool b = false;
    CHECK(vb::lookup(in, "/user/hand/left/input/trigger/value", h, f, b, vx, vy) == vb::FLOAT);
    CHECK(h == 0);
    CHECK(f == 0.8f);
}

static void trigger_click_from_value() {
    vb::Input in = parsed();
    int h = -1; float f = 0, vx = 0, vy = 0; bool b = false;
    CHECK(vb::lookup(in, "/user/hand/left/input/trigger/click", h, f, b, vx, vy) == vb::BOOL);
    CHECK(b == true);   // 0.8 >= 0.75
}

static void stick_vec2_and_x() {
    vb::Input in = parsed();
    int h = -1; float f = 0, vx = 0, vy = 0; bool b = false;
    CHECK(vb::lookup(in, "/user/hand/left/input/thumbstick", h, f, b, vx, vy) == vb::VEC2);
    CHECK(vx == 0.5f);
    CHECK(vy == -0.25f);
    CHECK(vb::lookup(in, "/user/hand/left/input/thumbstick/x", h, f, b, vx, vy) == vb::FLOAT);
    CHECK(f == 0.5f);
}

static void grip_pose() {
    vb::Input in = parsed();
    int h = -1; float f = 0, vx = 0, vy = 0; bool b = false;
    CHECK(vb::lookup(in, "/user/hand/left/input/grip/pose", h, f, b, vx, vy) == vb::POSE_GRIP);
    CHECK(in.hand[0].grip.set);
    CHECK(in.hand[0].grip.px == -0.2f);
    CHECK(in.hand[0].grip.qw == 1.0f);
}

static void unknown_path_is_none() {
    vb::Input in = parsed();
    int h = -1; float f = 0, vx = 0, vy = 0; bool b = false;
    CHECK(vb::lookup(in, "/user/hand/left/input/crouch/click", h, f, b, vx, vy) == vb::NONE);
}

static void unset_hand_value_is_none() {
    vb::Input in = parsed();
    int h = -1; float f = 0, vx = 0, vy = 0; bool b = false;
    CHECK(vb::lookup(in, "/user/hand/right/input/trigger/value", h, f, b, vx, vy) == vb::NONE);   // right has only squeeze
    CHECK(vb::lookup(in, "/user/hand/right/input/squeeze/value", h, f, b, vx, vy) == vb::FLOAT);
    CHECK(h == 1);
    CHECK(f == 1.0f);
}

static void bad_json_keeps_previous() {
    vb::Input in = parsed();
    const char* half = "{\"v\":1,";
    CHECK(!vb::parse(half, std::strlen(half), in));
    CHECK(in.hand[0].trigger == 0.8f);
    CHECK(in.valid);
}

static void wrong_version_rejected() {
    vb::Input in{};
    const char* v2 = "{\"v\":2}";
    CHECK(!vb::parse(v2, std::strlen(v2), in));
    CHECK(!in.valid);
}

int main() {
    parses_v1();
    left_trigger_value();
    trigger_click_from_value();
    stick_vec2_and_x();
    grip_pose();
    unknown_path_is_none();
    unset_hand_value_is_none();
    bad_json_keeps_previous();
    wrong_version_rejected();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
