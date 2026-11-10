# BackgroundVideo

A native mod for [LeviLaunchroid](https://github.com/LiteLDev/LeviLaunchroid) that plays your own video
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
3. Menu detection reuses two BedrockTools signatures (`ClientInstance::update` and `getLocalPlayer`),
   which all match exactly once in the 26.62 `libminecraftpe.so`.
4. Decoding and optional audio use Android's `MediaPlayer` over JNI, so any mp4 the phone can play works.
5. The video is opened on a background thread while Minecraft is still starting (so it is ready when the menu appears,
   and the render thread is never blocked), loops while the menu shows, and pauses when you join a world.
6. When the app goes to the background the video is paused (a small watchdog notices that frames stopped). Android
   destroys the GL context meanwhile; on return the mod gives the *same* MediaPlayer a fresh surface in the new context
   and resumes it, so the video continues exactly where it was instead of restarting.

While the video is not ready yet (opening, waiting for its first frame, right after returning to the app) the panorama
spot is drawn black instead, so the original panorama never shows through. The panorama is recognised by its position
(the first six quads of the menu frame, known from the previous frame), which needs no GL ids and so also works in the
very first frame after Android recreates the GL context.

If the video is missing or cannot be played, the mod steps aside and the normal panorama is shown.

## Setup

1. Build and import `BackgroundVideo.levipack` (the video is **not** part of the build, and the mod never creates folders or writes files on the phone, apart from its own `config.json`):

```sh
xmake f -y -p android -a arm64-v8a -m release --ndk=/path/to/android-ndk-r28c
xmake -y
```

2. Put your video anywhere on the phone and open the mod's `config.json` (in the mod's config folder; created with defaults on first run).
   Set `video_path` to the video's full path:

```json
{
  "video_path": "/storage/emulated/0/Movies/menu.mp4"
}
```

3. Open the main menu. Changing `video_path`, or replacing the video file, is picked up automatically within a
   couple of seconds, with no rebuild or restart.

The mod only needs permission to *read* the video. If `video_path` is empty it looks for `background.mp4`
in the config folder instead.

Edit `Author` in `src/Version.hpp` and `levimod.json` before you publish.

## Config

| Key | Default | Meaning |
| --- | --- | --- |
| `video_path` | `""` | Full path to your video file |
| `video` | `background.mp4` | Used only when `video_path` is empty: file name in the config folder |
| `volume` | `0.0` | 0.0 (muted) to 1.0 |
| `fit` | `cover` | `cover` fills the screen (crops edges), `contain` shows black bars |
| `brightness` | `1.0` | Lower it to dim the video if menu text is hard to read |
| `debug_trace` | `false` | Logs every draw call of three menu frames to `log.txt` (only for bug reports) |

## Troubleshooting

The mod writes `log.txt` next to `config.json` (the mod's config folder), rewritten on every start. It records
the hooks that were installed, whether menu detection works, which draw calls the game uses, a short trace of
three menu frames, and a stat line every ~10 seconds. Send it along when reporting a problem.
The same lines go to logcat under the tag `BackgroundVideo`.

- `panorama replaced by video` means it is working.
- `signatures not found`: this Minecraft build changed the two functions; the patterns in `src/Signatures.cpp` need updating.
- `in menu ... but the panorama was never recognised`: the first draws of a menu frame do not look like six face quads; the trace in `log.txt` shows how the frame is drawn.
- `learned panorama faces: ...` means the six face textures were found.
- `cannot read a video at ...`: the path in `video_path` is wrong, or the launcher has no storage access to that file.
- For the smoothest result use an unrotated H.264 mp4 at 1080p or lower, 30 fps, around 4-8 Mbps. Very high resolutions or bitrates cost battery and can stutter on weaker phones.
- Needs Android 8 or newer (the video is opened before the game has a GL context).
