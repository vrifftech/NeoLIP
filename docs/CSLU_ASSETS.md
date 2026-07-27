# CSLU assets bundled with NeoLIP

The NeoLIP source tree includes the minimum CSLU/Festival/OGI runtime assets required by the application.

Bundled source asset root:

```text
third_party/cslu_runtime/Toolkit/2.0/apps/fa
```

NeoLIP's CMake installation rules use that path when producing a self-contained runtime. Users of an installed asset-bundled build do not need a separate CSLU installation.

## Required runtime layout after bundling

```text
NeoLIP/
  NeoLIP.exe                    Windows desktop application
  neolip_backend.exe            Windows private GUI worker
  bin/
    NeoLIP                      Linux desktop launcher
    neolip_backend              Linux private GUI worker
  models/
    phonemes_new_16k.train.spec
    phonemes_new_16k.parts
    bc_16k/
    trans_16k/
  dictionaries/
    custom_dictionary.txt
    cmu/*.ob
    ogi_lex.out
    ogi_lex_addenda.scm
    kotor_override.dict
  docs/
    THIRD_PARTY_NOTICES.md
    licenses/
    attribution/
  examples/
    test.wav
    test.txt
    test_manual.ntp
    test_automatic.ntp
```

## Asset groups

Model assets:

```text
phonemes_new_16k.train.spec
phonemes_new_16k.parts
bc_16k/
trans_16k/
```

Dictionary assets:

```text
custom_dictionary.txt
cmu/*.ob
ogi_lex.out
ogi_lex_addenda.scm
kotor_override.dict
```

The KOTOR override is maintained by NeoLIP. The CSLU CMU dictionary, OGI lexicon, and OGI addenda are third-party runtime assets.

## License files

Runtime bundles that include CSLU/OGI/Festival assets must preserve:

```text
docs/THIRD_PARTY_NOTICES.md
docs/licenses/CSLU_Toolkit_license.txt
docs/licenses/CSLU_Toolkit_license.ogi.txt
docs/licenses/OGI_TTS_license.txt
docs/licenses/Festival_Copying.txt
docs/licenses/Festival_master_COPYING.txt
docs/licenses/Speech_Tools_master_COPYING.txt
docs/attribution/WOOKIEEPEDIA_PRONUNCIATION_AUDIT.tsv
```

Confirm the intended redistribution model before publishing an asset-bundled binary.

## Using a different asset set

Replace the files below `third_party/cslu_runtime/` while preserving the documented directory layout. Review and retain the applicable notices, attribution, and license texts before distributing the replacement assets.
