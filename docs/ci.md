# Continuous integration and releases

RetroStoneOS is built and tested by GitHub Actions in the public repository
[PaddleStroke/RetroStoneOS](https://github.com/PaddleStroke/RetroStoneOS). Two workflows:

| Workflow | Runs on | What it does | Time |
|---|---|---|---|
| `.github/workflows/ci.yml` | every push, every pull request, by hand | frontend tests (plain and ASan/UBSan), the frontend cross-compiled for 32- and 64-bit ARM, board script tests, defconfig check, linters | ~10-15 min (jobs run in parallel) |
| `.github/workflows/images.yml` | a version tag `v*`; by hand ("Run workflow") | the SD card images of every board; on a tag, a GitHub Release with the images, `SHA256SUMS`, the legal-info archives and release notes | 1-3 h per board, boards in parallel |

The workflows only call the scripts of `scripts/ci/`, which also run in WSL: what CI does can be reproduced locally
(see "Running the CI scripts locally").

Public repositories get GitHub-hosted runners for free, without a minute limit (ubuntu-24.04: 4 vCPU, 16 GB RAM,
4 GB of swap, about 14 GB free disk before `free-disk.sh`, 6 h per job, 20 concurrent jobs, 10 GB of caches per
repository).

## 1. `ci.yml`: the checks

| Job | Gate? | Details |
|---|---|---|
| frontend make check / check-asan | yes | `make -C frontend check` and `check-asan` (host build, Ubuntu's libdrm and alsa-lib, and libzstd, mbedTLS, `zstd` and `signify-openbsd` for the system updater's `check-update`). For ASan the job sets `vm.mmap_rnd_bits=28` (the Ubuntu 24.04 kernel's 32-bit mmap randomization breaks the GCC 13 sanitizer runtime). |
| frontend cross-compile | yes | `scripts/ci/frontend-cross.sh`: the whole `make all` for `arm-linux-gnueabihf` (Cortex-A7, 64-bit `off_t`/`time_t` as Buildroot builds it) and `aarch64-linux-gnu` (Cortex-A72), linked against the armhf/arm64 libdrm and alsa-lib from ports.ubuntu.com (multiarch), plus the modules' own ARM checks (`ui-arm-check`, `transfer-arm-check`, `power-arm-check`). Compiler warnings are reported, not fatal. |
| board script tests | yes | `scripts/ci/run-tests.sh host`: every `buildroot-external/board/common/tests/*.sh` (board.sh, rsos-boot-ok with the U-Boot 2026.07 host tools built from the checked tarball, the release defconfig check). |
| defconfigs and packages | yes, except check-package | `scripts/ci/check-defconfigs.sh`: every `configs/*_defconfig` goes through `make <board>_defconfig` without kconfig dropping a line (a symbol whose dependencies are not met disappears silently otherwise), and the CI homebrew switch works on it. `scripts/ci/check-package.sh`: Buildroot's `utils/check-package` on the external tree, **report only** (about 55 style findings when this was written, mostly Config.in help-text width and the `.hash` format of rsos-homebrew). |
| lint | yes, except shellcheck | `scripts/ci/check-modes.sh`: scripts are executable in git (see "Executable bits"). `scripts/ci/actionlint.sh`: the workflows (actionlint 1.7.12, sha256-checked, with shellcheck on the `run:` blocks). `scripts/ci/shellcheck.sh`: shellcheck on `board/**/*.sh`, the scripts of the rootfs overlays and packages, and `scripts/ci/`: **report only** for the existing scripts (61 findings in 34 scripts when this was written: 5 errors, 15 warnings, 41 notes; the most in `bootlog` and `rcK`), but `scripts/ci/` itself must stay clean. Each finding is an annotation on the commit; the step summary has the count per file. |

To make shellcheck or check-package a gate once the findings are fixed, add `--strict` to their line in `ci.yml`.

A push to a branch cancels the previous run of the same branch or pull request; runs on the default branch are
never cancelled.

## 2. `images.yml`: the images

### Which boards

`scripts/ci/list-boards.sh` reads `buildroot-external/configs/*_defconfig`, so a new port is built as soon as its
defconfig is committed. `scripts/ci/board-info.sh <board>` shows what CI derives from a defconfig:

| | Rule |
|---|---|
| Release set (tags) | every defconfig, except those with a line `# ci: skip`, and except a development variant: `<x>_defconfig` when `<x>_release_defconfig` exists (`retrostone2_defconfig` is only built by hand; `retrostone2_release_defconfig` is the RetroStone2 release image) |
| Image name | `retrostoneos-<version>-<image>.img.xz`, `<image>` = the defconfig name without `_defconfig` and `_release`, `_` -> `-` (`retrostone2`, `rpi4-64`), plus `-dev` for a development variant |
| Release or development build | on a tag, `<x>_release_defconfig` and every board without a release defconfig (`VARIANT=default`: the Raspberry Pi and Orange Pi boards) are **release builds** (`BR2_RETROSTONE_RELEASE=y`, set by `build-board.sh configure` for the latter), reporting the tag's version (`0.1.0`); a development variant stays one (`0.1.0-dev`). Manual runs are release builds only for `<x>_release_defconfig`, and report the package version (`RSOS_FRONTEND_VERSION`) |
| Board name (release notes) | a line `# ci: name=<text>` in the defconfig, else `NAMES` in `scripts/ci/board-info.sh` (the two Orange Pi H3 images, which share one board folder: "Orange Pi PC / PC Plus", "Orange Pi One / Lite"), else `name =` of the board's `rootfs-overlay/etc/rsos/board.ini` |
| Status (release notes) | "tested on hardware" for the boards listed in `HW_TESTED` in `scripts/ci/board-info.sh` (the RetroStone2), "community-tested" for the others; a line `# ci: status=<text>` in the defconfig overrides it |
| Heavy | `HEAVY_BOARDS` in `board-info.sh` (the Orange Pi 5) or a line `# ci: heavy`: a cold build longer than the build step's 300 minutes on a standard runner (section 6). The job runs on `IMAGES_RUNNER_HEAVY` when set, always keeps its ccache, and is `continue-on-error`: if it fails, the release goes out without that board (the release job warns) |

### A build job, step by step

1. `free-disk.sh`: removes the runner's Android SDK, .NET, Haskell, Swift, CodeQL, browsers and Docker images
   (about 30 GB) and picks the work directory (`/mnt` when it has more room). It keeps the runner's swap (and adds a
   4 GiB swapfile on the work disk if there is less), for the LLVM links of the Orange Pi 5. A board needs about
   20-25 GB: 11-13 GB of output, 3 GB of downloads, 2.5 GB of ccache, the legal-info copy and the compressed files;
   the Orange Pi 5 about 35 GB (26 GB of output).
2. `install-deps.sh buildroot tests`: the Buildroot host packages of docs/build.md, plus what the image tests need.
3. **Caches** (`actions/cache`), sized for GitHub's 10 GB per repository (9 boards x 2.5 GB of ccache would not fit,
   and entries unused for 7 days are evicted anyway, so they rarely survive from one release to the next):
   - one **download cache** for all boards (`BR2_DL_DIR`, key: the hash of the defconfigs, the packages'
     `.mk`/`.hash` files, the patches' `.hash` files and `buildroot.env`; a miss falls back to the newest one), about
     3 GB, saved only by the RetroStone2 build; the other boards start from it and download the rest;
   - a **ccache** per board (`BR2_CCACHE_DIR`, a new key per run, restored from the board's newest; capped at
     2.5 GB, 4 GB for a heavy board), saved only when the build failed or hit its time limit (so that "Re-run failed
     jobs" goes further), and always for a heavy board.

   Caches are scoped by ref: a tag's run reads those of the default branch, so a manual run on `main` before
   tagging (section 7) warms the download cache and the heavy boards' ccache.
4. Homebrew games: see section 4.
5. `build-board.sh <board> configure`: downloads Buildroot 2026.02.3 and checks its sha256 (`scripts/ci/buildroot.env`,
   the value of docs/build.md), `make <board>_defconfig`, sets `BR2_PACKAGE_RSOS_HOMEBREW` in `.config` (never in the
   defconfig) and, on a tag, `BR2_RETROSTONE_VERSION` (and `BR2_RETROSTONE_RELEASE=y` for a board without a release
   defconfig, see "Which boards"), then `make olddefconfig`. The log line says `release build` or
   `development build`.
6. `source`: `make source` (3 tries; every download is checked against the `.hash` files,
   `BR2_DOWNLOAD_FORCE_CHECK_HASHES`).
7. `build`: `make`; the log shows the `>>>` lines with timestamps, the full log is uploaded as `build-log-<board>`
   when the job fails. The step stops after 300 minutes (see "Build times"); the job after 360 (GitHub's limit for
   hosted runners), which leaves about an hour for the rest. Every other step has its own time limit.
8. `package`: `xz -T0 -9` of `images/sdcard.img` (the 1.16 GB RetroStone2 image becomes about 45 MB), its
   `.sha256`, and a `.info` file for the release notes. For a board with A/B slots (its image has
   `/etc/fw_env.config`) also the **system update package** `retrostoneos-<version>-<image>.rsu` and its `.sha256`
   (`scripts/ci/make-rsu.sh`, docs/updates.md §5: `zstd -19` of `images/rootfs.ext4`, the manifest, a changelog
   excerpt, and the signature). **Only tag builds are signed**: their job uses the `release` environment, whose
   `UPDATE_SIGNING_KEY` secret must match `frontend/assets/update.pub`, and sets `RSOS_RSU_REQUIRE_SIGNED=1`, so the
   step fails rather than make an unsigned package. Manual runs get no key and make `<...>-unsigned.rsu`, which
   release images refuse and the consoles' online check ignores. On a tag, a treeless `git fetch --unshallow` first
   gives the changelog its history, and the image reports the tag's version (`RSOS_CI_OS_VERSION` ->
   `BR2_RETROSTONE_VERSION`), which `make-rsu.sh` checks (plus `-dev` for a development variant). Uploaded as the
   artifact `image-<board>`.
9. `legal-info` (tags, or by hand): `make legal-info` -> `legal-info-<image>-<version>.tar.xz` (split into
   1900 MiB parts if it exceeds GitHub's 2 GiB per file; the RetroStone2 one was 1.53 GiB when this was written,
   mostly already-compressed source tarballs such as linux-firmware and the kernel). The permission-only homebrew
   ROMs are never in it (`RSOS_HOMEBREW_REDISTRIBUTE = NO`), and the NXEngine source is saved without the Cave Story
   game data of its tarball (`libretro-nxengine.mk`: `REDISTRIBUTE = NO` and a hook that saves the tree without
   `datafiles/`).
10. Image tests (not a gate yet; last, so that a slow test cannot cost the release files their time):
    `run-tests.sh image <board> <output>` runs the board/common tests and the board's own `tests/*.sh` against the
    build, from a fake `$HOME` whose `rsos/output` is the CI output: for the RetroStone2 `data-partition-test.sh` (as
    root: loop devices and a private mount namespace; `modprobe exfat` first) and `boot-ab-qemu-test.sh` (U-Boot for
    QEMU's cubieboard). They are `continue-on-error` until they have proved stable on the runners: check their
    result in the log.

### The release job (tags only)

Once every board succeeded (a heavy board may have failed: it is then left out, with a warning), it runs
`free-disk.sh` (the legal-info archives of all the boards are about 15 GB) and downloads the artifacts (images,
`.rsu` update packages, legal-info) to the work directory, refuses any `*-unsigned.rsu`, adds
`ucity-1.2-source.tar.gz` (the GPL source of the
bundled µCity, sha256-checked) when an image contains the homebrew games, writes `SHA256SUMS`, renders
`.github/release-notes.md` (`scripts/ci/release-notes.sh`: the boards table with sizes and status, flashing with
balenaEtcher/Rufus/Raspberry Pi Imager, first-boot notes, licences, the source offer, and the commits since the
previous `v*` tag), creates the release as a **draft**, uploads the files, then publishes it. A tag with `-rc`,
`-beta` or `-alpha` gives a pre-release; any other tag is marked "latest". It is the only job with
`contents: write`.

If one board fails, there is no release: fix it and re-run the failed jobs ("Re-run failed jobs" keeps the
successful boards), or delete the tag and tag again. A release that is already published is never modified (the job
stops with an error); a leftover draft is completed.

## 3. Making a release

1. Check that `ci.yml` is green on the commit, and that the owner checklist (section 7) is done: in particular the
   `UPDATE_SIGNING_KEY` secret of the `release` environment (without it every A/B board's job fails at "Compress the
   image, make the update package", and there is no release). The images report the tag's version
   (`BR2_RETROSTONE_VERSION`).
2. Tag and push (the tag name is the version: `v0.1.0`, `v0.2.0-rc1`):
   ```sh
   git tag -a v0.1.0 -m "RetroStoneOS 0.1.0"
   git push origin v0.1.0
   ```
3. Actions > Images: one job per board (if the `release` environment has a required reviewer, approve the waiting
   jobs: "Review deployments"), then "GitHub Release". The release appears under Releases when every board is done.
4. Edit the notes on GitHub if needed (text only: the files cannot change once published when release immutability
   is on).
5. **After tagging**, raise `RSOS_FRONTEND_VERSION` in `buildroot-external/package/rsos-frontend/rsos-frontend.mk`
   above the release (`0.1` -> `0.1.1` after `v0.1.0`, or the next planned version) and commit: development
   builds report `<RSOS_FRONTEND_VERSION>-dev`, and must sort after the release they follow.

## 4. Homebrew games: the `HOMEBREW_TOKEN` secret

The owner-approved homebrew games (docs/homebrew.md) must not be in the public repository. The images get them
from a **private** repository at build time. Owner decision (2026-09-27): **every board's image** includes them when
the secret is available; without it, every image is built with `BR2_PACKAGE_RSOS_HOMEBREW=n` (the release notes say
so, and `roms/` starts empty). The seeding at the first boot is in `board/common` (`data-partition` runs
`rsos-seed-homebrew`), so it works on every board.

The authors' permissions cover the RetroStoneOS images of every board (docs/homebrew.md), not the ROMs on their own:
that is why they live in a private repository.

One-time set-up (as the owner, on github.com):

1. **Create the private repository**: "New repository", owner `PaddleStroke`, name `RetroStoneOS-homebrew`,
   **Private**, no README.
2. **Upload the games**: the *contents* of the local `homebrew/license ok/` folder go to the root of that repository
   (the ROMs, covers, `ReadMe.txt`, `Espionage.zip`, ...). With git (Git Bash, in `homebrew/license ok/`):
   ```sh
   git init -b main && git add -A && git commit -m "Approved homebrew set"
   git remote add origin https://github.com/PaddleStroke/RetroStoneOS-homebrew.git
   git push -u origin main
   ```
   (or "Add file > Upload files" on the website). A copy of the whole `homebrew/` folder also works: CI takes its
   `license ok/` subfolder. Every file of `package/rsos-homebrew/rsos-homebrew.hash` must be there, unchanged; the
   build checks them.
3. **Create a fine-grained token**: your avatar > Settings > Developer settings > Personal access tokens >
   Fine-grained tokens > "Generate new token":
   - Token name `RetroStoneOS CI homebrew`, an expiration (e.g. 1 year; put a reminder in your calendar);
   - Resource owner `PaddleStroke`; Repository access: **Only select repositories** > `RetroStoneOS-homebrew`;
   - Permissions > Repository permissions > **Contents: Read-only** (Metadata: read-only is added automatically).
     Nothing else.
   - "Generate token" and copy it (it is shown once).
4. **Add it as a secret of the public repository**: `PaddleStroke/RetroStoneOS` > Settings > Secrets and variables >
   Actions > "New repository secret": name `HOMEBREW_TOKEN`, value the token.
5. Check: Actions > Images > "Run workflow" with `boards` = `retrostone2_release`: the "Stage the homebrew games" step
   prints the number of files, and the job summary says `homebrew games: yes`.

When the token expires, the next build fails at "Fetch the homebrew games": generate a new one (step 3) and update
the secret (step 4). To build without the games for a while, delete the secret.

Secrets are never given to workflows started by pull requests from forks, and `images.yml` does not run on pull
requests at all; the token can only read that one repository.

## 5. Building by hand (workflow_dispatch)

Actions > Images > "Run workflow", pick the branch, then:

- `boards`: `release` (the release set), `all` (every defconfig, development variants included), or names separated by
  spaces or commas, with or without `_defconfig`: `retrostone2`, `retrostone2_release rpi4_64`;
- `legal_info`: also make the legal-info archives (off by default: about 10 more minutes per board);
- `image_tests`: run the image tests (on by default).

The files are in the run's page, "Artifacts": `image-<board>` (a zip holding the `.img.xz`, its `.sha256` and the
`.info`), kept 30 days (7 days for tag builds, whose files are in the release). Their version is
`<branch>-<commit>`, e.g. `retrostoneos-main-1a2b3c4d-retrostone2-dev.img.xz`. No release is made, and nothing is
signed: manual runs never get the signing key (their update packages are `*-unsigned.rsu`, which only development
images install), so a branch build can never be installed on a console running a release.

## 6. Build times and costs

Measured locally (16 cores, WSL): a full RetroStone2 build takes about 20 min with a warm ccache and about 30 min
cold (downloads included); the Raspberry Pi 4 about 29 min (its kernel alone is 8 min). In those builds 60-75 % of
the time is compiling (it scales with the cores) and the rest is configure scripts, extraction and installation
(mostly one core). With 36 cores now (ScummVM, DOSBox Pure, VICE and FBNeo are the heavy ones), expect on the 4-vCPU
runner:

| Board | Cold (no ccache) | Warm ccache | + legal-info, tests, compression, uploads |
|---|---|---|---|
| RetroStone2 (release or dev) | 1.5-2.5 h | 45-75 min | +15-25 min |
| Raspberry Pi 4/5, 64-bit boards (big vendor kernel) | 2-3 h | 1-1.5 h | +15-25 min |
| Other 32-bit boards | 1.5-2.5 h | 45-75 min | +15-25 min |
| **Orange Pi 5 (heavy)** | **over 5 h** (86 min locally on 16 cores, 39 of them LLVM and Clang) | 2-3 h | +20-30 min |

These are estimates: the first tag gives the real numbers (each job's log prints the build time; the ccache
statistics follow). Boards build in parallel, so a release takes as long as the slowest board.

**The Orange Pi 5.** Mesa's panfrost driver cannot be built without LLVM in Buildroot 2026.02 (docs/boards.md), so
its cold build does not fit a standard runner. It is a **heavy** board (section 2): with the repository variable
`IMAGES_RUNNER_HEAVY` set to a larger or self-hosted runner it builds there; otherwise it tries on the standard
runner, keeps its ccache (4 GB) even when it succeeds, and a failure does not block the release, which goes out
without it (the release job warns). A manual run on `main` before the tag (section 7) warms its ccache, so the
tag's run usually fits.

**The 6 h limit.** The build step stops after 300 minutes and the job after 360. Plan, in order:

1. **Re-run the failed job.** The ccache is saved even after a timeout, so the second run compiles far less.
2. `RSOS_CI_PER_PACKAGE=1` (an `env:` of the build job): `BR2_PER_PACKAGE_DIRECTORIES=y` and a top-level
   `make -j<cores>`, so that packages build side by side (the single-threaded configure steps overlap). Test it by
   hand first: every package of the external tree must be per-package-safe.
3. **A larger runner**: set the repository variable `IMAGES_RUNNER` (Settings > Secrets and variables > Actions >
   Variables) to a larger runner's label (Team/Enterprise plans, billed per minute), or to `self-hosted` for a
   self-hosted runner (e.g. the 16-core WSL build host: Settings > Actions > Runners > "New self-hosted runner";
   `free-disk.sh` is skipped there); `IMAGES_RUNNER_HEAVY` does the same for the heavy boards only. A public
   repository should only use a self-hosted runner for these trusted triggers (tags and manual runs), which is the
   case: `images.yml` never runs on pull requests.
4. Last resort: build the heavy cores in a separate job and hand them over (not implemented: it needs the cores
   installed from a prebuilt archive).

**Cost**: nothing for a public repository (minutes and artifact storage are free). The caches count towards the
10 GB per repository (older entries are evicted first); a larger runner or more cache storage is paid.

## 7. Owner checklist (before the first tag)

On github.com, repository `PaddleStroke/RetroStoneOS`:

1. **Actions settings** (Settings > Actions > General): "Actions permissions": allow actions created by GitHub (the
   workflows only use `actions/checkout`, `cache`, `upload-artifact`, `download-artifact`, pinned by commit);
   "Workflow permissions": **Read repository contents** (the release job asks for `contents: write` itself); "Fork
   pull request workflows": keep "Require approval for first-time contributors".
2. **The signing key in a `release` environment** (Settings > Environments > "New environment", name `release`):
   - "Deployment branches and tags": **Selected branches and tags** > add a **tag** rule `v*` (only tag builds can
     then use it);
   - "Environment secrets" > `UPDATE_SIGNING_KEY` = the whole content (both lines) of
     `RetroStoneOS-keys\update-signing.key` on the owner's PC (docs/updates.md §3; keep an offline backup). Its
     public half must be `frontend/assets/update.pub`: `rsos-mkupdate pubkey -s update-signing.key` prints it, and
     its last line must equal the last line of `update.pub`;
   - optional: "Required reviewers" = yourself (each tag build then waits for your approval);
   - delete any repository-level `UPDATE_SIGNING_KEY` secret (Settings > Secrets and variables > Actions), so that
     only the environment holds the key.
3. **Homebrew games** (section 4): the private repository `PaddleStroke/RetroStoneOS-homebrew`, a fine-grained PAT
   (that repository only, Contents: read-only) and the repository secret `HOMEBREW_TOKEN`.
4. **Release immutability** (Settings > General > Releases > "Enable release immutability"): a published release's
   files and tag can no longer change, so a download always matches `SHA256SUMS` (the workflow publishes through a
   draft, as immutability requires).
5. **A `v*` tag ruleset** (Settings > Rules > Rulesets > New tag ruleset, target `v*`): restrict creation, update
   and deletion of release tags to the maintainers.
6. **A dry run**: Actions > Images > "Run workflow" on `main` with `boards` = `release` and `legal_info` = on. Every
   board should build (the Orange Pi 5 may time out: section 6); the update packages are `*-unsigned.rsu` (manual
   runs are never signed). Flash the `image-retrostone2_release` artifact and test it on the console (first boot,
   menu, a game, an unsigned `.rsu` is refused).
7. **After tagging**: raise `RSOS_FRONTEND_VERSION` (`buildroot-external/package/rsos-frontend/rsos-frontend.mk`)
   above the release, so that later development builds sort after it (section 3).

Optional: the secret `UART_PASSWORD` (only if a defconfig selects `BR2_RETROSTONE_UART_SHELL_PASSWORD`; the build
stops without it); the variables `IMAGES_RUNNER` and `IMAGES_RUNNER_HEAVY` (section 6); branch protection of `main`
(require the `CI` checks); a `.github/dependabot.yml` (`github-actions` ecosystem) for the pinned actions.

## 8. Executable bits

Buildroot runs the post-build and post-image scripts directly, and the rootfs overlays copy file modes into the
image. A commit made from Windows (`core.fileMode=false`) records new files as `100644`, which only breaks on Linux
(the CI build or an init script). `check-modes.sh` fails the lint job for any script with a `#!` line that is not
`100755` in git. To fix the index before committing (Git Bash):

```sh
git update-index --chmod=+x $(sh scripts/ci/check-modes.sh --list)
```

(`git add --chmod=+x <file>` for a new file.)

## 9. Running the CI scripts locally (WSL)

All scripts are POSIX `sh`, take a work directory in `RSOS_CI_WORK` (default `~/rsos-ci`, never `~/rsos/output`)
and reset `PATH` themselves (the Windows `PATH` must not leak into Buildroot):

```sh
cd /mnt/c/path/to/RetroStoneOS
export RSOS_CI_WORK=~/rsos/ci-test BR2_DL_DIR=~/rsos/dl     # reuse the local download cache
sh scripts/ci/list-boards.sh                     # the release set, as JSON
sh scripts/ci/board-info.sh rpi4_64
sh scripts/ci/check-defconfigs.sh                # ~10 s
sh scripts/ci/run-tests.sh host
sh scripts/ci/shellcheck.sh                      # apt install shellcheck
sh scripts/ci/check-package.sh                   # apt install python3-magic python3-flake8
sh scripts/ci/build-board.sh retrostone2_release configure source   # or: all
sh scripts/ci/run-tests.sh image retrostone2 ~/rsos/output          # after a build
H=~/rsos/output/host/bin CROSS_CC=$H/arm-linux-gcc CROSS_AR=$H/arm-linux-ar CROSS_PKG_CONFIG=$H/pkg-config \
    sh scripts/ci/frontend-cross.sh arm-linux-gnueabihf             # with the Buildroot toolchain
```

`build-board.sh` builds in `$RSOS_CI_WORK/output-<board>` and writes the files to publish to
`$RSOS_CI_WORK/artifacts`; `RSOS_CI_HOMEBREW=auto` (the default) builds the games in when `homebrew/license ok/`
exists, as it does on the build host.

## 10. Troubleshooting

| Symptom | Cause, fix |
|---|---|
| "Buildroot tarball ... does not match scripts/ci/buildroot.env" | the download is corrupt or the release was changed upstream: re-run; when moving to a new Buildroot release, update `buildroot.env` (version and sha256, checked against the signed `.sign` file) |
| `make source` fails 3 times | an upstream site is down (GitHub archive, kernel.org, sourceforge): re-run later. A hash mismatch means the upstream file changed: check it and update the package's `.hash` |
| "RSOS_CI_HOMEBREW=yes but ... is empty" / "stage-homebrew: missing <file>" | the homebrew repository lacks a file of `rsos-homebrew.hash` or has it in another folder (section 4, step 2) |
| "Fetch the homebrew games": "repository not found" / 401 | the token expired, or it does not include `RetroStoneOS-homebrew`, or lacks Contents: read (section 4, step 3) |
| rsos-homebrew: `sha256sum: WARNING ... did NOT match` | a ROM in the private repository differs from the approved one: restore the approved file |
| "selects a UART password login: set the UART_PASSWORD secret" | the defconfig uses `BR2_RETROSTONE_UART_SHELL_PASSWORD`: add the secret (no `"`, `\` or `$` in it) |
| "Permission denied" running `post-build.sh` or `post-image.sh` | the executable bit is missing in git: section 8 |
| No space left on device | a board outgrew the runner: check the "Free disk space" output; drop `legal_info` for manual runs, or use a larger runner |
| The build step stops at 300 min | section 6 |
| The release job says the release is already published | a published release is never overwritten: delete it (and the tag) on GitHub, or tag a new version |
| check-defconfigs: "lines of X_defconfig not in the resulting .config" | kconfig dropped these symbols: a dependency is missing (e.g. a core not available for the architecture) or the option was renamed; fix the defconfig |
| frontend-cross: "libdrm/alsa for ... not found" | the multiarch set-up of `install-deps.sh cross` failed (ports.ubuntu.com unreachable): re-run |
| check-asan: "AddressSanitizer:DEADLYSIGNAL" at start-up | the `vm.mmap_rnd_bits` step did not run (only the ASan job sets it) |
| Image tests fail (data partition) | often the runner kernel: no exFAT module, or no free loop device. They are not a gate; run `data-partition-test.sh` in WSL to confirm |
| make-rsu: "UPDATE_SIGNING_KEY does not match the public key in the image" | the secret holds another key than `frontend/assets/update.pub`: put the right key in the secret (docs/updates.md §3), never change `update.pub` without a key rotation plan |
| make-rsu: "the image reports version X, the tag says Y" | the tag is not a plain version (`v0.2.0`, `v0.2.0-rc1`), or `BR2_RETROSTONE_VERSION` / `BR2_RETROSTONE_RELEASE` did not reach the image: check the "configure" step's `release build` line |
| make-rsu: "UPDATE_SIGNING_KEY is not set: no unsigned package for a release" (tag builds) | the `release` environment has no `UPDATE_SIGNING_KEY` secret: add it (section 7), then "Re-run failed jobs" |
| A tag build waits, or fails with "Tag ... is not allowed to deploy to release" | the `release` environment has a required reviewer (approve: "Review deployments"), or its tag rule does not match the tag (it must be `v*`) |
| "<image> did not build (a heavy board)" in the release job | the Orange Pi 5 hit the build step's limit: the release went out without it. Build it by hand (`boards` = `orangepi5`, which re-uses its ccache) and set `IMAGES_RUNNER_HEAVY` for the next release (section 6) |
| The release job: "unsigned update packages in a release" | a build job made a `*-unsigned.rsu` on a tag, which `RSOS_RSU_REQUIRE_SIGNED=1` should prevent: check the package step's environment in `images.yml` |
