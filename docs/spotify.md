# Spotify on the TempoTec V1

The V1 plays Spotify through **q2-librespot**, the librespot fork written for the Shanling Q2 by [diamondBond](https://github.com/diamondBond/q2-librespot) (branch `shanlingq2`). It is a Spotify Connect receiver: your phone picks the player in its device list and casts to it. The fork adds three interfaces an unmodified librespot does not have — a status file, a control socket and a browse socket — and those are the whole reason it fits here: through them the player's own interface can show what is playing, browse your library, and drive the transport.

None of Spotify's protocol is implemented in this firmware. The login, the Connect session, the audio keys and the Ogg Vorbis decoding are the receiver's; the player drives it and reads its PCM. Nothing about the receiver is flashed either — it lives on the microSD card.

**Status: this half is written and host-tested, but no ready-made V1 receiver binary exists yet.** The upstream project publishes builds for the Q2, and a V1 build has to be cross-compiled for `mipsel-unknown-linux-musl` soft-float (see [Building the receiver](#building-the-receiver)). Until you put such a binary on the card, **Streaming → Spotify** says the receiver is missing instead of offering a switch.

## What it looks like on the player

**Streaming → Spotify** is the switch:

- Off until you turn it on. The receiver is a separate process and a separate login, so it is not started at boot.
- Turning it on requires a Wi-Fi connection; without one the page says so and points at Wi-Fi settings.
- If the card has no receiver, the page says that instead, with the path it looked in.
- While it is on, the page shows the current track's artwork, title, artist, album and a progress bar — all read from the receiver's status file.
- The receiver's own output and the player's runtime files are all in `/tmp`, so nothing the player writes goes on the card except the receiver's cache.

The rows below the switch are the browser, over the Q2SPOT/1 socket:

| Row | Sends | Shows |
| --- | --- | --- |
| Liked songs | `liked` | Your saved tracks |
| Playlists | `playlists` | Your playlists; one opens its tracks |
| Albums | `albums` | Saved albums; one opens its tracks |
| Artists | `artists` | Followed artists; one opens its albums |
| Queue | `queue` | What the receiver will play next on the phone |
| Downloaded | `downloaded` | Complete tracks in the card cache, replayable with no network |
| Search | `search <tracks\|albums\|artists\|playlists> <term>` | The result list |
| Receiver settings | `settings`, `account` | Shuffle, repeat, account, the player's own settings |
| — | `caps` | The backend version — how the player knows the receiver is new enough |

Rows come 50 at a time. A list longer than that gets a **next page** row; the cursor the receiver answered with is sent back as the request's last argument, and **previous page** walks the other way. Back leaves a list one step at a time — from the second page to the first, from a playlist's tracks to the list that named it — and only at the top does it leave the page.

Selecting a track plays it **in its list**: `play <context> <position> <uri>`. That is what makes Next on the player continue the album rather than the single track. Selecting from the Queue sends `skip` instead, because the phone's queue is already the context.

Shuffle and repeat are the receiver's own state, shown and changed on the receiver settings page. Repeat cycles **off → the list → one track**.

### Keys, DAC and volume

While Spotify is what is playing, the player's own transport keys drive it: play/pause, next and previous go to the receiver as one-byte datagrams. The moment local playback takes the DAC, the keys go back to local playback.

The audio path is the AirPlay path, not ALSA: the receiver is started with `--backend pipe --device /tmp/sonix-spotify.pcm`, writes S16 44.1 kHz stereo into that FIFO, and the player reads it and hands it to the DAC through the same external-PCM route AirPlay uses. So the V1's volume, jack detection, DAC hand-over and gapless-within-a-source behavior are the player's, exactly as they are for any other source. **librespot is given `--volume-ctrl fixed --initial-volume 100` on purpose** so it does not put a second, invisible volume on top of the player's.

Three seconds after audio stops arriving, the player closes the DAC and hands the output back; a phone that starts casting again re-opens it. That is also what makes switching to a local file or another service work — asking for the DAC calls the sink's release path, which pauses the receiver and returns the output.

### When it stops by itself

- **Idle stop** (off by default, or 5 min / 15 min / 1 h) stops the receiver after that long with nothing playing, once it *has* played and with the page shut. The receiver's saved login makes the next start instant, and casting from the phone turns it back on.
- **Power off, reboot, factory reset and firmware update** stop the receiver and wait for it to leave, so its cache and its credential file are flushed and the card is not unmounted under a running process.
- The receiver is kept alive while the browser is on screen or a request is in flight, and Wi-Fi is kept up while a request is outstanding.
- If the receiver dies on its own, the page says so and shows the tail of its log.

## Layout on the card

```text
<card>/
  .spotify/
    librespot        the receiver, executable, mipsel soft-float
    cache/           created by the player; the saved login and cached audio
```

`librespot` must be at that exact path and executable — the page checks for it before offering the switch. `cache/` is created on the first start; it holds the credentials that make the second start not ask the phone to pair again, and (with the cache size above zero) complete tracks for the **Downloaded** list to replay offline.

Everything else is runtime state under `/tmp` and is recreated on every start:

| Path | Who writes it |
| --- | --- |
| `/tmp/sonix-spotify.state` and `.state.jpg` | the receiver: what is playing, and the artwork |
| `/tmp/sonix-spotify.sock` | the receiver: the control socket |
| `/tmp/sonix-spotify.browse.sock` | the receiver: the browse socket |
| `/tmp/sonix-spotify.pcm` | the FIFO the player reads the audio from |
| `/tmp/sonix-spotify.log` | the receiver's own output, and where the error message comes from |

A leftover socket file after a crash is normal: the receiver unlinks and re-binds them at startup.

## Receiver settings

The **Receiver settings** page keeps the player's own settings in `device_config.ini`; the rest of that page is the receiver's and is asked for over the browse socket.

| Row | What it does |
| --- | --- |
| Account | Who is signed in, or that nobody is, from the `account` op |
| Shuffle | `shuffle on` / `shuffle off`; tap to toggle |
| Repeat | `repeat off` / `context` / `track`; tap to cycle |
| Bitrate | `--bitrate 96 / 160 / 320`, default **160** kbit/s |
| Card cache | `--cache-size-limit`: off, 1 GB or 8 GB (default), of cached audio |
| Device name | `--name`: what your phone shows in its device list |
| Turn off when idle | the idle stop above: never, 5 min, 15 min or 1 h |
| Sign out | `signout`, behind a confirmation; removes the saved login, keeps downloads |

The same settings, and two the page does not show, live in `device_config.ini`:

```ini
[spotify]
bitrate = 160          ; 96, 160 or 320 kbit/s
cache_mb = 1024        ; MiB of cached audio; 0 turns --disable-audio-cache on
idle_stop = 0          ; seconds with nothing playing before stopping; 0 = never
name =                 ; empty means the player's own name from system-info.json
extra =                ; extra receiver words, verbatim, spaces splitting them
rt_priority = 10       ; the sink thread's real-time priority; 0 = no RT
debug = false          ; true adds -vv to the receiver's arguments
```

`extra` is for the receiver's other options — a proxy, normalisation, a different discovery mode — split on whitespace, at most eight words, no quoting.

## Building the receiver

The fork is built static and soft-float for the Q2, which is also a MIPS `mipsel` soft-float device, and the same instructions work for the V1:

```sh
git clone --branch shanlingq2 https://github.com/diamondBond/q2-librespot
cd q2-librespot
# contrib/shanlingq2/README: mipsel-unknown-linux-musl via -Zbuild-std,
# mipsel-linux-muslsf-cross for the linker, nan2008.py to set EF_MIPS_NAN2008.
```

Copy the resulting binary to `<card>/.spotify/librespot`, `chmod +x` it, and the page's "missing receiver" message goes away. The `contrib/shanlingq2/launcher.sh` restart loop and `aplay.sh` sink from that folder are **not** used here: the player starts the receiver itself and reads the FIFO, so the receiver is run directly rather than behind a launcher.

The receiver reports a backend version (`q2-v1.4` for this branch) over the `caps` op; the player only needs the three interfaces above to exist.

## Validation

`sonix-player/tests/test_spotify.c` covers the protocol client, the status file, the control socket, the service's start/stop/idle/ownership rules and the sink's ring and DAC hand-over, against a shell stand-in receiver and a stub audio layer; `make -C tests spotify` runs it. `sonix-player/tests/ui/test_spotify_page.c` covers the page itself on both board profiles with a stubbed `spotify.h` and shell, and records every op and argument the page sends; `make -C tests ui` runs it.

Both are host tests: they do not certify real-time scheduling, the receiver's audio quality or Wi-Fi behavior on the device. Before relying on it, check on a V1:

- the receiver starts from the card and pairs with a phone; the second start reuses the saved login;
- playback through the FIFO: volume, jack detection and switching between 3.5 mm and 4.4 mm still behave, and so do local playback, AirPlay and Bluetooth in between;
- the transport keys: play/pause, next and previous while Spotify owns playback, and that they go back to local playback after switching away;
- browsing a large playlist from the second page backwards, and a search in each of the four categories;
- the idle stop firing, and a phone casting again turning it back on;
- power-off, reboot and firmware update with the receiver running — the process should be gone and the card clean;
- CPU use and battery drain against a local-playback baseline, at each bitrate, including with the card cache on.
