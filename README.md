<!--
SPDX-FileCopyrightText: Copyright 2026 IFreemz
SPDX-License-Identifier: GPL-2.0-or-later
-->

# shadPS4 Bloodborne DLSS & FSR

Temporal upscaling for Bloodborne on [shadPS4](https://github.com/shadps4-emu/shadPS4): NVIDIA
DLSS Super Resolution on GeForce RTX cards, AMD FSR 3.1 on everything else.

This is a modified build of shadPS4 0.19.0. It renders the game at the resolution set by the
Bloodborne resolution patch (for example 2560x1440) and upscales it to your window (for example
3840x2160). It is real temporal upscaling: the scene is rendered with sub-pixel jitter and the
upscaler gets depth plus camera and character motion, so it rebuilds detail instead of just
sharpening a stretched image. The HUD is drawn on top afterwards so it stays crisp.

DLSS looks better, especially from low render resolutions; FSR 3.1 runs on any GPU, including
AMD, Intel and older NVIDIA cards. Without the upscaler DLLs next to the exe, the build behaves
exactly like normal shadPS4 0.19.0.

| 1280x720, DLSS off | 1280x720 upscaled to 3840x2160 with DLSS |
|---|---|
| ![720p native](documents/dlss/720p-native.webp) | ![720p to 4K with DLSS](documents/dlss/720p-to-4k-dlss.webp) |

Same spot, rendered at 1280x720 in both shots; open them full size to compare.

## Requirements

- Windows 10 or 11, 64-bit
- Bloodborne v1.09 running in shadPS4 already (any region; tested with CUSA03173)
- For DLSS: an NVIDIA GeForce RTX card (20 series or newer) and a recent driver
- For FSR 3.1: any GPU that runs shadPS4
- For the optional FSR 4 add-on: a GPU with INT8 dot products and compute shader derivatives,
  such as an AMD Radeon RX 6000 or newer, an NVIDIA RTX card or an Intel Arc card

## Install

1. Download the latest zip from [Releases](../../releases).
2. Copy `shadPS4.exe` and the three DLLs into the folder of the shadPS4 build you play with,
   replacing its `shadPS4.exe`. Back up the old one first if you want to switch back.
   - `shadps4_dlss.dll` and `nvngx_dlss.dll`: DLSS, only used on RTX cards
   - `amd_fidelityfx_vk.dll`: FSR 3.1
3. In the Bloodborne patches (shadPS4 patch manager, or the Patches tab in BBLauncher), enable:
   - a **Resolution Patch** (see the table below)
   - **Disable AA**
   - **Disable Chromatic Aberration**
   - **Disable DoF**

   and make sure **Disable Motion Blur** is off (see below).
4. Start the game and press **F1** in gameplay. The panel should say *Active*, the upscaler and
   the resolutions.

Without a launcher, double-click `shadPS4.exe`: it opens shadPS4's game list (Big Picture
mode), where you pick Bloodborne. Launchers such as BBLauncher work as before.

To go back to normal rendering, delete the three DLLs, or untick *Upscaling enabled* in the F1
panel.

If a launcher manages your shadPS4 builds (BBLauncher, for example), copy the files into the
build folder it starts. Installing or updating a build through the launcher brings back the
normal `shadPS4.exe`, so copy the files again afterwards.

### Optional: FSR 4 add-on

FSR 4 is AMD's machine-learning upscaler, generally sharper and steadier than FSR 3.1. It is
mainly meant for Radeon and Intel Arc cards; RTX cards can run it too, but DLSS is the better fit
there. It comes as a separate download from
[Releases](../../releases), `shadPS4-Bloodborne-FSR4-addon-…-win64.zip`.

1. Unzip it and put the `fsr4` folder next to `shadPS4.exe` (the folder itself, not its
   contents).
2. Start the game. *Automatic* now uses FSR 4 on cards without DLSS, or pick *FSR 4 (add-on)*
   in the F1 panel. On RTX cards *Automatic* keeps DLSS.

To remove it, delete the `fsr4` folder. It works for output sizes up to 3840x2160 and costs
more GPU time than FSR 3.1, especially on mid-range cards.

The add-on runs the FSR 4 "v07" INT8 model through [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan),
with the model files built by the Q2RTX project from the FSR 4 source code AMD published under
the MIT license. It is not AMD's official FSR 4.1 DLL, which exists only for DirectX 12. It is
kept separate from the main download so the main build does not depend on it.

### Which resolution patch

The upscaler goes from the game's render resolution to your shadPS4 window size, keeping the
game's aspect ratio. The resolution patch sets the render resolution; any window size works.

| Window | Resolution Patch | Result |
|---|---|---|
| 3840x2160 | 2560x1440 | Quality mode (recommended for 4K) |
| 3840x2160 | none (native 1920x1080) | Performance mode, faster and a bit softer |
| 2560x1440 | none (native 1920x1080) | Quality mode |
| 2560x1440 | 2560x1440 | No upscaling, just anti-aliasing (DLAA / FSR Native AA) |
| 1920x1080 | 1280x720 | Quality mode, for slower GPUs |
| 1920x1080 | none (native 1920x1080) | Anti-aliasing only |

FSR shows its weaknesses more than DLSS from low render resolutions: on FSR, prefer a render
resolution of at least 1920x1080 for a 4K window.

If the render resolution is higher than the window, the upscaler only anti-aliases at the render
resolution and the result is scaled down to the window. The upscaler goes up to 3x per side; if
the window is larger than that, the rest is plain scaling.

Higher render resolutions need more VRAM. The resolution patch notes say to raise dmem in the
game-specific settings, and that still applies.

### Decoupled 1080p UI patches

The *Decoupled 1080p UI* patches render the scene at a lower resolution (for example 1280x720)
while the game keeps its UI at 1080p. The build detects them by itself: the scene is upscaled to
1080p with DLSS or FSR, and the game then adds its effects and draws its own sharp 1080p UI on
top. Use one of them instead of a regular Resolution Patch, not together with one. The 640x360,
960x540, 1280x720 and 1600x900 versions are supported. The 320x180 one and the versions above
1080p are not: with them the upscaler stays off (F1 says why) and the game looks as it would
without this build.

They are made for 1080p screens and low-VRAM setups. The final picture is 1080p, scaled to your
window, so on 1440p or 4K screens the regular Resolution Patches give a sharper image. FSR shows
more shimmer on thin detail from 720p and below; DLSS copes well. The Sharpness slider has no
effect in this mode.

### Other patches

Tested and working with the upscaler: *Performance Patch*, *rgba8f color space* and *lower
specific renders* (all "Perf Increase"), *HD Motion Blur* and *Optimal 1080p*. Keep *Better AA*
and *Enable TAA* off: they add the game's own anti-aliasing, which the upscaler replaces.

### Why the three "Disable" patches, and not Disable Motion Blur

The game blurs the image before the upscaler sees it: its own post-process AA softens edges,
chromatic aberration smears colour towards the screen edges, and depth of field blurs anything
that isn't in focus. With them off, the upscaler gets a clean image to work with.

**Disable Motion Blur (Perf Increase) must stay off.** It also removes the depth and motion data
the upscaler is built on, and the F1 panel then stays on *Waiting for gameplay*. You don't need
it: this build turns the game's motion blur off by itself (see *Game motion blur* below) and
keeps the data.

The F1 panel checks these patches and has a button that sets all four correctly; that takes
effect after a restart.

## Settings

Press **F1** in game. Mouse, keyboard and controller all work; the game ignores the controller
while the panel is open.

- **Upscaling enabled**: compare against the normal image at any time.
- **Upscaler**: *Automatic* uses DLSS when the card supports it, otherwise FSR 4 if its add-on
  is installed, otherwise FSR 3.1. You can also pick any of them by hand to compare.
- **DLSS model**: M is the default. K is the one NVIDIA recommends for this mode. J and L are
  there to experiment with.
- **Sharpness**: sharpening after upscaling, default 0.6. Set it to 0 if you prefer the plain
  look.
- **Game motion blur**: off by default. The game blurs the scene before the upscaler gets it,
  which leaves ghost trails around your character when the camera moves; with the blur off the
  image stays clean. Tick it if you want the game's blur back.
- **Menu fix**: on by default. While a menu covers most of the screen (inventory, pause menu),
  the game's own image is shown and the scene isn't jittered, so it can't shimmer behind the
  menu panels. Your frame rate is the same either way. Smaller popups keep the upscaled image.

The settings are saved in `dlss.ini` in your shadPS4 user folder (the `user` folder next to
`shadPS4.exe`, or `%APPDATA%\shadPS4` if there isn't one). You can also edit it by hand; changes
apply within a second.

## Troubleshooting

The F1 panel shows the reason when upscaling isn't running.

- **"not an NVIDIA GPU" / "does not support DLSS"**: DLSS needs an RTX card; the *Automatic*
  setting uses FSR instead. On laptops with two GPUs, make sure shadPS4 runs on the NVIDIA one
  (Windows graphics settings, or NVIDIA Control Panel).
- **"NGX initialization failed"**: update your NVIDIA driver.
- **"shadps4_dlss.dll is missing or from a different version"**: the exe and the DLL come from
  different releases. Copy all files from the same zip.
- **"FSR needs amd_fidelityfx_vk.dll"**: copy that DLL from the zip next to `shadPS4.exe`.
- **"FSR 4 needs the FSR 4 add-on" / "FSR 4 could not start"**: check that the `fsr4` folder
  sits next to `shadPS4.exe` with all its files, and that your GPU is in the requirements above.
  The shadPS4 log has the exact reason (search for `[FSR4]`).
- **The game crashes at boot after a "Config Migration" prompt**: you copied the files into a
  build that keeps its settings in `config.toml` (ShadLix and other forks). Use this release or
  newer, which carries the extra dmem over. Otherwise set *extra dmem* again (for example
  `"extra_dmem_in_mbytes": 8000` under `"General"` in `user/custom_configs/CUSA03173.json`); the
  resolution patches need it. Pick *Copy* if it asks about saves.
- **"Waiting for gameplay"**: menus, the title screen and loading screens use the normal image;
  this is expected. If it stays like this in gameplay, check that Disable Motion Blur is off.
- **The F1 panel doesn't open**: check `input_config/global.ini` in the shadPS4 user folder for
  a line like `hotkey_toggle_dlss = f1`. You can bind it to another key there.
- **Screen flicker with G-SYNC / FreeSync**: this comes from uneven frame pacing in emulation, not
  from DLSS. Use an FPS patch that matches your vblank setting (for example 60 FPS++ with 60)
  rather than *Uncap FPS++*.

## FAQ

**FSR 4?** Yes, as the optional add-on described under Install. AMD only ships its official
FSR 4.1 for DirectX 12; the add-on uses the community Vulkan port of the FSR 4 v07 model.

**XeSS?** Not included. FSR 3.1 already covers Intel cards.

**Frame generation?** Not built in. NVIDIA Smooth Motion, the driver-level frame generation in
the NVIDIA App, supports Vulkan programs like shadPS4 on recent RTX cards and drivers, and needs
nothing from this project.

**HDR?** Bloodborne only outputs SDR, so there is nothing to change on the upscaler side. For an HDR
look, use a driver-level SDR-to-HDR feature such as NVIDIA RTX HDR in the NVIDIA App.

**Other games?** No. The integration depends on how Bloodborne renders: which shaders draw the
HUD, where its camera matrices live and which passes write motion data.

**Linux or Steam Deck?** No. The upscaling part is Windows-only for now.

**Is this an official shadPS4 release?** No. It is a separate build based on shadPS4 0.19.0.
Please don't report problems with it to the shadPS4 team; open an issue here instead.

## How it works

For anyone curious, or anyone porting this to a newer shadPS4:

- **Jitter**: draws into the main scene depth buffer get a sub-pixel viewport offset from a
  Halton(2,3) sequence. Full-screen passes and HUD draws are not jittered.
- **Camera motion**: the view and projection matrices come from the game's scene constants, and
  the previous-to-current transform is built in double precision. The game also has a
  ready-made reprojection matrix for its motion blur, but it is computed in float32 with large
  world coordinates, and its rounding noise of about 0.6 px made the image shimmer.
- **Object motion**: the game's own velocity draws are replayed at full resolution into a
  separate buffer and used where they cover the screen. The camera motion fills in elsewhere.
- **Where the upscaler runs**: at the first HUD draw, on a copy of the finished scene. When the
  game copies the frame to the screen, the HUD is added back from its own render and the game's
  brightness/gamma table is applied at output resolution.
- **HUD and menus**: the HUD is treated as a dimming of the scene plus an overlay, so darkening
  overlays dim the upscaled image instead of mixing in the render-size one. When the HUD changes
  most of the screen (a menu), the game's own frame is shown and jitter pauses.
- **Frame pacing**: the emulator's vblank timer sleeps until shortly before each deadline and
  waits out the rest precisely, so frames reach a VRR display at even intervals.
- **Decoupled UI patches**: the game upscales its HDR scene to the 1080p UI resolution in one
  copy pass before its post-processing. When that pass gets a smaller scene, the build replaces
  it with DLSS/FSR on the HDR scene (with automatic exposure), and the game post-processes and
  draws its UI as usual. The scene depth for jitter comes from the draws into that HDR scene.
- **Motion blur**: the game blurs the HDR scene in two full-screen passes along a motion buffer
  it builds from depth and its velocity draws. With *Game motion blur* off, those two passes are
  replaced by plain copies, so the motion data is still there for the upscaler.
- **DLSS and FSR get the same inputs**: colour, depth, motion vectors and jitter in render
  pixels. FSR also gets the camera's near/far planes and field of view from the scene constants.
- **NVIDIA code**: shadPS4 itself contains no NVIDIA code. `shadps4_dlss.dll` (source in
  [`dlss_bridge/`](dlss_bridge)) is a small bridge that links the DLSS SDK and is loaded at
  runtime when present.
- **AMD code**: `amd_fidelityfx_vk.dll` is AMD's signed FidelityFX SDK 1.1.4 build, loaded at
  runtime when present. Only its API headers are in this repository
  ([`externals/ffx-api`](externals/ffx-api)).
- **FSR 4 add-on**: `fsr4/shadps4_fsr4.dll` (source in [`fsr4_addon/`](fsr4_addon)) links
  FSR-Vulkan's FSR 4 v07 provider and loads the model files next to it. When the folder is
  present, the device also enables INT8 dot products and compute shader derivatives, which
  the model passes use. It gets the same inputs as DLSS and FSR 3.1.

Menus, the title screen and loading screens are not upscaled; they show the normal image.

## Building

The emulator builds like normal shadPS4 (see [documents/building-windows.md](documents/building-windows.md))
and doesn't need the DLSS SDK.

The bridge needs the [NVIDIA DLSS SDK](https://github.com/NVIDIA/DLSS):

```
cmake -S dlss_bridge -B build-dlss -G Ninja -DCMAKE_BUILD_TYPE=Release -DDLSS_SDK_ROOT=<path to DLSS SDK>
cmake --build build-dlss
```

The FSR 4 add-on needs [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan) and the Vulkan SDK
(or a `vulkan-1` import library via `-DVULKAN_LIBRARY`):

```
cmake -S fsr4_addon -B build-fsr4 -G Ninja -DCMAKE_BUILD_TYPE=Release -DFSR_VULKAN_ROOT=<path to FSR-Vulkan>
cmake --build build-fsr4
```

Its model files are `fsr4_model_v07_i8_*`, `rcas.spv` and `spd_auto_exposure.spv` from
[Q2RTX's `baseq2/fsr4_shaders`](https://github.com/FireBurn/Q2RTX/tree/ae8d628fae208813172446d1e49ed94150b04658/baseq2/fsr4_shaders).

`nvngx_dlss.dll` comes from the SDK (`lib/Windows_x86_64/rel`). `amd_fidelityfx_vk.dll` comes
from the [AMD FidelityFX SDK v1.1.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4)
(`PrebuiltSignedDLL`).

## AI disclosure

An AI coding agent was used in the development of this project.

## Credits

- [shadPS4](https://github.com/shadps4-emu/shadPS4) and its contributors, for the emulator this
  is built on.
- deadinside28's [bloodborne_pc](https://github.com/deadinside28/bloodborne_pc), whose notes on
  Bloodborne's scene constants and on viewport jitter for scene geometry were the starting
  point for the camera motion and jitter here, and whose FSR 4 integration the add-on follows.
- FireBurn's [FSR-Vulkan](https://github.com/FireBurn/FSR-Vulkan), which runs FSR 4 on Vulkan, and
  the Q2RTX project, which built the FSR 4 v07 model files.
- The authors of the Bloodborne patches for shadPS4 (Kyo, illusion and others), including the
  resolution patches and the AA, chromatic aberration and depth-of-field patches recommended
  above.
- NVIDIA, for DLSS, and AMD, for FSR and the open FidelityFX SDK.

## License

- shadPS4 and the changes to it: GPL-2.0-or-later, see [LICENSE](LICENSE).
- The DLSS bridge in `dlss_bridge/`: MIT, see [dlss_bridge/LICENSE.txt](dlss_bridge/LICENSE.txt).
- `nvngx_dlss.dll` is distributed under the NVIDIA DLSS SDK license and is not covered by the
  licenses above.
- `amd_fidelityfx_vk.dll` and the headers in `externals/ffx-api/` are from the AMD FidelityFX SDK,
  MIT, see [externals/ffx-api/LICENSE.txt](externals/ffx-api/LICENSE.txt).
- The FSR 4 add-on: its wrapper is GPL-2.0-or-later and FSR-Vulkan inside it is MIT. The model
  files carry the MIT notice of the FSR 4 source they were built from; the add-on zip includes
  all three license texts.

NVIDIA, GeForce RTX and DLSS are trademarks of NVIDIA Corporation. AMD and FidelityFX are
trademarks of Advanced Micro Devices, Inc. Bloodborne is a trademark of Sony Interactive
Entertainment; this project is not affiliated with Sony, FromSoftware, NVIDIA, AMD or the
shadPS4 team, and includes no game files.
