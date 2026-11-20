# Bluetooth headphones (A2DP)

**Status: prototype, host-tested only.** Nothing here has run on a RetroStone2 yet; the "Measure on hardware"
list at the end is what has to be checked before this is a feature. Bluetooth controllers are in
[build.md](build.md) ("Bluetooth controllers"); this page is the audio side.

## What is in the image

| Piece | Where |
|---|---|
| BlueZ A2DP/AVRCP plugin | `BR2_PACKAGE_BLUEZ5_UTILS_PLUGINS_AUDIO=y` |
| bluez-alsa 4.3.1 (the `bluealsa` daemon, the `bluealsa` ALSA plugin, SBC) | `BR2_PACKAGE_BLUEZ_ALSA=y` |
| `bluealsa -p a2dp-source`, started and stopped with bluetoothd | `rsos-net bt on|off` |
| device kinds in the pairing screen, `rsos-bt sink` | `rsos-bt`, `frontend/src/ui/bt_ui.c` |
| the ALSA side | `frontend/src/audio/audio.c`, `host.c` |

Both option names were checked against the Buildroot 2026.02.3 sources (`package/bluez5_utils/Config.in`,
`package/bluez-alsa/Config.in`); `BR2_PACKAGE_BLUEZ_ALSA` selects the audio plugin, SBC, alsa-lib's mixer and
glib itself, the explicit plugin line only documents the dependency. Only `retrostone2_defconfig` and its release
twin changed (the other boards have no Bluetooth). AAC/aptX/LDAC are not built (no fdk-aac, libopenaptx): SBC is
what every headphone must support, and what the A20 can afford. Headset/hands-free (HFP, mono microphone audio)
is off: the `bluealsa` daemon runs with `-p a2dp-source` only.

## ALSA routing

There is no sound server: the frontend, the libretro cores (through the frontend's host process) and any SDL2
port talk to ALSA directly. bluez-alsa installs `/etc/alsa/conf.d/20-bluealsa.conf`, which defines the `bluealsa`
PCM. It plays to **the most recently connected** Bluetooth device (`DEV=00:00:00:00:00:00`) and **fails to open**
when none is connected. That failure is the routing rule:

1. `audio_open()` first tries `plug:bluealsa` (`plug` converts the 48 kHz S16 stereo the host writes to whatever
   rate the codec negotiated; 48 kHz is mandatory for an SBC sink, so normally nothing is converted).
2. If it opens, the stream is Bluetooth: the buffer is at least `latency_bt_ms` (120 ms, `audio.h`), because
   A2DP delivers in chunks.
3. If it does not (Bluetooth off, no `bluealsa` daemon, headphones not connected), the existing choice applies,
   unchanged: `audio_internal` (the codec, speaker and headphone jack) or `audio_hdmi` for a docked console
   (`board.ini`, `docs/porting.md`).

So the priority is **Bluetooth, then HDMI, then the codec**, and no setting has to be touched: connect the
headphones in Settings > Network > Bluetooth devices and the next game (or resume) plays through them. The
settings key `audio_bluetooth = false` keeps the sound on the card (headphones stay paired for other uses).
If the headphones vanish while a game runs (switched off, out of range), `snd_pcm_writei` returns `-ENODEV`;
the host then reopens on the card (`audio_lost()` in `audio_process()`) and stays there until the next open.
Headphones that connect *during* a game are picked up at the next open (game start, or return from the menu);
switching mid-game would need a periodic probe and a pacing reset, which is left out of the prototype.

Native SDL2 ports (not on `main` yet) are separate processes: the launcher should do the same, i.e. export
`SDL_AUDIODRIVER=alsa` and `AUDIODEV=plug:bluealsa` when `rsos-bt sink` exits 0, and the card's `plughw:` device
otherwise (`board_get()->audio_internal_dev`). The decision is taken once at launch: a game that is already
running keeps its device.

## Pairing screen and rsos-bt

`rsos-bt list` marks devices that BlueZ reports as an A2DP sink (UUID `110b` or an `audio-*` icon) with `audio=headphones`
in the optional fourth field, the shared extras list: comma-separated `key=value` pairs, unknown keys ignored by the
reader, three fields when a device has none. The pad battery work uses the same field (`battery=`):

```
MAC<TAB>connected|paired|found<TAB>name[<TAB>key=value[,key=value...]]
00:1B:66:00:00:07<TAB>connected<TAB>WH-1000XM3<TAB>battery=55,audio=headphones
```

The answer is cached in `/run/rsos-bt/kinds` (a `bluetoothctl info` per device and per 1.5 s refresh would be
too slow); a device whose UUIDs BlueZ does not know yet has no kind until it does. The screen shows a connected
audio device as "headphones" (with its battery, "headphones, 55%", when it reports one) instead of "connected", and failure messages say headphones. `rsos-bt sink` exits 0 when
an audio device is connected. Pair, connect, disconnect and forget are the same commands as for pads. Pairing is
Just Works; very old headphones that insist on a PIN (0000) are not supported. Headphones reconnect by themselves
when switched on, as long as they were trusted at pairing, like pads.

Tests: `board/common/tests/bt-test.sh` (stub `bluetoothctl` with headphones: kind field, `sink`, forget drops the
cache) and `make check-ui` (the screen with `frontend/tests/fake-rsos-bt.sh`: pairing headphones, the failure
message).

## Volume

The wheel is analog: it drives the codec's output, **not** Bluetooth. A2DP has its own absolute volume
(AVRCP), and bluez-alsa keeps it per device as a mixer control (`bluealsa` ctl). Until the frontend exposes it,
the headphones' own buttons are the volume control; the volume the headphones start at is whatever they
remember. Nothing in the prototype sets it. Open question for the frontend: show a volume bar in the OSD that
drives `amixer -D bluealsa` while Bluetooth is the output.

## Risks: measure on hardware

1. **Latency.** A2DP/SBC typically adds 100-250 ms. That is fine for most RPGs and strategy games and bad for
   rhythm games and twitchy platformers. The host's pacing follows the audio fill, so video stays smooth, but
   the sound is late by the pipeline delay. Measure with a click track (video flash and audio click against a
   camera) and check `audio: ... open` in the log for the real buffer; tune `latency_bt_ms`
   (120 ms is a guess: too low underruns, too high is just later).
2. **WiFi/Bluetooth coexistence.** The AP6210 shares one 2.4 GHz radio between WiFi (SDIO) and Bluetooth (UART).
   Streaming A2DP while WiFi transfers (ROM transfer, scraping, SMB) can stutter the audio or slow the
   transfer. Test A2DP during a WiFi download on a 2.4 GHz network (the AP6210 is 2.4 GHz only, so there is no 5 GHz
   way out) and look at whether the chip's coexistence settings in the firmware need attention.
3. **SBC cost on the A20.** Encoding 48 kHz stereo SBC is cheap compared with an emulator, but the A20 is
   already busy with cores in `performance` governor. Measure the `bluealsa` daemon CPU with a heavy core (N64, PSX) and
   count `underruns` in the frontend log. The `plug` resampling path should be idle (48 kHz in, 48 kHz out).
4. **Volume** (above): no wheel control, possibly very loud or very quiet at first connect.
5. **Native-game sleep/quit handling.** The frontend freezes and kills native games with process-group
   SIGSTOP/SIGCONT/SIGKILL. A stopped game's open `bluealsa` PCM stops feeding the `bluealsa` daemon, which should
   underrun quietly (keep-alive), but check that nothing clicks or latches when the game resumes, that a
   SIGKILLed game does not leave the transport acquired (the next game must open the PCM at once), and that
   the `bluealsa` daemon itself is never in the group that is signalled (it is started by `rsos-net`, outside the game's
   group).
6. **AVRCP buttons.** BlueZ turns the headphones' play/volume buttons into a uinput device. The frontend's input
   hotplug could treat that as a keyboard or pad: check `/proc/bus/input/devices` and that it does not steal a
   player slot or fire menu keys.
7. **Reconnect and boot.** Do the headphones reconnect after power-on while a game is running or at boot? Does
   bluealsa survive `rsos-net bt off`/`on` cycles (Settings toggle) and the frontend respawn? Is the ext4
   pairing store (`bluetooth.img`) mounted before bluealsa registers its endpoints?
8. **Sleep/idle power.** An idle open A2DP transport keeps the radio busy; check that closing audio on idle
   suspends the stream (bluez-alsa does so after a few seconds without data) and what Bluetooth costs in
   battery with and without headphones.
9. **dbus policy.** The `bluealsa` daemon must be allowed to own `org.bluealsa` on the system bus (bluez-alsa installs
   `/etc/dbus-1/system.d/bluealsa.conf` for root); verify it is in the image. (Checked against
   the bluez-alsa 4.3.1 sources: the daemon is `bluealsa` (`/usr/bin/bluealsa`; the `bluealsad` name is from later
   releases), `-p a2dp-source` is a valid profile argument, at least one `-p` is mandatory, and `bluealsa.conf` is
   installed in the D-Bus config directory. Not yet run.)
