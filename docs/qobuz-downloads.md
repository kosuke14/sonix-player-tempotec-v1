# Permanent Qobuz downloads

Use only for content you are authorized to save; application keys and a paid
subscription alone do not establish permission under Qobuz's terms.

## Setup and use

1. Put your own `[qobuz]` `app_id` and `app_secret` in
   `streaming-keys.ini` at the microSD root (see the example in sonix-player).
   Restart, connect to Wi-Fi, then sign in under Streaming → Qobuz.
2. Select the quality in Qobuz's gear/settings page: MP3 320, CD FLAC,
   FLAC up to 24/96, or FLAC up to 24/192. This is shared with playback;
   a download snapshots the selection when started. Actual resolution depends
   on the source and account.
3. Search for tracks, albums or artists. Open an album to see its tracks.
4. Open a track's ellipsis menu and choose **Download to music folder**.
5. Files are saved under **Music/Qobuz** on the microSD. Rescan the local music
   library afterward, or browse the folder directly.

The files are independent of the playback cache and are not removed by cache
cleanup, logout, or reboot. Filenames include artist, title, track ID and
requested quality. Existing files are not overwritten. Audio is saved as served,
without transcoding or tag rewriting; embedded metadata may be absent.

## Lightweight first version / limitations

- One track at a time, no album batch queue and no automatic retry/resume.
- Fixed 16 KiB transfer buffer and a 256 KiB worker stack; no whole-track RAM
  buffering, new runtime dependency, artwork download or tag-rewriting pass.
- The worker shares the existing device-wide transfer turn with streaming.
  It may wait behind a playback download and may delay subsequent prefetches;
  downloading while playing high-resolution music is not recommended.
- A track menu shows **Cancel download** while a transfer is pending or active.
  Cancellation is cooperative: a network operation can take up to its timeout;
  waiting for the shared transfer turn or DNS resolution can take longer.
- Completion/error notifications, but no byte-progress display in this version.
- The `.part` file is removed on handled failure/cancellation. Power loss can
  leave a `.part`; delete it manually before retrying the same track/quality.
- Successful transfers are synced and renamed only after a nonempty complete
  HTTP body. HTTPS downgrade is prohibited. No access restrictions are bypassed.
- Device battery, speed and memory usage have not been measured on hardware.

## Host regression test (mock transport, no credentials)

From the repository root:

```sh
cc -std=gnu11 -Wall -Wextra -Werror -Isonix-player \
  sonix-player/tests/test_qobuzdownload.c \
  sonix-player/src/system/streaming/qobuzdownload.c \
  -pthread -o /tmp/test_qobuzdownload
/tmp/test_qobuzdownload
```

Checks successful output, filename sanitization, existing-file refusal and
truncated-body cleanup. This is not a live Qobuz integration or device test.
