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
tests/                       optional core I/O test
tools/                       source-tree smoke-test scripts
```

NeoLIP consumes common wxWidgets infrastructure from the separate `neoshared` repository. Its private TLK unknown-word scanner also uses `neoshared::tlk` instead of carrying a second binary TLK parser. Clone them as siblings:

```text
workspace/
  neoshared/
  NeoLIP/
```

CMake automatically detects `../neoshared`. For another layout, pass `--neoshared-root /path/to/neoshared` to `build.sh`, `-NeoSharedRoot C:\path\to\neoshared` to `build.ps1`, or set `NEOSHARED_ROOT` directly.

## Build

Linux GUI build:

```sh
sudo apt install build-essential cmake ninja-build libwxgtk3.2-dev
./scripts/build.sh --wx ON --require-wx ON --jobs "$(nproc)"
```

Windows GUI build with the shared, pinned wxWidgets 3.3.3 overlay:

```powershell
& ..\neoshared\scripts\install-wxwidgets.ps1 `
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

NeoLIP has no supported public command-line application. `neolip_backend` is a private worker installed beside the GUI so long-running jobs can be cancelled safely.

## Use NeoLIP

Launch `neolip_gui`, select the installed NeoLIP runtime root or its `models` directory, then use the Align, Batch, Unknown words, PHN preview, LIP editor, timeline, Text to NTP, or Dictionary lookup views.

The source tree includes the runtime assets under `third_party/cslu_runtime`. Preserve `docs/THIRD_PARTY_NOTICES.md`, `docs/attribution/`, and `docs/licenses/` in any redistribution containing those assets. `docs/CSLU_ASSETS.md` documents the retained asset set.

## Shared game directories

The wxWidgets application exposes **File > Open Game Directory**. Its submenu lists every saved game install from the shared `neoshared` settings store; selecting an entry opens this application's supported-file dialog with that installation as the starting folder. **Manage Game Directories...** adds, renames, rescans, activates, or removes shared entries, and changes are visible in every Neo tool.

## Continuous integration

GitHub Actions checks out `vrifftech/neoshared` beside this repository, then builds the full wxWidgets application on Ubuntu 24.04 and Windows Server 2025 with Visual Studio 2026. Successful non-pull-request runs publish staged Linux and Windows artifacts.

The shared dependency defaults to `neoshared/main`. Set the repository Actions variable `NEOSHARED_REF` to a release tag or commit SHA to pin normal CI builds. A manual workflow run can override the ref, and the workflow accepts the `neoshared-updated` repository-dispatch event for cross-repository compatibility checks.
