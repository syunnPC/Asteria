# SPDX-License-Identifier: Apache-2.0
# Layout headers for hand-written assembly.
#
# A layout unit, Source/Platform/<Isa>/Layout/<Name>.cpp, states the constants
# an assembly file takes from C++ types with ASTERIA_ASM_LAYOUT (see
# Source/Platform/AsmLayout.hpp). asteria_add_asm_layout_units compiles each
# unit to assembly with COMPILER and FLAGS, and Tools/Build/GenerateAsmLayout.py
# turns the markers into Generated/Platform/<Isa>/<Name>.h of the build tree.
# The header is a byproduct that is rewritten only when a value changes, so
# assembly units rebuild only then. TARGET names a custom target that
# generates all headers; libraries with .S sources depend on it.
function(asteria_add_asm_layout_units target isa)
    cmake_parse_arguments(PARSE_ARGV 2 arg "" "COMPILER" "UNITS;FLAGS")
    set(directory "${CMAKE_BINARY_DIR}/Generated")
    set(stamps)
    foreach(unit IN LISTS arg_UNITS)
        get_filename_component(name "${unit}" NAME_WE)
        string(TOUPPER "${name}" upper)
        set(header "${directory}/Platform/${isa}/${name}.h")
        set(stamp "${directory}/${name}.stamp")
        add_custom_command(OUTPUT "${stamp}"
            BYPRODUCTS "${header}"
            COMMAND "${arg_COMPILER}" -std=c++23 ${arg_FLAGS} -I "${ASTERIA_ROOT}/Source"
                -MD -MF "${directory}/${name}.d" -MT "${stamp}" -S "${unit}" -o "${directory}/${name}.s"
            COMMAND "${Python3_EXECUTABLE}" "${ASTERIA_ROOT}/Tools/Build/GenerateAsmLayout.py"
                "${directory}/${name}.s" "${header}" ASTERIA_GENERATED_${upper}_H_INCLUDED
            COMMAND "${CMAKE_COMMAND}" -E touch "${stamp}"
            DEPENDS "${unit}" "${ASTERIA_ROOT}/Tools/Build/GenerateAsmLayout.py"
            DEPFILE "${directory}/${name}.d"
            VERBATIM)
        list(APPEND stamps "${stamp}")
    endforeach()
    add_custom_target(${target} DEPENDS ${stamps})
endfunction()
