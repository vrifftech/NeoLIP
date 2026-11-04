# NeoLIP

[![CI](https://github.com/vrifftech/NeoLIP/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoLIP/actions/workflows/ci.yml)

NeoLIP is a wxWidgets application for generating KotOR-compatible `.lip` files from dialogue audio and transcripts using the bundled CSLU/Festival/OGI alignment assets and the KotOR pronunciation dictionary.

```text
WAV + TXT -> KotOR .lip
```

Support is offered in the OpenKotOR Discord.

[![OpenKotOR Discord](https://discordapp.com/api/guilds/739590575359262792/widget.png?style=banner2)](https://discord.gg/OpenKotOR)

## Included source

```text
src/                         core, GUI, and private cancellable worker
dictionaries/                KotOR pronunciation overrides
third_party/cslu_runtime/    required alignment models and dictionaries
docs/                        asset provenance, notices, attribution, and licenses
```

NeoLIP consumes common wxWidgets infrastructure from the separate `neoshared` repository. Its private TLK unknown-word scanner also uses `neoshared::tlk` instead of carrying a second binary TLK parser. Clone them as siblings:

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

A successful GUI build produces `build/NeoLIP` on a single-configuration Unix build or `build/Release/NeoLIP.exe` with the Visual Studio Release configuration. The build scripts fail if the GUI was requested but that executable was not produced.

To create the same self-contained tree published by GitHub Actions:

```sh
cmake --install build --config Release --prefix stage
```

The resulting application is `stage/bin/NeoLIP` on Linux or `stage\NeoLIP.exe` on Windows. Models and dictionaries are installed under the same `stage` root.

NeoLIP builds the desktop application by default. A normal successful build produces both:

```text
NeoLIP              desktop application
neolip_backend      required private worker
```


## Use NeoLIP

Launch `NeoLIP` (`NeoLIP.exe` on Windows), then select the acoustic-model folder once under **Settings > Models...**. NeoLIP remembers that active Models path for subsequent launches.

The application has three main workspaces:

### Create

Select one dialogue WAV, provide its spoken transcript as a `.txt`/`.lab` file or type the text directly, choose the output `.lip`, and click **Create LIP**. Pronunciation, smoothing, pause handling, and report controls are available under the collapsed **Advanced options** section.

```text
WAV + TXT -> KotOR .lip
```

### Edit

Open an existing `.lip` and edit it in one synchronized workspace. The waveform, optional PHN alignment overlay, visual key timeline, and numeric keyframe table all represent the same document. Drag timeline keys or edit the table, then save the result.

### Batch

Select a folder containing same-name WAV and TXT/LAB pairs and an output folder. NeoLIP generates one `.lip` for each pair. Parallelism, recursive scanning, pronunciation, and output-quality controls are under **Advanced options**.

Pronunciation maintenance is available from **Tools > Pronunciation Dictionary...**. It combines unknown-word scanning, dictionary-stub generation, dictionary opening, and single-word lookup in one dialog.

Pronunciation and alignment intermediates remain internal to WAV/TXT-to-LIP generation. PHN files may optionally be loaded as a visual overlay in the editor.

The Windows package puts `NeoLIP.exe` at its top level. `neolip_backend.exe`, `models/`, and `dictionaries/` are required runtime components; the workflow refuses to publish an archive unless all four are present.

The source tree includes the runtime assets under `third_party/cslu_runtime`.

