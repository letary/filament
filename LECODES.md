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
| 0012 | Vulkan | one stride of slack after every vertex buffer (AMD last-vertex fetch) |
| 0013 | gltfio | bone-matrix flush without building an `Animator` (lazy animator) |
| 0014 | gltfio | transcode KTX2 to ASTC/BC7 before ETC2/BC3 |
| 0015 | OpenGL | BPTC (BC7) on desktop GL: the `_EXT` tokens aliased to the ARB names bluegl's headers carry, `GL_ARB_texture_compression_bptc` counts as support (NVIDIA lists only the ARB name — every BPTC format was "unsupported": BC7 KTX2 pages fell through to ETC2 / DXT5, the BC6H HDR lightmap panicked at `Texture::Builder::build`), and `isTextureFormatSupported` never claims a compressed format `getInternalFormat` cannot name (absorbs 0026) |
| 0016 | gltfio | anisotropic filtering on glTF textures (`setDefaultTextureAnisotropy`) |
| 0017 | View | `setStructureSamplingEnabled` — scene depth in the color pass (soft particles, decals) |
| 0018 | ColorGrading | extended-range (HDR display) output + `ExtendedRangeToneMapper` (compiled in; no host asks for an EDR swap chain yet) |
| 0019 | gltfio | bone matrices computed once per identical skin + target world transform (modular characters) |
| 0020 | OpenGL | external samplers as `sampler2D` where `OES_EGL_image_external_essl3` is missing (WebGL): shader rewrite + 2D texture allowed in the external slot |
| 0022 | ktxreader + gltfio | texture size cap: `Ktx2Reader::setMaxTextureSize` skips top mip levels, gltfio `setDefaultMaxTextureSize` downsamples stb images (the texture-quality setting) |
| 0023 | Vulkan | device extensions + features for a guest renderer on the shared device (sokol-gfx: descriptor buffer, descriptor indexing, buffer device address, synchronization2, copy commands 2); `filament_vk_guestBindingFeatures()` |
| 0024 | gltfio | ubershader archive order: `specular_*` before `transmission_*`/`volume_*` — a KHR_materials_specular-only material no longer takes the refractive transmission ubershader (SSR mip pyramid + second colour pass every frame) |
| 0025 | gltfio | `AssetLoader::createInstance(asset, donor)`: a new instance shares the donor's material instances (the `MaterialInstanceCache` is kept on the instance, sharers never destroy the list) — a scene-file level of 9 000 prefab copies no longer costs a material instance (UBO + descriptor set) per copy |
| 0027 | backend/opengl | desktop shader-compiler pool: 2–4 threads on Windows / Linux (was 1, "tbd" upstream) and 6 WGL worker contexts (was 2), bound under a lock with retries — the pool's threads all `wglMakeCurrent()` through the one dummy-window DC, a GDI DC is not thread-safe, and two racing binds failed at random (the desktop host's start-up flake "Failed to make current"); a `Material::compile()` batch (the LeCodes shader precompile) finishes in a fraction of the time (absorbs 0032) |
| 0028 | backend/vulkan | pipeline-cache prewarming that works: programs are prewarmed against the real pipeline CLASSES seen at draw time (key minus program), the finished pipelines adopted into the draw-time map, a program registry re-prewarms on new classes, and the decision + every costly draw-time build are logged. Upstream's stand-in pipeline (dynamic rendering, undefined formats, dynamic vertex input) is GONE: it never produced a driver-cache hit on NVIDIA and crashed the 560.94 driver's shader compiler on the prewarm thread (StreetCity, RTX 3070) — a program with no class yet is only registered and warmed by its layout's first class. A class owns a `resource_ptr` to its render pass and every queued job pins one (`mPins`, released in `gc()`), so VulkanFboCache cannot evict a pass a queued job will hand to the driver (absorbs 0042). Since 1.77.1 the class key is upstream's `mStaticPipelineKey` (the dynamic-state split) and upstream enables the vertex-input dynamic-state feature itself |

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
