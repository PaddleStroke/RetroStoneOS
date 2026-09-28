RETROSTONE - your games, BIOS files and saves
=============================================

This drive is the data partition of the RetroStone2 SD card. You can copy
files here from any computer (Windows, macOS, Linux) while the card is in
a card reader, then put the card back in the console.

Where things go
---------------

  roms\<system>\     your games, one folder per system:
      nes  fds  snes  n64  gb  gbc  gba  sg1000  mastersystem  megadrive
      segacd  sega32x  gamegear  pico  psx  pcengine  atari2600
      arcade   (MAME 2003-Plus romsets, .zip)
      fbneo    (FinalBurn Neo romsets, .zip)
      neogeo   (FinalBurn Neo romsets + neogeo.zip)
    Sub-folders are fine (for example one folder per multi-disc game).
    Zipped cartridge games (.zip) work. Disc games: .cue + .bin, .chd, .pbp,
    .m3u for multi-disc.

  bios\              BIOS files, for example:
      scph5501.bin (PlayStation, optional)    gba_bios.bin (optional)
      disksys.rom (Famicom Disk System)       bios_CD_U.bin / _E / _J (Mega-CD)
    Arcade BIOS zips (neogeo.zip...) go next to the games, not here.

  saves\<system>\    in-game saves (.srm), written by the console
  states\<system>\   save states, written by the console
  screenshots\       screenshots
  themes\            extra themes
  rsos\              console settings (settings.ini, wpa_supplicant.conf)

Folder names from RetroPie and other handhelds are recognised when you
import from a USB stick (genesis -> megadrive, sfc -> snes, ...), but on
this card please use the names above.

Other ways to add games
-----------------------

  * USB stick: plug it into one of the console's USB ports and choose
    "Import from USB". The stick is only read, never written.
  * Network: turn WiFi or Ethernet on, open "Transfer over network" and
    type the address shown on screen into a browser on your phone or PC.

Please note
-----------

  * Never connect the console to a PC with a USB A-to-A cable: its USB-A
    ports are hosts that always supply 5 V, and its micro-USB port is for
    power only. Use the SD card, a USB stick or the network instead.
  * If Windows offers to format another drive of this card, say NO: that
    is the console's system partition (Windows cannot read it).
  * Eject the card safely before removing it from the PC.
  * No games or BIOS files are included. Only use files you are allowed to.
