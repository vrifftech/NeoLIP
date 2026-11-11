# NeoLIP

[![CI](https://github.com/vrifftech/NeoLIP/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoLIP/actions/workflows/ci.yml)

NeoLIP is an application for creating KotOR-compatible `.lip` files from dialogue audio and transcripts using the bundled CSLU/Festival/OGI alignment assets and pronunciation dictionary.

Open WAV or MP3 audio, provide the spoken text, generate mouth movements, review them with audio playback, and save a `.lip` file.

Support is offered in the OpenKotOR Discord.

[![OpenKotOR Discord](https://discordapp.com/api/guilds/739590575359262792/widget.png?style=banner2)](https://discord.gg/OpenKotOR)

## Included source

```text
src/                         core, in-process alignment, and GUI
dictionaries/                bundled pronunciation lexicon
third_party/cslu_runtime/    required alignment models and dictionaries
docs/                        asset provenance, notices, attribution, and licenses
```

NeoLIP uses common infrastructure from the separate `neoshared` repository. Clone them as siblings:

```text
workspace/
  NeoShared/   (or neoshared/)
  NeoLIP/
```

CMake automatically detects either `../NeoShared` or `../neoshared`. For another layout, pass `--neoshared-root /path/to/neoshared` to `build.sh`, `-NeoSharedRoot C:\path\to\neoshared` to `build.ps1`, or set `NEOSHARED_ROOT` directly.

## Build

Linux GUI build:

```sh
sudo apt install build-essential cmake ninja-build libwxgtk3.2-dev
./scripts/build.sh --wx ON --require-wx ON --jobs "$(nproc)"
```

Windows GUI build with the shared, pinned wxWidgets 3.3.3 overlay:

```powershell
& ..\NeoShared\scripts\install-wxwidgets.ps1 `
  -VcpkgRoot C:\vcpkg `
  -Triplet x64-windows-static `
  -CleanAfterBuild

.\scripts\build.ps1 `
  -Wx ON `
  -RequireWx ON `
  -VcpkgRoot C:\vcpkg `
  -VcpkgTriplet x64-windows-static `
  -Parallel ([Environment]::ProcessorCount)
```


## Use NeoLIP

Launch `NeoLIP` (`NeoLIP.exe` on Windows). The application finds its bundled models and dictionaries automatically; keep the installed package together.

The bundled alignment assets are for English speech. Transcripts must match the actual recording.

The application has two tabs: **Single LIP** for generating and editing individual files, and **Batch** for processing multiple recordings.

### Single LIP

1. Open or drop a WAV or MP3 recording. A same-name `.txt` or `.lab` transcript loads automatically when available. You can also use **Load transcript...** or enter the text in **Spoken text**.
2. Match the words actually spoken in the recording. Write numbers as spoken words and replace dialogue tokens with what the speaker says. Select a name, number, or phrase and use **Say as...** to replace it with its spoken form. This edits the visible transcript, leaving the original transcript file unchanged.
3. Click **Generate**. The result opens in the editor as an unsaved LIP.
4. Play the recording and review the waveform, mouth-key timeline, and keyframe table. Drag keys or edit their times and mouth shapes in the table. Use **Sections to review** and **Review section** when suggestions are available. If you correct the transcript or add pronunciations, generate again to realign the line before making final keyframe edits.
5. Click **Save** or press **Ctrl+S** to write the `.lip`. The default destination has the audio's name and folder. Use **File > Save As...** (**Ctrl+Shift+S**) to choose another destination.

If generation requests a voice channel or reports unknown words, resolve those choices and use **Retry**. Pronunciations can be added through **Add pronunciations** or **Tools > Pronunciations...**.

To edit an existing `.lip`, open or drop it into the same workspace. Matching WAV or MP3 audio beside the LIP loads when available. A LIP contains mouth keys and duration, not the original transcript or word alignment.

### Batch

Choose a **Source**:

- **Audio + transcript files:** select a folder containing WAV or MP3 recordings with same-name `.txt` transcripts. **Options > Include subfolders in audio batches** controls recursive scanning and is enabled by default.
- **Dialogue (DLG):** select a `.dlg` and a **Game directory / search folder** containing its recordings. NeoLIP reads the dialogue's text and audio references, searches the folder and its subfolders, and resolves talk-table text when available. Use **Talk table override** to supply a different `.tlk`, such as a mod's talk table. **File > Use Game Directory for Dialogue** selects a saved game directory.

Dialogue batches require a loose `.dlg` and accessible WAV or MP3 recordings. Extract resources stored inside module or other game archives before using them here.

Review the scan before starting. Open a dialogue row to correct its recording or spoken text, or exclude a line. These corrections apply only to the batch; they do not modify the DLG or TLK. Missing audio and unresolved text must be supplied or corrected before those lines can be generated.

Audio/transcript batches save beside each recording by default. DLG batches save to a `<dialogue>_lips` folder beside the DLG. Use **Output folder...** to choose another destination; audio batches preserve subfolders under that destination. Select **Skip existing** or **Replace existing**, then click **Create LIP files** (the button includes the number of files). Batch generation writes the LIPs directly.

Use the result filters and **Previous problem** / **Next problem** to find pronunciation requests, channel choices, suggested review sections, or failed files. After correcting a problem, **Retry** processes unsuccessful items. Open a completed row with **Open selected** or a double-click to review and edit its LIP in **Single LIP**; save any edits there.

### Pronunciations

Use **Say as...** for a spoken form specific to the current transcript. For a reusable word pronunciation, use **Tools > Pronunciations...** to check a word, add or edit an entry, remove a custom entry, or import a dictionary. **Check folder** finds unknown words in transcript files before a batch run.

Custom pronunciations are saved separately from the bundled dictionary. After changing one, regenerate or retry the affected lines so the alignment uses the correction.

### Mouth movement

Choose **Options > Mouth movement**:

- **Natural** (default): broader blends, consonant preparation, and cleanup of brief non-contact reversals.
- **Minimal smoothing:** short transitions that retain each mapped mouth target, without consonant preparation.

Both use the same audio/text alignment. The setting changes how mouth targets become motion. Changing it regenerates the current single-line result when its source is available; review and save that result again. Use **Replace existing** when rerunning a batch to update previously generated files.
