# Playable CI builds

`recomp-jp.tar.gz` contains only generated C, headers and the CMake source list. It contains no disc image,
PS-X executable, overlay binary, textures, audio, captions, saves or settings. The resulting game still reads
the user's JP disc at runtime; the USA disc is optional. No build secret is needed.

The **Playable builds** workflow runs on pushes to `main`, `v*` tags, or manually from Actions. It publishes:

- Windows x64 ZIP (self-contained executable).
- Linux x64 tar.gz.
- macOS Apple Silicon and Intel tar.gz files, including a double-clickable launcher.
- Physical-device iOS arm64 IPA, unsigned for the user's sideloading tool to sign.

Downloads are workflow artifacts, retained for 90 days. macOS builds are not notarized. The separate **CI**
workflow continues running synthetic-disc smoke tests, including iOS simulator/device compilation.

After changing the generator, seeds, JP symbols or overrides, regenerate locally and refresh the bundle:

```sh
python -I tools/recomp/gen.py build/exe/SCPS_180.11 build/gen_jp --seeds tools/recomp/seeds_jp.txt \
  --overlays build/exe/ov 801c5110 --overrides tools/recomp/overrides_jp.txt --symbols tools/recomp/symbols_jp.txt
python -I tools/ci_recomp_bundle.py pack
```

Commit the updated source bundle with those changes. CI checks the generator fingerprint and file hashes,
and rejects a stale bundle instead of producing a game with outdated hooks. Packaging copies only explicit
binary/bundle paths and includes the SDL/ImGui licenses. Package and source-bundle checks run with:

```sh
python -I tests/ci_tools_test.py
```
