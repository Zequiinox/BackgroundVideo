# BackgroundVideo

A native mod for [LeviLaunchroid](https://github.com/LiteLDev/LeviLaunchroid) that plays your own video
as the main menu background **instead of the Minecraft panorama**.
Built for Minecraft Bedrock 26.62, Android arm64-v8a.

## How it works

In 26.x the menu buttons are a separate HTML-based UI drawn on top of a native cubemap panorama
(`CubemapBackgroundScreen` / `PanoramaRenderer`). The mod replaces that panorama in place:

1. It hooks the GL draw calls in `libGLESv2.so` (the same technique BedrockTools' Motion Blur uses for `glClear`).
2. While no world is loaded, any draw that uses a cube map sampler is the panorama. That draw call is
   skipped and the video is drawn into the same render target instead, so the buttons still sit on top.
3. Menu detection reuses two BedrockTools signatures (`ClientInstance::update` and `getLocalPlayer`),
   which all match exactly once in the 26.62 `libminecraftpe.so`.
4. Decoding and optional audio use Android's `MediaPlayer` over JNI, so any mp4 the phone can play works.
5. The video loops while the menu shows and pauses when you join a world.

If the video is missing or anything fails, the mod steps aside and the normal panorama is shown.

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

## Troubleshooting (adb logcat -s BackgroundVideo)

- `hooks installed` / `game hooks installed` / `state: menu` confirm the setup. 
- `panorama replaced by video` means it is working.
- `cannot read a video at ...`: the path in `video_path` is wrong, or the launcher has no storage access to that file.
- `signatures not found`: this Minecraft build changed the two functions; the patterns in `src/Signatures.cpp` need updating.
- `in menu ... but no cubemap draw was seen`: the panorama is not drawn with a cube map sampler on this build/renderer.
- Use an unrotated, H.264 mp4 at or below your screen resolution for the smoothest result.
