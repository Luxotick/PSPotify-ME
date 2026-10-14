# PSPotify ME: setup and troubleshooting

## Install

Copy the `PSP` folder from the build to the root of the Memory Stick:

```text
ms0:/PSP/GAME/PSPOTIFY/EBOOT.PBP
ms0:/PSP/GAME/PSPOTIFY/THEMES/...
```

The folder must be writable. On start the app writes the following files into it:
- `kcall.prx` (the embedded Media Engine bridge),
- `spotify.cfg` (your sign-in),
- `settings.ini` (theme, Cover colors, equalizer, shuffle and repeat),
- `spotify.log`.

Without the `THEMES` folder the app still runs, with its built-in dark theme.

You do not need to install any plugin or edit any file by hand.

## First start: pairing

1. Make sure a Wi-Fi profile is saved under *Settings → Network Settings*. The app connects with the first profile that works.
2. Start **PSPotify ME**. After the Wi-Fi connects, the PSP shows a six-letter code.
3. Open **spotify.com/pair** on any phone or computer, sign in and enter the code. The approval page names the client "Spotify for Desktop".
4. The PSP checks every few seconds and continues by itself. The code is valid for about 15 minutes.

Pairing only happens once. Later starts refresh the stored token silently.

## Moving an existing install

If you already have a `spotify.cfg` from an older build, copy it into `ms0:/PSP/GAME/PSPOTIFY/` and the app will skip pairing.

## Troubleshooting

Every run writes `spotify.log` next to the EBOOT. Each line is stamped with the seconds since start. Attach this file to bug reports.

| Symptom | What to check |
|---|---|
| "NETWORK FAILED" | Wi-Fi switch on, a saved profile, router reachable |
| Stuck on "Signing in..." | Look for `OAUTH` / `TLS` lines in the log. A `BearSSL err 62` means a certificate was not trusted. A `53`/`54` means the PSP clock is wrong. |
| Library empty | Look for `LIBRARY: HTTP …` lines |
| Track never starts | Look for `SESSION: audio key failed` or `PLAYER` lines. `AUDIOKEY: … error code 0x0001` means Spotify refused the track: usually no Premium. |
| Audio stutters | Look at the `PLAYER: 10 s played, … KB fetched, … waits` lines: many waits with little fetched means the download is too slow (Wi-Fi signal). |
| Search or Recent stays empty | Look for `SEARCH:` / `RECENT:` lines and an HTTP status just before them |
| "Media Engine stopped responding" | A decode timed out. Press START to reboot the PSP; the app never unloads while the Media Engine may still run. |
| Sign-in suddenly fails | Delete `refresh_token` from `spotify.cfg` (or the whole file) to pair again |

## Revoking access

`spotify.cfg` holds a refresh token and reusable credentials in plain text. To cut off a lost Memory Stick, remove "Spotify for Desktop" under *Account → Apps* on spotify.com, then pair again.
