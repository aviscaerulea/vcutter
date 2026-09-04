# avply

[![日本語](https://img.shields.io/badge/lang-日本語-red)](README.md)
[![English](https://img.shields.io/badge/lang-English-blue)](README.en.md)
[![Release](https://img.shields.io/github/v/release/aviscaerulea/avply)](https://github.com/aviscaerulea/avply/releases/latest)
[![License](https://img.shields.io/github/license/aviscaerulea/avply)](LICENSE)
[![Build](https://github.com/aviscaerulea/avply/actions/workflows/release.yml/badge.svg)](https://github.com/aviscaerulea/avply/actions/workflows/release.yml)

avply is a media player for reviewing meeting recordings quickly, clearly, and only where it matters.
Faster playback and voice enhancement cut down the time you spend watching, and you can pull out just the part you need.
It starts up light, so it also works well as an everyday video and audio player.

![avply screenshot](docs/images/screenshot.png)

## Features

- Fast startup: little delay between launching and playing
- Speed control: change playback speed in 0.05 steps while keeping the original pitch
  (kept across files)
- Voice enhancement: automatically evens out volume differences between speakers
  and lifts quiet remarks
- Output device follow: switches the output when the OS default audio device changes,
  even during playback
- Seek bar preview: shows a thumbnail and timestamp for the position under the cursor
- Waveform display: draws the waveform of the loaded audio over the seek bar
- Fast trimming: cuts the selected range at keyframe boundaries without re-encoding
- Conversion: re-encodes video to AV1 + Opus and audio to Opus

### Supported file formats

| Type | Extensions |
| --- | --- |
| Video | mp4, mkv, mov, avi, webm |
| Audio | mp3, wav, flac, ogg, opus |

When you load an audio file, the window switches to a compact layout without the preview area.

### Voice enhancement

In meeting recordings, remarks made far from the microphone sound quiet while nearby ones sound loud.
Voice enhancement combines noise suppression and automatic gain control to even out that difference during playback.
Pressing the C key toggles it on and off.
It always starts off at launch, and the setting is not saved.

## Installation

### Requirements

- Windows 11
- ffmpeg (installed separately; it is also used to read media information during playback)
- NVIDIA GPU (only needed for video conversion; AV1 NVENC support required,
  RTX 30 series or later recommended)

Trimming does not re-encode, so no GPU is required. Audio-only conversion also runs on the CPU.

### Steps

#### From the release ZIP

Download `avply-<version>-x64.zip` from [Releases](https://github.com/aviscaerulea/avply/releases). Then extract the downloaded file. Finally, run `avply.exe`.
In that case, install ffmpeg separately (`scoop install ffmpeg` or an [official build](https://www.gyan.dev/ffmpeg/builds/)).

#### From Scoop

Recommended when Scoop is available. ffmpeg is pulled in as a dependency.

```powershell
scoop bucket add aviscaerulea https://github.com/aviscaerulea/scoop-bucket
scoop install aviscaerulea/avply
```

#### Uninstall

Even after removing the app, the settings changed from the right-click menu (such as always-on-top) remain in the registry under `HKCU\Software\avply`.
To remove them completely, delete that key with the Registry Editor.

## Usage

### Loading a file

You can load a file in any of the following ways.

- "Open file" in the right-click menu
- Drag and drop onto the window
- Drag and drop onto `avply.exe`, or use "Send to" / "Open with" in Windows
- Run `avply.exe <media file>` from the command line

Loading the same file again restarts playback from the beginning.

### Keyboard and mouse

Playback controls are as follows.

| Action | Key | Mouse |
| --- | --- | --- |
| Play / pause | Space | Click the preview area (not available for audio-only files) |
| Seek | ← → | Drag the seek bar, or scroll over the seek bar or preview area |
| Previous / next file in the folder | Alt+← / Alt+→ | |
| Playback speed ±0.05x | `.` faster / `,` slower | Ctrl+wheel |
| Volume ±0.05 | ↑ ↓ | Shift+wheel |
| Switch voice enhancement | C | |
| Reset playback settings | G | |

Trimming controls are as follows.

| Action | Key | Mouse |
| --- | --- | --- |
| Set the start of the range | `[` | 【 button |
| Set the end of the range | `]` | 】 button |
| Clear the range only (playback position kept) | R | |
| Run / cancel trimming | | ✂ button |

The first press of G returns to neutral values (speed 1.00, volume 100%, voice enhancement off), and the second press restores the speed and volume from startup.
File switching follows the file-name order and stops at the first and last files in the folder.
The bottom of the window always shows the current playback speed, volume, and voice enhancement state.

### Trimming and conversion

Select a range first, then run the trim.
The selected range is highlighted in red on the seek bar.
Because nothing is re-encoded, saving runs at close to disk-copy speed.

Conversion runs from "Convert file" in the right-click menu.
Video is re-encoded to AV1 + Opus, and audio to Opus at 96kbps.
Video wider than 2048px is scaled down automatically while keeping its aspect ratio.

### Output files

Output goes to the same folder as the input, named `<original name>_mod.<extension>`.
If that name already exists, a number is appended, as in `_mod2` or `_mod3`.
Processing a file that already has a `_mod` suffix overwrites the file with the same name.

| Mode | Input | Output extension |
| --- | --- | --- |
| Conversion | Video | `.mp4` (AV1 + Opus) |
| Conversion | Audio | `.opus` |
| Trimming | Video and audio | Same as the input |

## Configuration

Behavior is adjusted in `avply.toml`, located in the same folder as the executable.
To keep machine-specific values out of the repository, write the same keys in `avply.local.toml` in that folder; those values take precedence.
The main entries are listed below. Default values and valid ranges for each key are documented in the comments inside `avply.toml`.

| Section | Contents |
| --- | --- |
| `[ffmpeg]` | Path to ffmpeg.exe |
| `[seek]` | Seek amounts for arrow keys and the wheel |
| `[playback]` | Initial playback speed, hardware decoder priority |
| `[window]` | Maximum window size on load (ratio of the monitor) |
| `[audio]` | Initial volume, silence tone |

The ffmpeg path is resolved in this order: `path` under `[ffmpeg]`, the default Scoop location, then the `PATH` environment variable.
No configuration is needed if ffmpeg is available through Scoop or `PATH`.
To set it explicitly, write it as follows.

```toml
[ffmpeg]
path = "C:/Users/yourname/scoop/apps/ffmpeg/current/bin/ffmpeg.exe"
```

Always-on-top during playback, single-instance enforcement, and process priority are toggled from the settings in the right-click menu.
These are stored in the registry and kept for the next launch.

## Limitations

- Trimming cuts at keyframe boundaries, so the start position is rounded back
  from the one you specified
- With some formats such as Ogg/Opus, the playback position right after a trim can be off
  by a few tens of milliseconds
- Volume is capped at 100%; amplification beyond that is not supported
  (use voice enhancement to lift quiet remarks)
- Video conversion requires an NVIDIA GPU with AV1 NVENC support

## Build

The following tools are required.

- Visual Studio 2026 Build Tools (C++ workload)
- CMake 3.25 or later
- Qt 6.10.3 MSVC2022 x64

Qt can be installed with the following command.
Match the install location with `CMAKE_PREFIX_PATH` in `CMakePresets.json`.

```powershell
python -m aqt install-qt windows desktop 6.10.3 win64_msvc2022_64 --outputdir <install folder> --modules qtmultimedia
```

Build with the following command.
The executable is generated at `out/Release/avply.exe`.

```powershell
pwsh.exe -File build.ps1
```

## License

avply itself is distributed under the GNU LGPL v3.
See the bundled `LICENSE` (LGPL v3) and `COPYING` (GPL v3) for the full text.

License handling for the dependencies is as follows.

- Qt 6.10 (LGPL v3): linked dynamically as DLLs, so users can replace Qt by swapping
  in DLLs of the same name
- SoundTouch 2.4.0 (LGPL v2.1 or later): linked statically, so avply is licensed
  under LGPL v3 to preserve the right to relink
- WebRTC Audio Processing (BSD): linked statically; BSD is compatible with LGPL v3
  and adds no further redistribution obligations
- ffmpeg: invoked as an external process, so no linking relationship arises
