# SDL2 patch (RetroStone2)

`0001-kmsdrm-output-policy.patch` changes SDL 2.32.10's KMS/DRM video driver (`src/video/kmsdrm`) for the native
Ports: it opens only the output RetroStoneOS would use (HDMI when plugged in, else the built-in panel), renders
at a fixed size and scales it in the display engine, and follows HDMI being plugged or unplugged, the sound
included (the ALSA backend plays on the HDMI sound card or the speaker). See
`docs/display-design.md`, section 9b.

SDL is Copyright (C) 1997-2025 Sam Lantinga and is under the **zlib license**. This patch alters that source and
is marked as such; the changed files (`SDL_kmsdrmvideo.c`, `SDL_kmsdrmvideo.h`, `SDL_kmsdrmopengles.c`,
`SDL_kmsdrmsym.h`, and the ALSA backend `SDL_alsa_audio.c` and `.h`) and the files it adds (`rsos_scanout.c`, `policy.c`, `policy.h`) carry RetroStoneOS comments.
The patched library is the one in the image, so the legal-info archive of a build has the SDL2 license text and
this patch. Buildroot applies every `*.patch` of this folder to the `sdl2` package.
