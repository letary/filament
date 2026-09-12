/*
 * Copyright (C) 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef GLTFIO_MATERIALINSTANCECACHE_H
#define GLTFIO_MATERIALINSTANCECACHE_H

#include <gltfio/MaterialProvider.h>

#include <utils/debug.h>
#include <utils/FixedCapacityVector.h>

struct cgltf_data;
struct cgltf_material;

namespace filament {
class MaterialInstance;
}

namespace filament::gltfio {

// MaterialInstanceCache
// ---------------------
// Each glTF material definition corresponds to a single MaterialInstance, which are temporarily
// cached when loading a FilamentInstance. If a given glTF material is referenced by multiple
// glTF meshes, then their corresponding Filament primitives will share the same Filament
// MaterialInstance and UvMap. The UvMap is a mapping from each texcoord slot in glTF to one of
// Filament's 2 texcoord sets.
//
// lecodes 0025: lifted out of AssetLoader.cpp so a FFilamentInstance can KEEP the cache it was
// built with and hand it to a later createInstance(asset, donor) — that instance then reuses the
// donor's material instances instead of getting a fresh set (see AssetLoader::createInstance).
//
// Notes:
// - The Material objects (used to create instances) are cached in MaterialProvider, not here.
// - The cache is not responsible for destroying material instances.
class MaterialInstanceCache {
public:
    struct Entry {
        MaterialInstance* instance;
        UvMap uvmap;
    };

    MaterialInstanceCache() {}

    explicit MaterialInstanceCache(const cgltf_data* hierarchy);   // AssetLoader.cpp

    void flush(utils::FixedCapacityVector<MaterialInstance*>* dest) {
        size_t count = 0;
        for (const Entry& entry : mMaterialInstances) {
            if (entry.instance) {
                ++count;
            }
        }
        for (const Entry& entry : mMaterialInstancesWithVertexColor) {
            if (entry.instance) {
                ++count;
            }
        }
        if (mDefaultMaterialInstance.instance) {
            ++count;
        }
        if (mDefaultMaterialInstanceWithVertexColor.instance) {
            ++count;
        }
        assert_invariant(dest->size() == 0);
        dest->reserve(count);
        for (const Entry& entry : mMaterialInstances) {
            if (entry.instance) {
                dest->push_back(entry.instance);
            }
        }
        for (const Entry& entry : mMaterialInstancesWithVertexColor) {
            if (entry.instance) {
                dest->push_back(entry.instance);
            }
        }
        if (mDefaultMaterialInstance.instance) {
            dest->push_back(mDefaultMaterialInstance.instance);
        }
        if (mDefaultMaterialInstanceWithVertexColor.instance) {
            dest->push_back(mDefaultMaterialInstanceWithVertexColor.instance);
        }
    }

    // A null *mat is the default glTF material: it is replaced by `defaultMat` (the caller's
    // static) and served from the default entries.
    Entry* getEntry(const cgltf_material** mat, bool vertexColor, const cgltf_material* defaultMat);

private:
    using EntryVector = utils::FixedCapacityVector<Entry>;
    const cgltf_data* mHierarchy = {};
    EntryVector mMaterialInstances;
    EntryVector mMaterialInstancesWithVertexColor;
    Entry mDefaultMaterialInstance = {};
    Entry mDefaultMaterialInstanceWithVertexColor = {};
};

} // namespace filament::gltfio

#endif // GLTFIO_MATERIALINSTANCECACHE_H
