# VR Bench

VR Bench plays a VR game for you, so a game can be tested and learned without anyone wearing a headset.
It is the layer half of the VR Bench feature of the Lanes plugin (https://github.com/TefMeister/lanes-plugin):
a small OpenXR layer that sits between a game and the OpenXR runtime and gives the game pretend hands,
buttons and sticks read from a file. The plugin's tools write that file, and so can drive a game in the
background while you get on with something else.

When a real headset is worn, the same layer adds one thing: a button chord (both grips and the left
trigger, held) that marks "look at this" and records where the hand or the gun was pointing.

Status: being built (October 2026). The layer's first build is on the Releases page (0.1.0: pretend hands,
buttons and sticks from the file; nothing switched on by installing). The plugin half that drives it, and the
"look at this" chord, are still being built, so there is nothing to use on its own yet.


Description


- Pretend hands: the game's grip and aim poses come from the file, in the room's own space, so a test
  can hold a controller exactly here and check what the game draws.
- Pretend buttons, triggers and sticks: trigger, squeeze, thumbstick or trackpad, A/B/X/Y, menu and the
  stick click, each either set, added on top of the real controller, or passed through untouched.
- Anything the file does not mention is passed through to the runtime unchanged.
- The "look at this" chord for a worn headset, with a short haptic pulse as the receipt.
- Works with the OpenXR Simulator (no headset) and with a real headset, 64-bit and 32-bit games.


What it never does


- It is off unless the plugin switches it on for one run, and it is switched off again after.
- Installing it registers nothing with OpenXR; the plugin registers it per run, only for the game's bitness.
- It never sends anything anywhere, and it never writes anything but its own log and the files you ask for.
- It never records names, paths or anything about the person at the PC.


Installation instructions


The plugin installs VR Bench for you. During Lanes setup, and at each Lanes update, the OpenXR Simulator is
offered in three ways:

STEP 1 - Choose during Lanes setup or update

1.1  OpenXR Simulator (webhead2oo9's fork, which also runs 32-bit games) together with VR Bench.
     Pick this to let the plugin test games for you.

1.2  OpenXR Simulator, webhead2oo9's fork only. No VR Bench.

1.3  OpenXR Simulator, elliotttate's original, 64-bit games only. No VR Bench.

STEP 2 - By hand, if you build it yourself

2.1  Run build.bat (needs Visual Studio 2022 Build Tools with C++). It builds the layer, 64-bit and 32-bit,
     and runs the tests.

2.2  Run install.ps1. It copies the layer into your local programs folder and writes its manifests. It
     registers nothing; the plugin does that per run.


Requirements


- Windows 10 or 11.
- An OpenXR runtime: the OpenXR Simulator (webhead2oo9's fork for 32-bit games) or a real headset's runtime.
- The Lanes plugin, which does the driving.


Shout outs


VR Bench is made with the knowledge gained from using elliotttate's tools, above all his OpenXR Simulator
(https://github.com/elliotttate/OpenXR-Simulator). See CREDITS.md for everyone else. If you should be credited here and are not, email us and we will fix it as soon as we can.


Licence


MIT, see LICENSE. The OpenXR headers in third_party/openxr are Khronos's (Apache-2.0 OR MIT).
