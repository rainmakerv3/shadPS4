<!--
SPDX-FileCopyrightText: Copyright 2026 IFreemz
SPDX-License-Identifier: GPL-2.0-or-later
-->

# shadPS4 Bloodborne DLSS

NVIDIA DLSS Super Resolution for Bloodborne on [shadPS4](https://github.com/shadps4-emu/shadPS4).

This is a modified build of shadPS4 0.19.0. It renders the game at the resolution set by the
Bloodborne resolution patch (for example 2560x1440) and uses DLSS to upscale it to your window
(for example 3840x2160). It is real temporal DLSS: the scene is rendered with sub-pixel jitter
and DLSS gets depth plus camera and character motion, so it rebuilds detail instead of just
sharpening a stretched image. The HUD is drawn on top afterwards so it stays crisp.

Without an RTX card, or without the two DLSS files next to the exe, it behaves exactly like
normal shadPS4 0.19.0.

| 1280x720, DLSS off | 1280x720 upscaled to 3840x2160 with DLSS |
|---|---|
| ![720p native](documents/dlss/720p-native.webp) | ![720p to 4K with DLSS](documents/dlss/720p-to-4k-dlss.webp) |

Same spot, rendered at 1280x720 in both shots; open them full size to compare.

## Requirements

- Windows 10 or 11, 64-bit
- NVIDIA GeForce RTX card (20 series or newer) and a recent driver
- Bloodborne v1.09 running in shadPS4 already (any region; tested with CUSA03173)

AMD and Intel GPUs are not supported. DLSS only exists on NVIDIA RTX hardware.

## Install

1. Download the latest zip from [Releases](../../releases).
2. Copy `shadPS4.exe`, `shadps4_dlss.dll` and `nvngx_dlss.dll` into the folder of the shadPS4
   build you play with, replacing its `shadPS4.exe`. Back up the old one first if you want to
   switch back.
3. In the Bloodborne patches (shadPS4 patch manager, or the Patches tab in BBLauncher), enable:
   - a **Resolution Patch** (see the table below)
   - **Disable AA**
   - **Disable Chromatic Aberration**
   - **Disable DoF**
4. Start the game and press **F1** in gameplay. The panel should say *Active* along with the
   resolutions.

To go back to normal rendering, delete the two DLLs, or untick *DLSS enabled* in the F1 panel.

If a launcher manages your shadPS4 builds (BBLauncher, for example), copy the files into the
build folder it starts. Installing or updating a build through the launcher brings back the
normal `shadPS4.exe`, so copy the files again afterwards.

### Which resolution patch

DLSS upscales from the game's render resolution to your shadPS4 window size, keeping the game's
aspect ratio. The resolution patch sets the render resolution; any window size works.

| Window | Resolution Patch | Result |
|---|---|---|
| 3840x2160 | 2560x1440 | DLSS Quality (recommended for 4K) |
| 3840x2160 | none (native 1920x1080) | DLSS Performance, faster and a bit softer |
| 2560x1440 | none (native 1920x1080) | DLSS Quality |
| 2560x1440 | 2560x1440 | DLAA: no upscaling, just DLSS anti-aliasing |
| 1920x1080 | 1280x720 | DLSS Quality, for slower GPUs |
| 1920x1080 | none (native 1920x1080) | DLAA |

If the render resolution is higher than the window, DLSS runs as DLAA at the render resolution
and the result is scaled down to the window.

Higher render resolutions need more VRAM. The resolution patch notes say to raise dmem in the
game-specific settings, and that still applies.

### Why the three "Disable" patches

The game blurs the image before DLSS sees it: its own post-process AA softens edges, chromatic
aberration smears colour towards the screen edges, and depth of field blurs anything that isn't
in focus. With them off, DLSS gets a clean image to work with. Leave motion blur on, because
the patch that removes it also removes the data DLSS uses to track moving characters.

The F1 panel checks these patches and has a button to enable them; that takes effect after a
restart.

## Settings

Press **F1** in game. Mouse, keyboard and controller all work; the game ignores the controller
while the panel is open.

- **DLSS enabled**: compare against the normal image at any time.
- **Model preset**: M is the default. K is the one NVIDIA recommends for this mode. J
  and L are there to experiment with.
- **Sharpness**: sharpening after DLSS, default 0.6. Set it to 0 if you prefer the plain DLSS
  look.

The settings are saved in `dlss.ini` in your shadPS4 user folder (the `user` folder next to
`shadPS4.exe`, or `%APPDATA%\shadPS4` if there isn't one). You can also edit it by hand; changes
apply within a second.

## Troubleshooting

The F1 panel shows the reason when DLSS isn't running.

- **"not an NVIDIA GPU" / "does not support DLSS"**: DLSS needs an RTX card. On laptops with two
  GPUs, make sure shadPS4 runs on the NVIDIA one (Windows graphics settings, or NVIDIA Control
  Panel).
- **"NGX initialization failed"**: update your NVIDIA driver.
- **"shadps4_dlss.dll is missing or from a different version"**: the exe and the DLL come from
  different releases. Copy all three files from the same zip.
- **"Waiting for gameplay"**: menus, the title screen and loading screens use the normal image;
  this is expected.
- **The F1 panel doesn't open**: check `input_config/global.ini` in the shadPS4 user folder for
  a line like `hotkey_toggle_dlss = f1`. You can bind it to another key there.
- **Screen flicker with G-SYNC / FreeSync**: this comes from uneven frame pacing in emulation, not
  from DLSS. A frame rate cap or V-Sync usually helps.

## FAQ

**Frame generation?** Not built in. NVIDIA Smooth Motion, the driver-level frame generation in
the NVIDIA App, supports Vulkan programs like shadPS4 on recent RTX cards and drivers, and needs
nothing from this project.

**HDR?** Bloodborne only outputs SDR, so there is nothing to change on the DLSS side. For an HDR
look, use a driver-level SDR-to-HDR feature such as NVIDIA RTX HDR in the NVIDIA App.

**Other games?** No. The integration depends on how Bloodborne renders: which shaders draw the
HUD, where its camera matrices live and which passes write motion data.

**Linux or Steam Deck?** No. The DLSS part is Windows-only for now, and the Steam Deck has no
NVIDIA GPU.

**Is this an official shadPS4 release?** No. It is a separate build based on shadPS4 0.19.0.
Please don't report problems with it to the shadPS4 team; open an issue here instead.

## How it works

For anyone curious, or anyone porting this to a newer shadPS4:

- **Jitter**: draws into the main scene depth buffer get a sub-pixel viewport offset from a
  Halton(2,3) sequence. Full-screen passes and HUD draws are not jittered.
- **Camera motion**: the view and projection matrices come from the game's scene constants, and
  the previous-to-current transform is built in double precision. The game also has a
  ready-made reprojection matrix for its motion blur, but it is computed in float32 with large
  world coordinates, and its rounding noise of about 0.6 px made DLSS shimmer.
- **Object motion**: the game's own velocity draws are replayed at full resolution into a
  separate buffer and used where they cover the screen. The camera motion fills in elsewhere.
- **Where DLSS runs**: at the first HUD draw, on a copy of the finished scene. When the game
  copies the frame to the screen, the HUD is added back from its own render and the game's
  brightness/gamma table is applied at output resolution.
- **NVIDIA code**: shadPS4 itself contains no NVIDIA code. `shadps4_dlss.dll` (source in
  [`dlss_bridge/`](dlss_bridge)) is a small bridge that links the DLSS SDK and is loaded at
  runtime when present.

Menus, the title screen and loading screens are not upscaled by DLSS; they show the normal image.

## Building

The emulator builds like normal shadPS4 (see [documents/building-windows.md](documents/building-windows.md))
and doesn't need the DLSS SDK.

The bridge needs the [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS):

```
cmake -S dlss_bridge -B build-dlss -G Ninja -DCMAKE_BUILD_TYPE=Release -DDLSS_SDK_ROOT=<path to DLSS SDK>
cmake --build build-dlss
```

`nvngx_dlss.dll` comes from the SDK (`lib/Windows_x86_64/rel`).

## AI disclosure

An AI coding agent was used in the development of this project.

## Credits

- [shadPS4](https://github.com/shadps4-emu/shadPS4) and its contributors, for the emulator this
  is built on.
- deadinside28's [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc), whose notes on
  Bloodborne's scene constants and on viewport jitter for scene geometry were the starting
  point for the camera motion and jitter here.
- The authors of the Bloodborne patches for shadPS4 (Kyo, illusion and others), including the
  resolution patches and the AA, chromatic aberration and depth-of-field patches recommended
  above.
- NVIDIA, for DLSS.

## License

- shadPS4 and the changes to it: GPL-2.0-or-later, see [LICENSE](LICENSE).
- The DLSS bridge in `dlss_bridge/`: MIT, see [dlss_bridge/LICENSE.txt](dlss_bridge/LICENSE.txt).
- `nvngx_dlss.dll` is distributed under the NVIDIA DLSS SDK license and is not covered by the
  licenses above.

NVIDIA, GeForce RTX and DLSS are trademarks of NVIDIA Corporation. Bloodborne is a trademark of
Sony Interactive Entertainment; this project is not affiliated with Sony, FromSoftware, NVIDIA
or the shadPS4 team, and includes no game files.
