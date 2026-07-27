# Bundled CSLU runtime assets

This directory contains the minimum CSLU/Festival/OGI runtime assets NeoLIP needs to build a self-contained runtime bundle.

NeoLIP uses these files only as runtime data: forced-alignment model files, CSLU CMU dictionary files, OGI lexicon/addenda, and the original CSLU sample files.

NeoLIP's CMake installation rules use:

```text
third_party/cslu_runtime/Toolkit/2.0/apps/fa
```

Do not remove this directory from source distributions intended to produce a self-contained NeoLIP runtime without a separate CSLU Toolkit installation.

Preserve `docs/THIRD_PARTY_NOTICES.md` and `docs/licenses/` when redistributing builds that include these assets.
