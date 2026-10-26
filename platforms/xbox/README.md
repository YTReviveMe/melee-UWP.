# Xbox Dev Mode

Build prerequisites: Visual Studio UWP C++ tools, LLVM clang-cl and llvm-rc,
Ninja, Python, MinGW x64 GCC, Rust nightly with `rust-src`, protoc, and an SDL3
UWP CMake package.

Build `nod-ffi` from `encounter/nod` commit `ebd80ca` in
`build-xbox-artifacts/nod-src`:

```powershell
New-Item -ItemType Directory build-xbox-artifacts -Force | Out-Null
git clone https://github.com/encounter/nod.git build-xbox-artifacts/nod-src
git -C build-xbox-artifacts/nod-src checkout ebd80ca
Push-Location build-xbox-artifacts/nod-src
rustup +nightly component add rust-src
cargo +nightly build -Z build-std=std,panic_abort --release --target x86_64-uwp-windows-msvc -p nod-ffi --no-default-features --features compress-bzip2-static,compress-lzma-static,compress-zlib-static,compress-zstd-static,threading
Pop-Location
```

Configure `melee` with `CMAKE_SYSTEM_NAME=WindowsStore`, clang-cl, llvm-rc,
`MELEE_XBOX_GCC`, `MELEE_XBOX_NOD_SOURCE`, `SDL3_DIR`, and `PROTOC_EXECUTABLE`.
Build the `melee` target, then package it:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File platforms/xbox/package.ps1 -OutputDirectory build-xbox-artifacts/release -Sign
```

The signed package, `MeleeUWP.cer`, and the Microsoft.VCLibs dependency are
written to `build-xbox-artifacts/release`. Install them through Xbox Device
Portal. The package does not include a disc image.
Read `licenses/LICENSE.md` before distributing the package.

Place an ISO, GCM, CISO, RVZ, GCZ, or WIA image in
`D:\developmentfiles\Melee` or on a USB drive. Use the launcher to select it.
The disc browser uses Left/Right for storage actions, Up/Down for files, A to
select, and B to go to the parent folder or close the browser.

On launch, the app creates `D:\developmentfiles\Melee` with `cache`, `textures`,
`music`, `saves`, and `settings`. Selecting a USB disc creates the same folders
under the drive's `Melee` directory except `cache`. Saves, settings, textures,
and music use the selected drive. Graphics and network caches always use D:.
Existing app data is copied to the internal folders on first launch.

Extract texture packs into the selected drive's `Melee\textures`. The loader
scans subfolders for Dolphin-style `tex1_*.png` and `tex1_*.dds` files.

Press Start + Select in game to open Settings. Slot A creates a memory card
under `Melee\saves`. The log is saved in the app's LocalState directory.

The game code is built with MinGW GCC to preserve big endian disc structures.
The UWP platform code uses clang-cl, and the nod disc reader uses Rust's
`x86_64-uwp-windows-msvc` target. Audio uses SDL for the AX callback clock and
XAudio2 for output. `MELEE_XBOX_AUDIO_DRIVER` can override the SDL driver for
local debugging.
