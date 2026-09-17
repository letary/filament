//------------------------------------------------------------------------------
// Instancing
// ------------------------------------------------------------------------------------------------

highp mat4 object_uniforms_worldFromModelMatrix;
highp mat3 object_uniforms_worldFromModelNormalMatrix;
highp int object_uniforms_morphTargetCount;
highp int object_uniforms_flagsChannels;                   // see packFlags() below (0x00000fll)
highp int object_uniforms_objectId;                        // used for picking
highp float object_uniforms_userData;   // TODO: We need a better solution, this currently holds the average local scale for the renderable
#if MATERIAL_FEATURE_LEVEL > 0
// lecodes 0029: the renderable's baked ambient cube (RenderableManager::setAmbientCube); [6] = (sky visibility, signature x 2, the baked sun visibility)
highp vec4 object_uniforms_ambientCube[7];
#endif

//------------------------------------------------------------------------------
// Instance access
//------------------------------------------------------------------------------

void initObjectUniforms() {
    // Adreno drivers workarounds:
    // - We need to copy each field separately because non-const array access in a UBO fails
    //    e.g.: this fails `p = objectUniforms.data[instance_index];`
    // - We can't use a struct to hold the result because Adreno driver ignore precision qualifiers
    //   on fields of structs, unless they're in a UBO (which we just copied out of).

#if defined(FILAMENT_HAS_FEATURE_INSTANCING)
    highp int i;
#   if defined(MATERIAL_HAS_INSTANCES)
    // instancing handled by the material
    if ((objectUniforms.data[0].flagsChannels & FILAMENT_OBJECT_INSTANCE_BUFFER_BIT) != 0) {
        // hybrid instancing, we have a instance buffer per object
        i = logical_instance_index;
    } else {
        // fully manual instancing
        i = 0;
    }
#   else
    // automatic instancing
    i = logical_instance_index;
#   endif
#else
    // we don't support instancing (e.g. ES2)
    const int i = 0;
#endif
    object_uniforms_worldFromModelMatrix        = objectUniforms.data[i].worldFromModelMatrix;
    object_uniforms_worldFromModelNormalMatrix  = objectUniforms.data[i].worldFromModelNormalMatrix;
    object_uniforms_morphTargetCount            = objectUniforms.data[i].morphTargetCount;
    object_uniforms_flagsChannels               = objectUniforms.data[i].flagsChannels;
    object_uniforms_objectId                    = objectUniforms.data[i].objectId;
    object_uniforms_userData                    = objectUniforms.data[i].userData;
#if MATERIAL_FEATURE_LEVEL > 0
    object_uniforms_ambientCube[6]              = objectUniforms.data[i].reserved[6];
    if (object_uniforms_ambientCube[6].yz == vec2(7885.0, -7885.0)) {   // the signature (Scene.cpp), not a flag
        object_uniforms_ambientCube[0]          = objectUniforms.data[i].reserved[0];
        object_uniforms_ambientCube[1]          = objectUniforms.data[i].reserved[1];
        object_uniforms_ambientCube[2]          = objectUniforms.data[i].reserved[2];
        object_uniforms_ambientCube[3]          = objectUniforms.data[i].reserved[3];
        object_uniforms_ambientCube[4]          = objectUniforms.data[i].reserved[4];
        object_uniforms_ambientCube[5]          = objectUniforms.data[i].reserved[5];
    }
#endif
}

#if defined(FILAMENT_HAS_FEATURE_INSTANCING) && defined(MATERIAL_HAS_INSTANCES)
/** @public-api */
highp int getInstanceIndex() {
    return logical_instance_index;
}
#endif

#if CLIENT_MATERIAL_API_LEVEL >= UNSTABLE_MATERIAL_API_LEVEL
/** @public-api */
// NOTE: Under API level 2, these functions are available with highp precision in both
// vertex and fragment shaders.
highp mat4 getWorldFromModelMatrix() {
    return object_uniforms_worldFromModelMatrix;
}

/** @public-api */
highp mat3 getWorldFromModelNormalMatrix() {
    return object_uniforms_worldFromModelNormalMatrix;
}
#elif defined(GL_FRAGMENT_SHADER)
// NOTE: getWorldFromModelMatrix requires API level 2 in the fragment shader.
// (It's available at API level 1 for vertex shader, see surface_getters.vs)
// We define redirect macros here (only during fragment shader compilation) to trigger
// a custom compiler error if a material compiled with API level 1 attempts to call them.
#define getWorldFromModelMatrix ERROR_getWorldFromModelMatrix_api_level_2_END
#define getWorldFromModelNormalMatrix ERROR_getWorldFromModelNormalMatrix_api_level_2_END
#endif
