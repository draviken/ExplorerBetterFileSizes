# ExplorerBetterFileSizes

A standalone port of m417z's Windhawk mod [Better file sizes in Explorer details](https://github.com/ramensoftware/windhawk-mods/blob/main/mods/explorer-details-better-file-sizes.wh.cpp) (v1.6) for Windows 10/11 x64.

- Show folder sizes using Everything or folder scanning
- Display large file sizes in MB/GB, or keep KB only
- Mix files and folders when sorting by size
- Use IEC unit names (KiB, MiB)
- Use `B` instead of the localized word for bytes

## Install

1. Download the ZIP from [Releases](https://github.com/draviken/ExplorerBetterFileSizes/releases/latest) and extract it.
2. Edit `config.ini`. Folder sizes are disabled by default; the comments explain each option.
3. Run `install.cmd` as administrator. This restarts Explorer.

Initial setup and some Windows updates require an internet connection.

After changing settings, rerun `install.cmd`. Restart Everything too if it is running.

To uninstall, run `uninstall.cmd` as administrator. Restart Everything before deleting the extracted folder.

## Build

Requires llvm-mingw and GNU make.

```sh
make dist CC=x86_64-w64-mingw32-clang
```

Output: `dist/`.

## Acknowledgments

- [m417z](https://github.com/m417z) for explorer-details-better-file-sizes
- [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu
- [Everything](https://www.voidtools.com/) and its SDK by voidtools

## License

GPL-3.0. MinHook is BSD-2-Clause.
