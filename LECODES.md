# LeCodes fork of Filament

This is `letary/filament`, branch **`lecodes`**: upstream `google/filament` at the release tag the
branch sits on (`v1.77.1` today) plus ONE commit per local change. The LeCodes core monorepo
(`lecodes-next`) consumes it as the `engines/gl/filament` submodule; this file is also the marker its
build scripts check for (`engines/gl/check-filament.sh`, the PowerShell twins in
`hosts/desktop/build.ps1` and `hosts/android/build-android.ps1`), so a submodule that points at
upstream instead of this branch fails loudly before anything compiles.

## The changes

Every non-upstream commit is titled `lecodes NNNN: <area>: <what>` and its message says **why** it
exists, **when** it can be dropped (`Drop when:`) and **what breaks** without it (`Symptom if
missing:`). That message is the documentation — keep the format when adding one. The commit also
carries its row in the table below, so dropping a change (`git rebase --skip`) drops its row.

```sh
git log --oneline --reverse v1.77.1..lecodes      # the list
git show <sha>                                    # one change with its rationale
```

Numbers are never reused. Gaps: 0001 (Metal reported `ShaderModel::MOBILE` on macOS under
`CREATOR_MACOS_MOBILE` — the in-repo macOS host compiles its materials `-p desktop`, nothing defines the
macro; dropped at the 1.77.1 rebase), 0011 (Vulkan: no memcpy into a STATIC buffer the GPU still reads —
upstream 1.76 dropped STATIC from the bypass itself), 0021 (`FILAMENT_GLTFIO_DRACO` — no consumer),
0026 / 0032 / 0034 / 0040 / 0042 (fix-ups folded into 0015 / 0027 / 0029 / 0039 / 0028 at the same
rebase; the row says "absorbs"), 0035–0037 (the PCSS series, withdrawn — see 0038). The pre-rebase
branch (v1.75.1 + 0000–0044, the dropped ones included) is tag `lecodes-1.75.1-last`; the branch as
first published is `lecodes-1.75.1`.

| # | Area | Change |
|---|---|---|
| 0002 | Metal | hold descriptor-set argument buffers strongly (hardening) |
| 0003 | core | always commit a MaterialInstance's descriptor set |
| 0004 | CMake | `FILAMENT_MATC_TARGET_OVERRIDE` (mobile materials on the Linux GLES host) |
| 0005 | CMake | skip `web/filament-js` + `web/examples` on WASM |
| 0006 | OpenGL | `GL_POINT_SPRITE` in a compatibility context (point-sprite particles on Windows) |
| 0007 | WGL | robust shared-context creation (NVIDIA + tgfx share group) |
| 0008 | WGL | `SWAP_CHAIN_CONFIG_NO_PRESENT` + preferred pixel format + frame fence hand-off |
| 0009 | Vulkan | `Texture::Builder::import()` of an external `VkImage` |
| 0010 | Vulkan | shared-queue lock hooks + frame timeline signal + frame-skipper bypass |

## Adding a change

Work in the monorepo's submodule checkout (`engines/gl/filament`) or in a clone of this repo — it is
the same branch:

```sh
git checkout lecodes
# edit, build, verify with the desktop host (hosts/desktop/build.ps1)
# add the row to the table above, in the same commit
git commit                     # title: "lecodes NNNN: <area>: <what>"; body: why / Drop when / Symptom if missing
git push origin lecodes
```

Then, in the monorepo, `git add engines/gl/filament` to move the submodule pointer. One commit per
concern — that is what lets a change be dropped on its own once upstream absorbs it. A later fix to
an existing change is a fix-up of THAT commit (`git commit --fixup <sha>` + `git rebase -i
--autosquash`, force-push), not a new number: the branch is rebased anyway at every upstream bump,
and one commit per concern is what keeps that rebase a sequence of clean skips and replays.

## Bumping upstream

```sh
git remote add upstream https://github.com/google/filament.git   # once
git fetch upstream --tags
git tag lecodes-<old>-last                 # keep the old state reachable, e.g. lecodes-1.77.1-last
git rebase --onto v<new> v<old> lecodes    # replays every lecodes commit; resolve conflicts per commit
git push origin lecodes --force-with-lease
git push origin --tags
```

While rebasing, `git show v<new> -- <file>` for each conflict: a change whose `Drop when:` has come
true is simply skipped (`git rebase --skip`, its row goes with it — mention the number under "Gaps"
above). Then update the version named at the top of this file. After the bump, the monorepo's
`Updating Filament` checklist (`engines/gl/README.md`) says what to rebuild — every material and
uberarchive when `MATERIAL_VERSION` in `libs/filabridge/include/filament/MaterialEnums.h` changed
(75 → 77 at the 1.77.1 bump: `matcVersion` in `engines/gl/materials/materials.json` follows it).

`main` on this repo mirrors upstream and is never built from.
