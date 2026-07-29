# NeoLIP

[![CI](https://github.com/vrifftech/NeoLIP/actions/workflows/ci.yml/badge.svg)](https://github.com/vrifftech/NeoLIP/actions/workflows/ci.yml)

NeoLIP is a wxWidgets application for generating KotOR-compatible `.lip` files from dialogue audio and transcripts using the bundled CSLU/Festival/OGI alignment assets and the KotOR pronunciation dictionary.

```text
WAV + TXT -> KotOR .lip
```

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

NeoLIP has no supported public command-line application. `neolip_backend` must remain beside the Windows executable, or in the same installed `bin` directory on Linux, so long-running jobs can be cancelled safely. Configuration now fails instead of silently producing an asset-only package when the GUI was requested but wxWidgets was not found.

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

The source tree includes the runtime assets under `third_party/cslu_runtime`. Preserve `docs/THIRD_PARTY_NOTICES.md`, `docs/attribution/`, and `docs/licenses/` in any redistribution containing those assets. `docs/CSLU_ASSETS.md` documents the retained asset set.

## LIP V1.0 format

NeoLIP uses one canonical reader and writer for KotOR `LIP V1.0` files:

```text
offset  size       value
0x00    8          "LIP V1.0"
0x08    4          duration as little-endian float32
0x0C    4          total record count as little-endian uint32
0x10    5 * count  records: float32 time followed by uint8 shape
```

The first record is explicitly stored as time `0.0`, shape `0`, and is included in the uint32 count. The editor hides that initial rest record while preserving it on disk. There is no 255-record limit.

## Shared game directories

The wxWidgets application exposes **File > Open Game Directory**. Its submenu lists every saved game install from the shared `neoshared` settings store; selecting an entry opens this application's supported-file dialog with that installation as the starting folder. **Manage Game Directories...** adds, renames, rescans, activates, or removes shared entries, and changes are visible in every Neo tool.

## Continuous integration

GitHub Actions checks out `vrifftech/NeoShared` beside this repository, then builds the full wxWidgets application on Ubuntu 24.04 and Windows Server 2025 with Visual Studio 2026. Successful non-pull-request runs publish one ready-to-run archive per platform:

```text
NeoLIP-linux-x86_64.tar.gz
NeoLIP-windows-x64.zip
```

The workflows build, install, and package the application, backend, models, and dictionaries. The portable Linux archive keeps the real GUI and its private backend together under `libexec/neolip/`; `bin/NeoLIP` is a launcher that selects the bundled wxWidgets runtime.

The shared dependency defaults to `NeoShared/main`. A manual workflow run or `neoshared-updated` repository-dispatch event can supply a branch, tag, or commit SHA. Each CI job verifies that both the GUI and its private backend are present before publishing an artifact.
