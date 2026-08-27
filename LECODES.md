# LeCodes fork of Filament

This is `letary/filament`, branch **`lecodes`**: upstream `google/filament` at the tag named in
the branch's tag (`lecodes-<upstream version>`, e.g. `lecodes-1.75.1`) plus one commit per local
change. The LeCodes monorepo consumes it as the `packages/creator-gl/filament` submodule; this
file is also the marker its build scripts check for (`check-filament.sh`), so a submodule that
points at upstream instead of this branch fails loudly before anything compiles.

## The changes

Every non-upstream commit is titled `lecodes NNNN: …` and its message says **why** it exists,
**when** it can be dropped (`Drop when:`) and **what breaks** without it (`Symptom if missing:`).
That message is the documentation — keep the format when adding one.

```sh
git log --oneline --reverse upstream/release..lecodes      # the list (after `git remote add upstream https://github.com/google/filament.git`)
git show <sha>                                            # one change with its rationale
```

| # | Area | Change |
|---|---|---|
| 0001 | Metal (macOS) | report `ShaderModel::MOBILE` so the iOS materials/uberarchive load |
| 0002 | Metal | hold descriptor-set argument buffers strongly (hardening) |
| 0003 | core | always commit a MaterialInstance's descriptor set |
| 0004 | CMake | `FILAMENT_MATC_TARGET_OVERRIDE` (mobile materials on the Linux GLES host) |
| 0005 | CMake | skip `web/filament-js` + `web/examples` on WASM |
| 0006 | OpenGL | `GL_POINT_SPRITE` in a compatibility context (point-sprite particles on Windows) |
| 0007 | WGL | robust shared-context creation (NVIDIA + tgfx share group) |
| 0008 | WGL | `SWAP_CHAIN_CONFIG_NO_PRESENT` + preferred pixel format + frame fence hand-off |
| 0009 | Vulkan | `Texture::Builder::import()` of an external `VkImage` |
| 0010 | Vulkan | shared-queue lock hooks + frame timeline signal + frame-skipper bypass |
| 0011 | Vulkan | no unsynchronized memcpy into a buffer the GPU still reads (UMA) |
| 0012 | Vulkan | one stride of slack after every vertex buffer (AMD last-vertex fetch) |
| 0013 | gltfio | bone-matrix flush without building an `Animator` (lazy animator) |
| 0014 | gltfio | transcode KTX2 to ASTC/BC7 before ETC2/BC3 |
| 0015 | OpenGL | BPTC (BC7) named with the desktop tokens; never claim an unnameable format |
| 0016 | gltfio | anisotropic filtering on glTF textures (`setDefaultTextureAnisotropy`) |
| 0017 | View | `setStructureSamplingEnabled` — scene depth in the color pass (soft particles) |
| 0018 | ColorGrading | extended-range (HDR display) output + `ExtendedRangeToneMapper` |

## Adding a change

Work in the monorepo's submodule checkout (`packages/creator-gl/filament`) or in a clone of this
repo — it is the same branch:

```sh
git checkout lecodes
# edit, build, verify with the desktop host (packages/desktop/build.ps1)
git commit                     # title: "lecodes NNNN: <area>: <what>"; body: why / Drop when / Symptom if missing
git push origin lecodes
```

Then, in the monorepo, `git add packages/creator-gl/filament` to move the submodule pointer and
add a row to the table above (in the same commit as the change is fine). One commit per concern —
that is what lets a change be dropped on its own once upstream absorbs it.

## Bumping upstream

```sh
git remote add upstream https://github.com/google/filament.git   # once
git fetch upstream --tags
git tag lecodes-<old>                      # keep the old state reachable, e.g. lecodes-1.75.1
git rebase --onto v<new> v<old> lecodes    # replays every lecodes commit; resolve conflicts per commit
git push origin lecodes --force-with-lease
git push origin --tags
```

While rebasing, `git show upstream/<tag> -- <file>` for each conflict: a change whose `Drop when:`
has come true is simply skipped (`git rebase --skip`) and its row removed from the table. After the
bump, the monorepo's `Updating Filament` checklist (`packages/creator-gl/README.md`) says what to
rebuild — materials only when `MATERIAL_VERSION` in
`libs/filabridge/include/filament/MaterialEnums.h` changed.

`main` on this repo mirrors upstream and is never built from.
