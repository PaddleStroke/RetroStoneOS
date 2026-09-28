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
about 14 GB free disk before `free-disk.sh`, 6 h per job, 20 concurrent jobs, 10 GB of caches per repository).

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
| Board name (release notes) | `name =` of the board's `rootfs-overlay/etc/rsos/board.ini` |
| Status (release notes) | "tested on hardware" for the boards listed in `HW_TESTED` in `scripts/ci/board-info.sh` (the RetroStone2), "community-tested" for the others; a line `# ci: status=<text>` in the defconfig overrides it |

### A build job, step by step

1. `free-disk.sh`: removes the runner's Android SDK, .NET, Haskell, Swift, CodeQL, browsers and Docker images
   (about 30 GB) and picks the work directory (`/mnt` when it has more room). A board needs about 20-25 GB: 11 GB of
   output, 3 GB of downloads, 2 GB of ccache, the legal-info copy and the compressed files.
2. `install-deps.sh buildroot tests`: the Buildroot host packages of docs/build.md, plus what the image tests need.
3. Caches (`actions/cache`): the download directory (`BR2_DL_DIR`, key: the hash of the defconfigs, the packages'
   `.mk`/`.hash` files and `buildroot.env`; a miss falls back to the newest cache of the board, then of any board) and
   the ccache (`BR2_CCACHE_DIR`, a new key per run, restored from the board's newest; capped at 2.5 GB). The ccache
   is saved even when the build fails or hits its time limit, so a re-run goes further.
4. Homebrew games: see section 4.
5. `build-board.sh <board> configure`: downloads Buildroot 2026.02.3 and checks its sha256 (`scripts/ci/buildroot.env`,
   the value of docs/build.md), `make <board>_defconfig`, sets `BR2_PACKAGE_RSOS_HOMEBREW` in `.config` (never in the
   defconfig), then `make olddefconfig`.
6. `source`: `make source` (3 tries; every download is checked against the `.hash` files,
   `BR2_DOWNLOAD_FORCE_CHECK_HASHES`).
7. `build`: `make`; the log shows the `>>>` lines with timestamps, the full log is uploaded as `build-log-<board>`
   when the job fails. The step stops after 300 minutes (see "Build times").
8. `package`: `xz -T0 -9` of `images/sdcard.img` (the 900 MiB RetroStone2 image becomes about 45 MB), its
   `.sha256`, and a `.info` file for the release notes. For a board with A/B slots (its image has
   `/etc/fw_env.config`) also the **system update package** `retrostoneos-<version>-<image>.rsu` and its `.sha256`
   (`scripts/ci/make-rsu.sh`, docs/updates.md §5: `zstd -19` of `images/rootfs.ext4`, the manifest, a changelog
   excerpt, signed with the `UPDATE_SIGNING_KEY` secret, which must match `frontend/assets/update.pub`; without the
   secret the package is `<...>-unsigned.rsu`, which release images refuse and the consoles' online check ignores).
   On a tag, a treeless `git fetch --unshallow` first gives the changelog its history, and the image reports the
   tag's version (`RSOS_CI_OS_VERSION` -> `BR2_RETROSTONE_VERSION`), which `make-rsu.sh` checks. Uploaded as the
   artifact `image-<board>`.
9. Image tests (not a gate yet): `run-tests.sh image <board> <output>` runs the board/common tests and the board's
   own `tests/*.sh` against the build, from a fake `$HOME` whose `rsos/output` is the CI output: for the RetroStone2
   `data-partition-test.sh` (as root: loop devices and a private mount namespace; `modprobe exfat` first) and
   `boot-ab-qemu-test.sh` (U-Boot for QEMU's cubieboard). They are `continue-on-error` until they have proved stable
   on the runners: check their result in the log.
10. `legal-info` (tags, or by hand): `make legal-info` -> `legal-info-<image>-<version>.tar.xz` (split into
    1900 MiB parts if it exceeds GitHub's 2 GiB per file; the RetroStone2 one was 1.53 GiB when this was written,
    mostly already-compressed source tarballs such as linux-firmware and the kernel). The permission-only homebrew ROMs are never in it
    (`RSOS_HOMEBREW_REDISTRIBUTE = NO`).

### The release job (tags only)

Once every board succeeded, it downloads the artifacts (images and `.rsu` update packages), adds
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

1. Check that `ci.yml` is green on the commit, and that the `UPDATE_SIGNING_KEY` secret is set (section 7). The
   images report the tag's version (`BR2_RETROSTONE_VERSION`); raise `RSOS_FRONTEND_VERSION` in
   `package/rsos-frontend/rsos-frontend.mk` too, for the development builds made afterwards.
2. Tag and push (the tag name is the version: `v0.1.0`, `v0.2.0-rc1`):
   ```sh
   git tag -a v0.1.0 -m "RetroStoneOS 0.1.0"
   git push origin v0.1.0
   ```
3. Actions > Images: one job per board, then "GitHub Release". The release appears under Releases when every
   board is done.
4. Edit the notes on GitHub if needed (text only: the files cannot change once published when release immutability
   is on).

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
`<branch>-<commit>`, e.g. `retrostoneos-main-1a2b3c4d-retrostone2-dev.img.xz`. No release is made.

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

These are estimates: the first tag gives the real numbers (each job's log prints the build time; the ccache
statistics follow). Boards build in parallel, so a release takes as long as the slowest board.

**The 6 h limit.** The build step stops after 300 minutes and the job after 355. Plan, in order:

1. **Re-run the failed job.** The ccache is saved even after a timeout, so the second run compiles far less.
2. `RSOS_CI_PER_PACKAGE=1` (an `env:` of the build job): `BR2_PER_PACKAGE_DIRECTORIES=y` and a top-level
   `make -j<cores>`, so that packages build side by side (the single-threaded configure steps overlap). Test it by
   hand first: every package of the external tree must be per-package-safe.
3. **A larger runner**: set the repository variable `IMAGES_RUNNER` (Settings > Secrets and variables > Actions >
   Variables) to a larger runner's label (Team/Enterprise plans, billed per minute), or to `self-hosted` for a
   self-hosted runner (e.g. the 16-core WSL build host: Settings > Actions > Runners > "New self-hosted runner";
   `free-disk.sh` is skipped there). A public repository should only use a self-hosted runner for these trusted
   triggers (tags and manual runs), which is the case: `images.yml` never runs on pull requests.
4. Last resort: build the heavy cores in a separate job and hand them over (not implemented: it needs the cores
   installed from a prebuilt archive).

**Cost**: nothing for a public repository (minutes and artifact storage are free). The caches count towards the
10 GB per repository (older entries are evicted first); a larger runner or more cache storage is paid.

## 7. What the owner sets on GitHub

- **Settings > Actions > General**:
  - "Actions permissions": allow actions created by GitHub (the workflows only use `actions/checkout`,
    `actions/cache`, `actions/upload-artifact` and `actions/download-artifact`, pinned by commit), or "Allow all".
    Optionally tick "Require actions to be pinned to a full-length commit SHA".
  - "Workflow permissions": **Read repository contents** (the default for new repositories). The release job asks for
    `contents: write` itself.
  - "Fork pull request workflows": keep "Require approval for first-time contributors".
- **Settings > Secrets and variables > Actions**: the secret **`UPDATE_SIGNING_KEY`**: the whole content (both lines)
  of the update signing key, `RetroStoneOS-keys\update-signing.key` on the owner's PC (docs/updates.md §3; keep a
  backup offline: consoles only accept packages signed with it). Without it, the releases' update packages are
  unsigned and the consoles cannot install them. Then the secret `HOMEBREW_TOKEN` (section 4). Optional: the secret
  `UART_PASSWORD`, only needed if a defconfig selects the UART password login
  (`BR2_RETROSTONE_UART_SHELL_PASSWORD`; the build stops without it); the variable `IMAGES_RUNNER` (section 6).
- **Settings > General > Releases: "Enable release immutability"** (recommended): once published, a release's files
  and tag cannot be changed, so a downloaded image always matches `SHA256SUMS`. The workflow already publishes
  through a draft, as immutability requires.
- **Tag protection** (Settings > Rules > Rulesets > New tag ruleset, target `v*`): restrict creation and deletion of
  release tags to the maintainers.
- **Branch protection** of `main` (optional): require the `CI` checks before merging.
- Dependabot for the pinned actions (optional): a `.github/dependabot.yml` with the `github-actions` ecosystem
  proposes the SHA updates.

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
| make-rsu: "the image reports version X, the tag says Y" | the tag is not a plain version (`v0.2.0`, `v0.2.0-rc1`), or `BR2_RETROSTONE_VERSION` did not reach the image: check the "configure" step |
| The release has `*-unsigned.rsu` files | the `UPDATE_SIGNING_KEY` secret was missing: add it, then sign the packages by hand (`rsos-mkupdate sign`, docs/updates.md §3) or tag a new version |
