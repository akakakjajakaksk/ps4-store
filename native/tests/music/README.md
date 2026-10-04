# Native music checks

From the repository root:

```sh
g++ -std=c++11 -O2 -Wall -Wextra -Werror -pthread \
  -Inative/tests/music/stubs native/tests/music/music_test.cpp -o /tmp/peppy-music-tests-bin
/tmp/peppy-music-tests-bin
```

The tests use synthetic PCM fixtures, not copies of the user's songs. They check
RIFF chunk bounds, unsupported formats, truncated/misaligned samples, padded final
buffers, both songs in a repeated playlist, software volume/mute, safe buffer
reuse, stop/restart cleanup and original native audio error returns. Missing tracks
and unavailable audio ports fail independently of the store's UI and downloader.

The packaged tracks are prepared from the user's existing repository files:

```sh
python3 native/scripts/prepare-music.py --repo-root . --output native/assets/music
```

This requires `ffmpeg` and `ffprobe` during the build. The runtime reads only a
single fixed 256-frame PCM block at a time on its worker; no runtime decoder,
music network request or full-song allocation is needed. Original MP3/M4A files
remain untouched. The two generated WAV files must be listed in `pkg.gp4` as
`assets/music/fight.wav` and `assets/music/acendaofarol.wav`.

`musicStart` synchronously validates the small headers and holds both file
descriptors before starting its worker. Playlist repeats and track switches seek
those descriptors without resolving an asset path again. This preserves playback
while the local installer temporarily changes the process root. Tests remove the
song paths after start and verify repeated playback, mute and next-track control.

Audio signatures, native already-initialized return code and output-buffer wait
behavior follow the public OpenOrbis v0.5.4 headers and `samples/audio-wav`.
Console playback still needs an actual PS4 test.

The MAIN output uses `ORBIS_USER_SERVICE_USER_ID_SYSTEM` (0xFF), following the
[OpenOrbis v0.5.4 audio example](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain/blob/v0.5.4/samples/audio-wav/audio-wav/main.cpp).
Passing the controller's profile ID returned `0x809B0001` on the user's PS4;
the public [device-service error definitions](https://github.com/shadps4-emu/shadPS4/blob/main/src/core/libraries/pad/pad_errors.h)
identify this as `INVALID_USER`. The audio mock reproduces that rejection for
profile IDs and confirms playback opens the system mixer instead.
