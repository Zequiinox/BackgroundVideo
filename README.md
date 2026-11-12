# BackgroundVideo

A native mod for [LeviLaunchroid](https://github.com/LiteLDev/LeviLaunchroid) that plays your own video (or shows one image)
as the main menu background **instead of the Minecraft panorama**.
Built for Minecraft Bedrock 26.62, Android arm64-v8a.

## How it works

In 26.x the menu buttons are a separate HTML-based UI drawn on top of a native cubemap panorama
(`CubemapBackgroundScreen` / `PanoramaRenderer`). The mod replaces that panorama in place:

1. It hooks the GL draw calls in `libGLESv2.so` / `libGLESv3.so` (the same technique BedrockTools' Motion Blur uses for `glClear`).
2. On the tested build (Mali GPU, GLES 3.2) the panorama is the first six draws of every menu frame: six quads, each with
   its own 2D texture (the six cube faces). The mod learns those six texture ids from the start of each menu frame, then
   skips exactly those draws and draws the video in their place, so the buttons still sit on top. (If a build uses a real
   cube map sampler instead, that is detected as well.)
3. Menu detection reads the `ClientInstance` vtable (RTTI name `14ClientInstance`, slot 24 = `update`, slot 31 =
   `getLocalPlayer`), the same way BedrockTools resolves other virtual functions. Both slots point at exactly the same
   functions as the old byte-pattern signatures did on 26.62, but vtable slots survive recompiles better than byte patterns.
4. Decoding and audio use Android's `MediaPlayer` over JNI, so any mp4 the phone can play works.
5. The video is opened on a background thread while Minecraft is still starting (so it is ready when the menu appears,
   and the render thread is never blocked), loops while the menu shows, and pauses when you join a world.
6. When the app goes to the background the video is paused (a small watchdog notices that frames stopped). Android
   destroys the GL context meanwhile; on return the mod gives the *same* MediaPlayer a fresh surface in the new context
   and resumes it, so the video continues exactly where it was instead of restarting.

The panorama is recognised by its six face textures wherever they appear in the frame, and for a second after it was last
seen, so screens that open on top of the menu (e.g. Profile, where the game can have a local player) keep the video.
`log.txt` records every time the panorama appears or disappears.

If the video is missing or cannot be played, the mod steps aside and the normal panorama is shown.

## Setup

1. Build and import `BackgroundVideo.levipack` (the video is **not** part of the build, and the mod never creates folders or writes files on the phone, apart from its own `config.json`):

```sh
xmake f -y -p android -a arm64-v8a -m release --ndk=/path/to/android-ndk-r28c
xmake -y
```

2. Copy your video or image to the mod's config folder as `menu.mp4` (that is the default `video_path`), or put it anywhere
   on the phone and set `video_path` in `config.json` to its full path:

```json
{
  "video_path": "/storage/emulated/0/Android/media/org.levimc.launcher/minecraft/com.mojang.minecraftpe/mods/BackgroundVideo/config/menu.mp4"
}
```

   **Images:** if `video_path` ends in `.png`, `.jpg`, `.jpeg`, `.webp`, `.bmp` or `.gif`, that image is shown as a still
   background instead of a video (a GIF shows its first frame). Images larger than 2048 px are scaled down.
3. Open the main menu. Changing `video_path`, or replacing the file, is picked up automatically within a
   couple of seconds, with no rebuild or restart.

In a world there is no menu background, so nothing is shown there; the video is back as soon as you return to the menu.

The mod only needs permission to *read* the video. If `video_path` is empty it looks for the file named in `video` (default `menu.mp4`)
in the config folder instead.

Edit `Author` in `src/Version.hpp` and `levimod.json` before you publish.

## Config

| Key | Default | Meaning |
| --- | --- | --- |
| `video_path` | `<config folder>/menu.mp4` | Full path to your video or image |
| `video` | `menu.mp4` | Used only when `video_path` is empty: file name in the config folder |
| `hide_ui_background` | `true` | Also hide a wallpaper image (larger than the screen) that a UI pack draws over the panorama |
| `audio_enabled` | `true` | Play the video's sound (also a toggle in the in-game mod menu) |
| `audio_volume` | `1.0` | 0.0 to 1.0 (also a slider in the in-game mod menu) |
| `fit` | `cover` | `cover` fills the screen (crops edges), `contain` shows black bars |
| `brightness` | `1.0` | Lower it to dim the video if menu text is hard to read |
| `debug_trace` | `false` | Logs every draw call of three menu frames to `log.txt` (only for bug reports) |

## In-game settings

The mod registers a **Menu Video Background** module in the LeviLauncher mod menu: a master toggle for the whole mod, a
**Video audio** toggle and a **Volume** slider. Changes apply live and are saved back into `config.json`
(`audio_enabled`, `audio_volume`); other keys in the file are kept.

## UI / theme packs

The mod replaces the *native panorama*. Resource packs that restyle the menu normally still work, because the buttons and
overlays they draw come after the panorama. Some big UI themes also draw their **own full-screen background image** on
the start screen (for example CubeVisage UI ships a 2800x1272 `bg.jpg`), which would sit on top of the video and hide it.

With `hide_ui_background` on (the default) the mod also skips such a background: once the video has been drawn in a
frame, an image quad among the first draws of the menu frame whose texture is **larger than the screen in both
directions** is treated as a pack wallpaper and not drawn. The pack's tint layers and panels are kept, so the video
shows through them. Backdrops that are screen-sized or smaller (for example the dimmed or blurred copy that screens like
Play and Settings draw behind themselves) are never touched. Set `"hide_ui_background": false` to keep the pack's
background instead. Limitation: a pack wallpaper that is not larger than your screen is not detected.

The panorama itself is only recognised as the very first six quads of a menu frame (confirmed over two frames in a row),
so extra things a pack draws on the menu are never mistaken for it.

If a pack still misbehaves, set `"debug_trace": true` in `config.json`, open the menu with the pack enabled, and send
`log.txt`: it records what the menu draws frame by frame, including the size of the texture of each draw (`size0=WxH`).

## Troubleshooting

The mod writes `log.txt` next to `config.json` (the mod's config folder), rewritten on every start. It records
the hooks that were installed, whether menu detection works, which draw calls the game uses, a short trace of
three menu frames, and a stat line every ~10 seconds. Send it along when reporting a problem.
The same lines go to logcat under the tag `BackgroundVideo`.

- `panorama replaced by video` means it is working.
- `ClientInstance vtable not resolved`: RTTI is missing or the slot numbers changed in this Minecraft build; check `kSlotUpdate` / `kSlotGetLocalPlayer` in `src/Signatures.cpp`.
- `in menu ... but the panorama was never recognised`: the first draws of a menu frame do not look like six face quads; the trace in `log.txt` shows how the frame is drawn.
- `learned panorama faces: ...` means the six face textures were found.
- `cannot read a video at ...`: the path in `video_path` is wrong, or the launcher has no storage access to that file.
- For the smoothest result use an unrotated H.264 mp4 at 1080p or lower, 30 fps, around 4-8 Mbps. Very high resolutions or bitrates cost battery and can stutter on weaker phones.
- Needs Android 8 or newer (the video is opened before the game has a GL context).
