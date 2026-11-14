<p align="center"><img src="docs/logo.webp" width="160" alt="DSi FTP logo"></p>

# DSi FTP — NAND + SD over Wi-Fi

FTP server for the **Nintendo DSi** that exposes the SD card **and the internal NAND** over Wi-Fi (WPA2 supported). Browse and back up the system memory from a PC with Windows Explorer, FileZilla or any FTP client, without removing the SD card.

**Status: beta.** Tested on one console and one router; see [What has been tested](#what-has-been-tested).

| Path | Content | Default access |
|---|---|---|
| `/sd` | SD card | read/write |
| `/nand` | internal NAND, main partition | read-only |
| `/nand2` | NAND photo partition | read-only |

## Features

- DSi-mode Wi-Fi: open, WEP and **WPA2** networks (WPA3-only networks are not supported by the DSi hardware).
- Network scan, on-screen keyboard, saved networks with a default one joined at start-up; DHCP or a static IP per network; automatic retries if the association gets stuck.
- Interface in **8 languages**: English, Italiano, Español, Français, Deutsch, Русский, 中文, 日本語 (the DSi system language by default).
- Touch and button controls; bottom-screen tabs: Console, Server, Network, Access.
- Login with a random PIN (new at every start) or a custom user name and password; configurable port.
- Login lock per client address (after 3/5/10 failures, for 30 s/2 min/10 min), list of blocked addresses, kick clients.
- Independent permissions: SD read, SD write, NAND read, NAND write, NAND full write.
- Optional diagnostic log, including the messages of the Wi-Fi driver.

## Requirements and installation

- A Nintendo DSi with [Unlaunch](https://dsi.cfw.guide/) (or TWiLight Menu++ launching in DSi mode) and an SD card.
- Copy `dsiftp.dsi` to the SD card and start it. It only runs in DSi mode.

On the PC open `ftp://<address shown on the DSi>` in Windows Explorer. Only **passive mode** is supported (data ports 50000-50999). In FileZilla choose "Only use plain FTP": the server has no TLS.

Files on the SD card: `_nds/dsiftp/settings.ini`, `networks.ini`, `log.txt` (only when the log is on) and `nand_backup/`.

## Transfer speed

Measured on one console and one router: about **700-800 KB/s uploading** to the SD card and about **600 KB/s downloading** from it. A 512 MB file takes roughly 11 minutes to upload; for files of that size a card reader is still much faster.

The server has a built-in speed test that does not touch the SD card, to tell the Wi-Fi link from the card: a transfer from or to the name `.dsiftp-speedtest`, in any folder, sends 32 MB of zeros or discards what it receives.

```sh
curl -o NUL ftp://USER:PASSWORD@ADDRESS/.dsiftp-speedtest          # download
curl -T some_big_file ftp://USER:PASSWORD@ADDRESS/.dsiftp-speedtest  # upload
```

## NAND writing

Writing to the NAND can **brick** the console with no software recovery. For this reason:

- NAND writing is **off at every start** and can only be enabled **on the DSi itself** (Permissions menu, hold L+R and press A), never from the PC.
- Even with writing enabled these stay read-only: the launcher and Unlaunch, `sys/`, `shared1/`, system titles, DSi FTP itself and the top-level folders.
- Names the filesystem would read differently from how they are written (ending in a dot or a space, or containing `\`, `:` or `~`) are refused on the NAND, so they cannot be used to reach a protected path. If you ever need such a name, set `nand_strict_names=0` in `settings.ini`.
- Every file is copied to `_nds/dsiftp/nand_backup/` before it is changed or deleted, and every operation is written to `nand_backup/journal.txt`. Files are replaced through a temporary copy, so an interrupted upload leaves the original intact.
- Writing needs the charger or a battery level of at least 8/15, and is switched off automatically below 4/15.

**Full NAND write (experimental)** removes the protected paths (L+R+X, then a 5-second countdown; the screens turn red). Turning it on has been tested; writing in this mode has **not**. Use it only if you have a NAND backup and know how to restore it.

## Security notes

- FTP is not encrypted: on the local network the password travels in clear text.
- Wi-Fi passwords and the custom FTP password are stored in plain text on the SD card.
- `log.txt` contains the DSi MAC address and the names of nearby and saved Wi-Fi networks: remove them before posting a log publicly.

## What has been tested

| Tested | Not tested yet |
|---|---|
| EUR DSi with Unlaunch, one WPA2 router | other consoles and routers, open/WEP networks |
| DHCP, static IP, switching between them | the automatic retry after a stuck association |
| Windows Explorer and FileZilla | running as a title installed to the DSi Menu |
| protected NAND write: refusals, write, overwrite, delete | writing in full NAND write mode |
| all 8 interface languages, full write mode activation | |
| repeated transfers from 4 MB to 128 MB, compared byte for byte with the original | files larger than 128 MB |
| clients that open a data connection and leave without a transfer | |

**Known issue:** on some start-ups the first association stays stuck in the Wi-Fi library. The app retries up to 3 times; if it still fails, switch the DSi off and on.

## Building

Requires [BlocksDS](https://blocksds.skylyrac.net/) (tested with 1.25.0 through the Wonderful toolchain):

```sh
bash build.sh               # builds dsiftp.dsi
bash build.sh WIFI_DEBUG=0  # release ARM7 core, without Wi-Fi driver messages
```

DSi FTP links against a patched DSWiFi (a larger TCP receive window, which roughly doubles the upload speed; see `patches/README.md`). The first `build.sh` downloads the BlocksDS source with git, applies the patch and builds the library into `~/.cache/dsiftp`; the installed BlocksDS is not modified. `STOCK_DSWIFI=1 bash build.sh` builds with the unmodified library instead.

- Translations: `source/lang_strings.h` (English, widths) and `source/lang_xx.c`. Check them with `python3 tools/check_lang.py`.
- Fonts: `source/font_data.c` is generated by `tools/mkfont.py` (see `THIRD_PARTY_NOTICES.md`).

The translations were made with AI assistance and may contain mistakes: corrections from native speakers are welcome.

## Credits and licence

MIT licence (see `LICENSE`). Third-party components and fonts: `THIRD_PARTY_NOTICES.md`.

This project was developed with the help of an AI assistant (Claude); design decisions, soft code verification and all testing on real hardware were done by a human.

DSi FTP is not affiliated with or endorsed by Nintendo.
