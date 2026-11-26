# Build

Everything runs from the repository root — no manual LLVM setup, no
`/tmp` symlinks, no entering the SDK tree.

## Commands

```sh
make setup     # resolve SDK + LLVM, pre-fetch third-party sources (idempotent)
make           # build -> build/apps/wup_thinclient.wup
make test      # host-side unit tests
make clean     # remove build/
make rebuild   # clean + build
make deploy    # copy .wup to ${WUP_SHARE_DIR:-$HOME/wup-share}
make help      # list targets
```

## Dependency management

| Dependency | How it is obtained | Pinned to |
|---|---|---|
| libwup SDK (`wup-homebrew` repo, `libwup/` subdir) | git submodule `third_party/wup-homebrew` (auto-init by `make setup`) | `7b641902f9ea134e2d2a585be3ca3f60442ba174` (wozt/wup-homebrew `feature/netfs`) |
| LLVM (clang, ld.lld, llvm-ar/nm/objcopy, >= 19) | system install; auto-detected by `scripts/setup.sh` (`$LLVM_BIN` override) | toolchain, not vendored |
| littlefs, lwIP, tinf, heatshrink, picolibc, compiler-rt | fetched by the SDK's own `tools/deps.py`/`tools/sdk.py` into `libwup/third_party` and `libwup/build` (git-ignored there) | upstream tags/commits in `tools/deps.py` |

Using your own SDK instead of the submodule:

```sh
make setup LIBWUP_DIR=/home/you/wup-homebrew/libwup
```

`LIBWUP_DIR` must point at the `libwup` directory itself (the one
containing `tools/wupbuild.py`).

## What the scripts do

- `scripts/setup.sh` — picks `LIBWUP_DIR` (env or submodule), detects an
  LLVM bin dir (`$LLVM_BIN`, `/usr/lib/llvm-*/bin`, Homebrew paths, or
  PATH), writes `build/env.sh`, pre-fetches the app-needed sources.
- `scripts/build.sh` — sources `build/env.sh`, runs
  `python3 tools/wupbuild.py pad` **with the app dir as the only argument**
  (so `pad/app.cfg` is honoured), then copies the artifact to
  `build/apps/wup_thinclient.wup`. Fails if no non-empty `.wup` appears.
- `scripts/deploy.sh` — verifies the artifact, copies atomically
  (`.tmp` + rename) into `$WUP_SHARE_DIR` (default `~/wup-share`),
  compares SHA256, prints destination/size/checksum.

## Artifact

`build/apps/wup_thinclient.wup` (~111 KB). `build/` is git-ignored;
release ZIPs/checksums are produced by CI later.
