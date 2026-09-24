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
| 0019 | gltfio | bone matrices computed once per identical skin + target world transform (modular characters) |
| 0020 | OpenGL | external samplers as `sampler2D` where `OES_EGL_image_external_essl3` is missing (WebGL): shader rewrite + 2D texture allowed in the external slot |
| 0021 | gltfio | `FILAMENT_GLTFIO_DRACO` option (default ON) so a host can build without Draco |
| 0022 | ktxreader + gltfio | texture size cap: `Ktx2Reader::setMaxTextureSize` skips top mip levels, gltfio `setDefaultMaxTextureSize` downsamples stb images (the texture-quality setting) |
| 0023 | Vulkan | device extensions + features for a guest renderer on the shared device (sokol-gfx: descriptor buffer, descriptor indexing, buffer device address, synchronization2, copy commands 2); `filament_vk_guestBindingFeatures()` |
| 0024 | gltfio | ubershader archive order: `specular_*` before `transmission_*`/`volume_*` — a KHR_materials_specular-only material no longer takes the refractive transmission ubershader (SSR mip pyramid + second colour pass every frame) |
| 0025 | gltfio | `AssetLoader::createInstance(asset, donor)`: a new instance shares the donor's material instances (the `MaterialInstanceCache` is kept on the instance, sharers never destroy the list) — a scene-file level of 9 000 prefab copies no longer costs a material instance (UBO + descriptor set) per copy |
| 0026 | backend/opengl | BPTC support test accepts `GL_ARB_texture_compression_bptc` (what desktop drivers report; the EXT spelling is ES-only) — BC7 KTX2 pages and the BC6H HDR lightmap were falling through to ETC2 / DXT5 on desktop GL |
| 0027 | backend/opengl | desktop shader-compiler pool: 2–4 threads on Windows / Linux (was 1, "tbd" upstream) and 6 WGL worker contexts (was 2) — a `Material::compile()` batch (the LeCodes shader precompile) finishes in a fraction of the time |
| 0028 | backend/vulkan | pipeline-cache prewarming that works: programs are prewarmed against the real pipeline CLASSES seen at draw time (key minus program), the finished pipelines adopted into the draw-time map, a program registry re-prewarms on new classes, `VK_EXT_vertex_input_dynamic_state`'s feature is actually enabled, and the decision + every costly draw-time build are logged — upstream's stand-in pipeline (dynamic rendering, undefined formats, writes masked) never produced a driver-cache hit on NVIDIA |
| 0029 | filament + shaders | BAKED AMBIENT LIGHT per renderable: `RenderableManager::setAmbientCube(instance, float4 cube[6], skyVisibility, sunVisibility = 1)` / `clearAmbientCube` - a side table by entity (the upstream SoA untouched), written by `FScene::prepareVisibleRenderables` into `PerRenderableData.reserved[0..6]` ([6] = sky visibility, a two-float SIGNATURE, sun visibility: materials compiled with this may meet an engine without it). Every lit material then takes its diffuse indirect light from the cube (weighed by n^2, / pi, scaled by the exposure: the cube is lux), the IBL's reflections through the sky visibility, and the DIRECTIONAL light through the sun visibility - the baked statics' shadow on a mover, so they need not cast in real time. creator-gl's light grid drives it (`src/lightvolume.cpp`) |
| 0030 | shaders | `float getSunShadowVisibility()` for material code: the directional light's shadow-map visibility at the fragment (far attenuation applied, whatever the renderable's light channels; 1 without a shadow map). Prototype at the end of `surface_getters.fs`, defined in `surface_main.fs` and `surface_depth_main.fs` - user material code is pasted BEFORE the shadowing code. lightmap.mat takes a mover's real-time shadow out of the baked sun with it |
| 0031 | tools/cmgen | `--clamp-sun[=degrees]` (equirectangular input): inside 8 degrees of the brightest texel nothing stays brighter than the median of the ring outside; prints the energy kept and the disc's illuminance in the picture's units. A lighting probe next to a directional sun must not hold the sun a second time; `lecodes assets sky --no-sun` uses it and calibrates environmentIntensity from the disc |
| 0032 | backend/opengl (WGL) | worker contexts bind under a lock, with retries: the compiler pool's threads (0027) all call `wglMakeCurrent()` through the one dummy-window DC, a GDI DC is not thread-safe, and two racing binds failed at random - the desktop host's start-up flake ("Failed to make current"). Failures are logged with the Windows error |
| 0033 | libs/gltfio | `KHR_materials_emissive_strength` was applied twice (folded into `emissiveFactor` AND passed as `emissiveStrength`, which the ubershader multiplies in again: strength 10 drew as 100) - it is folded only for a material without an `emissiveStrength` parameter |
| 0034 | shaders | THE AMBIENT CUBE REFLECTS TOO (0029's renderables): indoors a mover reflected nothing (the IBL x a sky visibility of 0 - a metal barrel black next to baked walls). `evaluateIBL` adds the cube read along the reflection vector, E / pi, minus the sky's share of it (the SH irradiance along r x the sky visibility - that part the IBL already reflects, sharp): the room's light as a six-sided blur. `reserved[6].x` + 2 switches it off (creator-gl `CREATOR_CUBE_SPECULAR=0`) |
| 0038 | filament | `ShadowMap.cpp`, `details/Engine.h`: debug property `d.shadowmap.log_directional_fit` - twice a second `updateDirectional` prints `[shadowfit]`: the light's near / far and depth range, LiSPSM / VSM flags and the world size of a map texel at the centre / bottom / top. Additive, off by default (creator-gl turns it on under `CREATOR_SHADOW_DEBUG=1`). It is what showed a +-100 m particle box in the caster set as a 239 m depth range (2026-09-19). Numbers 0035-0037 are not reused: they were the PCSS series (a receiver-only renderable flag, the VSM pre-blur under PCSS, a depth saturate in the EVSM samplers), WITHDRAWN the same day - filament's VSM / PCSS filter what is under the pixel in the map, so keeping receivers out of it is the wrong regime, and creator-gl now falls back to PCF on baked levels instead. Tag `lecodes-archive-pcss-0035-0037` keeps them. |
| 0039 | filament + shaders | `ShadowType::DPCF` IS A TYPE OF ITS OWN AGAIN: PCSS on the plain DEPTH map. Upstream deprecated it (#10188: it falls back to the EVSM `PCSS`), and the EVSM filters read the blocker and the penumbra from what is under the pixel IN THE MAP - every receiver has to be in it. A level with baked statics keeps them out (their shadow is in the lightmap), where those filters give hard, ragged, view-dependent shadows. `surface_shadowing.fs`: `ShadowSample_DPCF` (runtime type 3) - a 32-tap blocker search and a 64-tap variable-width PCF (2x2 bilinear per tap) on a Vogel spiral turned per pixel, receiver-plane depth bias, the penumbra in METRES through the gradients of the light matrix (so LiSPSM is accounted for; upstream's old version ignored it), width = blocker distance x the uniform bulb radius, 0.6 m at most, 1.5 texels at least. C++: `FView::hasPCSS()` no longer covers DPCF, `hasDPCF()` added; DPCF binds the depth map without compare (PCFd's binding), sends `shadowSamplingType` 3, and turns depth clamp off (a clamped caster has no distance). It uses the materials' existing sampler2D shadow variant: every material is recompiled. creator-gl picks it for `shadowsQuality` 3 on baked levels (scene.cpp `effectiveSunQuality`). |
| 0040 | shaders | `surface_shadowing.fs` `ShadowSample_DPCF` (0039): a blocker is STRICTLY above the receiver's plane, that plane never goes under 0, and the receiver's depth is saturated AFTER its slopes are taken. A receiver past the light's far plane (= the deepest caster IN VIEW) saturates to 0, which is also what an EMPTY texel holds; with `>=` every tap counted as a blocker and the whole ground past the far plane went dark along a straight line, coming and going with what is in view (fps-demo yard, 2026-09-19). Every material is recompiled. |
| 0041 | NOOP backend | `NoopDriver::getShaderModel` reports `MOBILE` in an `FILAMENT_SUPPORTS_EGL_ON_LINUX` build (a GLES host whose materials, the built-in default one included, are compiled `-p mobile` by 0004) — a NOOP engine there otherwise rejects every material (`was not built for desktop`) and gltfio dereferences a null default material: the windowless server host (lecodes-server) on Linux. |
| 0042 | backend/vulkan | prewarming (0028) without upstream's stand-in pipeline: the dynamic-rendering / `VK_FORMAT_UNDEFINED` / dynamic-vertex-input pipeline built for every program at load crashed the driver's shader compiler on the prewarm thread (StreetCity, RTX 3070, NVIDIA 560.94 — clean on two newer drivers) and never produced a cache hit; a program with no class yet is only registered and warmed by its layout's first class. Also a pipeline CLASS owns a `resource_ptr` to its render pass and every queued job pins one (`mPins`, released in `gc()` when the job reports) — 0028 let VulkanFboCache evict a pass while a job for it was still queued behind a burst of builds. |
| 0043 | CMake | `combine_static_libs` on a WINDOWS HOST cross-compiling (Android): the post-build step runs through `cmd.exe`, which cannot execute `build/linux/combine-static-libs.sh` — `libgeometry_combined.a` (and filamat's, abseil's) failed to build. `elseif (CMAKE_HOST_WIN32)` combines with the toolchain's `llvm-ar -M` through `build/common/combine-static-libs.cmake` (inputs comma-joined: `;` would be split by the custom command, `|` becomes a pipe inside Ninja's `cmd /C "..."`). The LeCodes Android host builds on Windows since 2026-09-24 (`hosts/android/build-android.ps1`, with upstream's `ANDROID_ON_WINDOWS` for resgen). |

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
