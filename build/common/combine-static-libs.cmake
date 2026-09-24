# Combine static libraries into one archive with llvm-ar's MRI script mode — the portable twin of
# build/linux/combine-static-libs.sh for hosts that cannot run a bash script (an Android build on a
# Windows host, where the post-build command runs through cmd.exe).
#
#   cmake -DAR=<llvm-ar> -DOUTPUT=<archive> -DINPUTS=<a.a,b.a,...> -P combine-static-libs.cmake
# (comma-separated: a `;` list would be split by the custom command that passes it, and `|` turns
# into a pipe inside the `cmd /C "..."` Ninja wraps the post-build step in.)
if (NOT AR OR NOT OUTPUT OR NOT INPUTS)
    message(FATAL_ERROR "combine-static-libs.cmake: AR, OUTPUT and INPUTS are required")
endif()
string(REPLACE "," ";" INPUTS "${INPUTS}")

set(SCRIPT "${OUTPUT}.mri")
set(MRI "CREATE ${OUTPUT}\n")
foreach(LIB ${INPUTS})
    string(APPEND MRI "ADDLIB ${LIB}\n")
endforeach()
string(APPEND MRI "SAVE\nEND\n")
file(WRITE "${SCRIPT}" "${MRI}")
file(REMOVE "${OUTPUT}")

execute_process(
    COMMAND "${AR}" -M
    INPUT_FILE "${SCRIPT}"
    RESULT_VARIABLE RESULT
    ERROR_VARIABLE ERR
)
if (NOT RESULT EQUAL 0)
    message(FATAL_ERROR "combine-static-libs.cmake: ${AR} -M failed (${RESULT}): ${ERR}")
endif()
file(REMOVE "${SCRIPT}")
