# agb_add_game(): builds a 100%-decompiled GBA game (pret-style decomp: C game code, data
# units in GNU-as) as a native program on top of the com.recomp.gba runtime.
#
# The game code is compiled from the decomp's own sources with the runtime's prelude
# (compat/agb_pret.h) force-included, which redirects the hardware (memory regions, IO
# registers, DMA) to the runtime. The data units are converted by tools/asm_port.py and
# assembled for the host. Code units in ARM assembly (crt0, BIOS calls, m4a_1, ...) are
# excluded and replaced by the runtime or the game glue.
#
# Today's host is 32-bit Windows (the game stores pointers in 32-bit fields). Configure
# from an x86 Visual Studio environment with clang (tools/build_game.ps1 does this), in a
# project declared with `project(<name> C ASM)`.
#
#   agb_add_game(
#       NAME khcom                          # executable name (khcom.exe), also the save file name
#       TITLE "..."                         # window title
#       DECOMP <dir>                        # the decomp checkout
#       UNITS_JSON <file>                   # link units, {"units": [{"src": ...}, ...]} (game generator)
#       LDSCRIPT <file>                     # decomp linker script: its `name = 0x...;` symbols
#       ROM <file>                          # ROM image, for ROM-address symbols with no data label
#       SYMS name=0xADDR ...                # more absolute symbols (heap starts, BIOS area words)
#       EXCLUDE_REGEX <re>                  # units replaced by the runtime / glue
#       DEFINES ... INCLUDES ...            # game compile definitions / include dirs
#       PRELUDE <h>                         # game prelude, force-included after the runtime's
#       PATCHES <file.patch>...             # unified diffs against the decomp, applied to copies
#       EXTRA_SOURCES <c>...                # game glue (entry point, IRQ dispatch, BIOS call names)
#       PUBLISH_DIR <dir>                   # native guest: where the built exe/pdb are copied (optional)
#       PACKAGE <id> PACKAGE_DIR <dir>)     # wasm guest: the game package to publish into (Source/Guest, Assets)
cmake_minimum_required(VERSION 3.20)

set(AGB_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}/.." CACHE INTERNAL "")
get_filename_component(AGB_RUNTIME_DIR "${AGB_RUNTIME_DIR}" ABSOLUTE)

function(_agb_list outvar)
    execute_process(COMMAND ${ARGN} OUTPUT_VARIABLE raw RESULT_VARIABLE result)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${ARGN} failed")
    endif()
    string(STRIP "${raw}" raw)
    if(raw STREQUAL "")
        set(${outvar} "" PARENT_SCOPE)
    else()
        string(REPLACE "\n" ";" raw "${raw}")
        set(${outvar} ${raw} PARENT_SCOPE)
    endif()
endfunction()

set(AGB_GUEST "native" CACHE STRING "How the game code is built: native (32-bit x86, Windows) or wasm (wasm2c, any host)")
set_property(CACHE AGB_GUEST PROPERTY STRINGS native wasm)
set(AGB_WASM_ROM_CACHE_KB "" CACHE STRING "wasm test host: page the read-only data through a cache of this size (KB), as on small consoles")
option(AGB_WASM_WATCH "wasm test host: report guest stores to the AGBW_WATCH range (slow, debugging)" OFF)
set(AGB_WASM_MEMORY_MB "64" CACHE STRING "Linear memory limit of the wasm link in MB (the guest only allocates what it uses)")

# Finds <Tools>/<pattern> in this directory or one of its parents (wasi-sdk, wabt).
function(_agb_find_tool var pattern)
    if(${var} AND EXISTS "${${var}}")
        return()
    endif()
    set(dir "${CMAKE_SOURCE_DIR}")
    foreach(i RANGE 12)
        file(GLOB found LIST_DIRECTORIES true "${dir}/Tools/${pattern}")
        if(found)
            list(SORT found)
            list(GET found -1 found)
            set(${var} "${found}" CACHE PATH "${pattern}" FORCE)
            return()
        endif()
        get_filename_component(dir "${dir}" DIRECTORY)
    endforeach()
    message(FATAL_ERROR "${pattern} not found in a Tools/ folder above the project: set ${var}")
endfunction()

# ---- wasm guest ---------------------------------------------------------------------------
# The game, its data and the portable runtime core (Runtime/src) are compiled to one
# wasm32 module (32-bit pointers on any host) and translated to C by wasm2c (WABT). The
# module's data segments become <name>.agbdata, loaded into the guest memory at start
# (Runtime/wasm/agbw_guest.c); the C is compiled with the host program. Here the host is
# the Windows test program (<name>.exe, x64 when configured for x64); engine targets
# compile the same generated C into the GbaPlayer addon.
macro(_agb_wasm_game)
    _agb_find_tool(AGB_WASI_SDK "wasi-sdk-*")
    _agb_find_tool(AGB_WABT "wabt-*")
    set(WASM_DIR "${CMAKE_BINARY_DIR}/wasm")
    set(WASM_CC "${AGB_WASI_SDK}/bin/clang${CMAKE_HOST_EXECUTABLE_SUFFIX}")
    set(WASM_LD "${AGB_WASI_SDK}/bin/wasm-ld${CMAKE_HOST_EXECUTABLE_SUFFIX}")
    set(WASI_LIB "${AGB_WASI_SDK}/share/wasi-sysroot/lib/wasm32-wasip1")
    file(GLOB WASM_RT_BUILTINS "${AGB_WASI_SDK}/lib/clang/*/lib/wasm32-unknown-wasip1/libclang_rt.builtins.a")
    set(WASM2C "${AGB_WABT}/bin/wasm2c${CMAKE_HOST_EXECUTABLE_SUFFIX}")
    # data units are assembled as i686 ELF first (full GNU-as) and translated (elf2wasm.py)
    set(ELF_AS "${CMAKE_C_COMPILER}" --target=i686-unknown-linux-gnu)

    set(include_args "")
    foreach(dir IN ITEMS "${RT}/include" "${RT}/compat/libc" ${G_INCLUDES})
        list(APPEND include_args "-I${dir}")
    endforeach()
    set(define_args "")
    foreach(def IN ITEMS ${G_DEFINES} PORT=1 NONMATCHING=1)
        list(APPEND define_args "-D${def}")
    endforeach()
    set(alias_list "${WASM_DIR}/rom_aliases.txt")
    set(game_cflags --target=wasm32-wasip1 -O2 -g0 -std=gnu99 -ffreestanding -nostdlibinc -fno-builtin -fno-strict-aliasing
        -fwrapv -funsigned-char -Wno-everything ${define_args} ${include_args}
        -include "${RT}/compat/agb_pret.h")
    if(G_PRELUDE)
        list(APPEND game_cflags -include "${G_PRELUDE}")
    endif()
    set(core_cflags --target=wasm32-wasip1 -O2 -g0 -mllvm -wasm-enable-sjlj -mexception-handling
        "-I${RT}/include" "-I${RT}/src")

    # GBA memory regions + ROM aliases (labels added to the data units by elf2wasm.py:
    # wasm cannot alias undefined symbols)
    set(mem_s "${WASM_DIR}/agb_memory.s")
    set(mem_o "${WASM_DIR}/agb_memory.o")
    add_custom_command(OUTPUT "${mem_o}" "${alias_list}"
        COMMAND "${PY}" "${TOOLS}/gen_memory.py" --ldscript "${G_LDSCRIPT}" ${sym_args} ${rom_args}
                --labels "@${label_list}" --wasm --alias-list "${alias_list}" --out "${mem_s}"
        COMMAND "${WASM_CC}" --target=wasm32 -c "${mem_s}" -o "${mem_o}"
        DEPENDS "${TOOLS}/gen_memory.py" "${G_LDSCRIPT}" "${label_list}"
        COMMENT "gen_memory (wasm)" VERBATIM)

    set(objs "${mem_o}")
    set(index 0)
    # game C (decomp sources, glue, m4a) with the game's headers
    foreach(src IN LISTS C_UNITS G_EXTRA_SOURCES ITEMS "${RT}/src/agb_m4a.c")
        get_filename_component(stem "${src}" NAME_WE)
        set(obj "${WASM_DIR}/obj/${index}_${stem}.o")
        set(extra "")
        string(FIND "${src}" "${GEN_DIR}/patched/" at)
        if(at EQUAL 0)
            # patched copy: keep the original directory for "local.h" includes
            string(REPLACE "${GEN_DIR}/patched/" "${G_DECOMP}/" original "${src}")
            get_filename_component(original_dir "${original}" DIRECTORY)
            set(extra "-I${original_dir}")
        endif()
        add_custom_command(OUTPUT "${obj}"
            COMMAND "${WASM_CC}" ${game_cflags} ${extra} -c "${src}" -o "${obj}"
            DEPENDS "${src}" "${RT}/compat/agb_pret.h" ${G_PRELUDE}
            COMMENT "wasm ${stem}.c" VERBATIM)
        list(APPEND objs "${obj}")
        math(EXPR index "${index} + 1")
    endforeach()
    # portable runtime core
    file(GLOB core_sources CONFIGURE_DEPENDS "${RT}/src/*.c")
    list(REMOVE_ITEM core_sources "${RT}/src/agb_m4a.c")
    foreach(src IN LISTS core_sources)
        get_filename_component(stem "${src}" NAME_WE)
        set(obj "${WASM_DIR}/obj/core_${stem}.o")
        add_custom_command(OUTPUT "${obj}"
            COMMAND "${WASM_CC}" ${core_cflags} -c "${src}" -o "${obj}"
            DEPENDS "${src}"
            COMMENT "wasm ${stem}.c" VERBATIM)
        list(APPEND objs "${obj}")
    endforeach()
    # signatures of the code's functions: data units that store function addresses
    # (handler tables) need them to emit function-table indices
    set(code_objs ${objs})
    list(REMOVE_ITEM code_objs "${mem_o}")
    set(code_list "${WASM_DIR}/code_objects.txt")
    string(REPLACE ";" "\n" code_text "${code_objs}")
    file(WRITE "${code_list}.new" "${code_text}\n")
    file(COPY_FILE "${code_list}.new" "${code_list}" ONLY_IF_DIFFERENT)
    set(functypes "${WASM_DIR}/functypes.txt")
    add_custom_command(OUTPUT "${functypes}"
        COMMAND "${PY}" "${TOOLS}/wasm_functypes.py" "${functypes}" "--inputs-from=${code_list}"
        DEPENDS ${code_objs} "${TOOLS}/wasm_functypes.py"
        COMMENT "wasm function signatures" VERBATIM)
    # data units: asm_port -> i686 ELF object -> elf2wasm -> wasm object
    foreach(src IN LISTS S_UNITS)
        file(RELATIVE_PATH rel "${G_DECOMP}" "${src}")
        string(REGEX REPLACE "[/\\\\.]" "_" stem "${rel}")
        set(base "${WASM_DIR}/data/${stem}")
        add_custom_command(OUTPUT "${base}.o"
            COMMAND "${PY}" "${TOOLS}/asm_port.py" "${G_DECOMP}" "${src}" "${base}.elf.s"
            COMMAND ${ELF_AS} -c "${base}.elf.s" -o "${base}.elf.o"
            COMMAND "${PY}" "${TOOLS}/elf2wasm.py" "${base}.elf.o" "${base}.s" "${stem}" "${alias_list}" "${functypes}"
            COMMAND "${WASM_CC}" --target=wasm32 -c "${base}.s" -o "${base}.o"
            DEPENDS "${src}" "${TOOLS}/asm_port.py" "${TOOLS}/elf2wasm.py" "${alias_list}" "${functypes}"
            COMMENT "wasm data ${rel}" VERBATIM)
        list(APPEND objs "${base}.o")
    endforeach()

    math(EXPR memory_bytes "${AGB_WASM_MEMORY_MB} * 1048576")
    set(wasm "${WASM_DIR}/${G_NAME}.wasm")
    # the inputs go through a list file (cmd.exe limits a command to 8191 characters)
    set(inputs_list "${WASM_DIR}/link_inputs.txt")
    string(REPLACE ";" "\n" inputs_text "${objs};${WASI_LIB}/libsetjmp.a;${WASI_LIB}/libc.a;${WASM_RT_BUILTINS}")
    file(WRITE "${inputs_list}.new" "${inputs_text}\n")
    file(COPY_FILE "${inputs_list}.new" "${inputs_list}" ONLY_IF_DIFFERENT)
    add_custom_command(OUTPUT "${wasm}"
        COMMAND "${PY}" "${TOOLS}/wasm_link.py" "${WASM_LD}" --no-entry --export=agb_game_entry --export=__heap_base
            "--allow-undefined-file=${RT}/wasm/imports.txt" --no-stack-first --global-base=4096 -z stack-size=1048576
            "--initial-memory=${memory_bytes}" "--max-memory=${memory_bytes}" --error-limit=0
            "--inputs-from=${inputs_list}" -o "${wasm}"
        DEPENDS ${objs} "${RT}/wasm/imports.txt" "${TOOLS}/wasm_link.py"
        COMMENT "wasm-ld ${G_NAME}.wasm" VERBATIM)

    set(W2C_DIR "${WASM_DIR}/c")
    set(w2c_count 8)
    set(w2c_c "")
    math(EXPR last "${w2c_count} - 1")
    foreach(i RANGE ${last})
        list(APPEND w2c_c "${W2C_DIR}/agb_guest_${i}.c")
    endforeach()
    set(data_file "${CMAKE_BINARY_DIR}/${G_NAME}.agbdata")
    add_custom_command(OUTPUT ${w2c_c} "${W2C_DIR}/agb_guest.h" "${W2C_DIR}/agbw_config.h" "${data_file}"
        COMMAND "${PY}" "${TOOLS}/wasm_to_c.py" "${WASM2C}" "${wasm}" "${W2C_DIR}" agb_guest ${w2c_count} "${data_file}"
        DEPENDS "${wasm}" "${TOOLS}/wasm_to_c.py"
        COMMENT "wasm2c ${G_NAME}.wasm" VERBATIM)

    # The game package gets the translated game as addon sources + the data in Assets/
    # (publish_guest.py), so the engine compiles it for every target.
    if(G_PACKAGE_DIR AND G_PACKAGE)
        get_filename_component(package_dir "${G_PACKAGE_DIR}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        add_custom_target(${G_NAME}_publish_guest ALL
            COMMAND "${PY}" "${TOOLS}/publish_guest.py" --package-dir "${package_dir}" --package "${G_PACKAGE}"
                --name "${G_NAME}" --memory "${memory_bytes}" --runtime "${RT}" --gba-source "${RT}/../Source"
                --wabt-include "${AGB_WABT}/include" --w2c-dir "${W2C_DIR}" --data "${data_file}"
            DEPENDS ${w2c_c} "${data_file}" "${TOOLS}/publish_guest.py"
            COMMENT "publish ${G_PACKAGE} guest" VERBATIM)
    endif()

    # Windows test host: platform host + wasm2c runtime/guest + the translated game.
    file(GLOB host_sources CONFIGURE_DEPENDS "${RT}/host/win32/*.c" "${RT}/wasm/*.c")
    list(REMOVE_ITEM host_sources "${RT}/wasm/agbw_bridge.c")  # the engine's in-process host
    add_executable(${G_NAME} ${host_sources} ${w2c_c})
    target_include_directories(${G_NAME} PRIVATE "${RT}/include" "${RT}/wasm" "${W2C_DIR}" "${AGB_WABT}/include")
    target_compile_definitions(${G_NAME} PRIVATE "AGB_GAME_TITLE=\"${G_TITLE}\"" "AGB_GAME_NAME=\"${G_NAME}\""
        _CRT_SECURE_NO_WARNINGS WASM_RT_USE_MMAP=0 WASM_RT_MEMCHECK_BOUNDS_CHECK=1
        WASM_RT_NONCONFORMING_UNCHECKED_STACK_EXHAUSTION=1 WASM_RT_SKIP_SIGNAL_RECOVERY=1)
    target_compile_options(${G_NAME} PRIVATE -O2 -g -gcodeview -w)
    if(AGB_WASM_ROM_CACHE_KB)
        # test the paged read-only data of the small consoles (agbw.h) on the PC
        math(EXPR cache_bytes "${AGB_WASM_ROM_CACHE_KB} * 1024")
        target_compile_definitions(${G_NAME} PRIVATE AGBW_ROM_CACHE_BYTES=${cache_bytes}u)
    endif()
    if(AGB_WASM_WATCH)
        # store watchpoint for debugging (AGBW_WATCH=lo-hi at run time, see agbw_mem_ops.h)
        target_compile_definitions(${G_NAME} PRIVATE AGBW_WATCH=1)
    endif()
    target_link_libraries(${G_NAME} PRIVATE kernel32 user32 gdi32 winmm dbghelp xinput)
    target_link_options(${G_NAME} PRIVATE -fuse-ld=lld -Wl,/debug -Wl,/STACK:0x1000000)
endmacro()

function(agb_add_game)
    cmake_parse_arguments(G "" "NAME;TITLE;DECOMP;UNITS_JSON;LDSCRIPT;ROM;EXCLUDE_REGEX;PRELUDE;PUBLISH_DIR;PACKAGE;PACKAGE_DIR"
        "SYMS;DEFINES;INCLUDES;PATCHES;EXTRA_SOURCES" ${ARGN})
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_property(langs GLOBAL PROPERTY ENABLED_LANGUAGES)
    if(NOT "ASM" IN_LIST langs)
        message(FATAL_ERROR "agb_add_game: declare the project with C and ASM: project(<name> C ASM)")
    endif()
    get_filename_component(G_DECOMP "${G_DECOMP}" ABSOLUTE)
    set(RT "${AGB_RUNTIME_DIR}")
    set(TOOLS "${RT}/tools")
    set(GEN_DIR "${CMAKE_BINARY_DIR}/gen")
    file(MAKE_DIRECTORY "${GEN_DIR}")
    set(PY "${Python3_EXECUTABLE}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${G_UNITS_JSON}" "${G_LDSCRIPT}")

    # ---- game sources ---------------------------------------------------------------
    _agb_list(C_UNITS "${PY}" "${TOOLS}/list_units.py" "${G_UNITS_JSON}" "${G_DECOMP}" c "${G_EXCLUDE_REGEX}")
    _agb_list(S_UNITS "${PY}" "${TOOLS}/list_units.py" "${G_UNITS_JSON}" "${G_DECOMP}" s "${G_EXCLUDE_REGEX}")

    if(G_PATCHES)
        # "original|patched-copy" pairs; the checkout stays untouched.
        _agb_list(map "${PY}" "${TOOLS}/apply_patches.py" "${G_DECOMP}" "${GEN_DIR}/patched" ${G_PATCHES} ${C_UNITS})
        foreach(pair IN LISTS map)
            string(REPLACE "|" ";" pair_list "${pair}")
            list(GET pair_list 0 original)
            list(GET pair_list 1 generated)
            list(TRANSFORM C_UNITS REPLACE "^${original}$" "${generated}")
            # the copy lives elsewhere: keep the original directory for "local.h" includes
            get_filename_component(original_dir "${original}" DIRECTORY)
            set_source_files_properties("${generated}" PROPERTIES COMPILE_OPTIONS "-I${original_dir}")
            set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${original}")
        endforeach()
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${G_PATCHES})
    endif()

    # Data units: ARM GNU-as -> host GNU-as (build step, so edits to the decomp propagate).
    set(ASM_OUT "")
    foreach(src IN LISTS S_UNITS)
        file(RELATIVE_PATH rel "${G_DECOMP}" "${src}")
        string(REGEX REPLACE "[/\\\\.]" "_" stem "${rel}")
        set(out "${GEN_DIR}/asm/${stem}.s")
        add_custom_command(OUTPUT "${out}"
            COMMAND "${PY}" "${TOOLS}/asm_port.py" "${G_DECOMP}" "${src}" "${out}" --underscore
            DEPENDS "${src}" "${TOOLS}/asm_port.py"
            COMMENT "asm_port ${rel}" VERBATIM)
        list(APPEND ASM_OUT "${out}")
    endforeach()
    set(label_list "${GEN_DIR}/data_units.txt")
    string(REPLACE ";" "\n" label_text "${S_UNITS}")
    file(WRITE "${label_list}.new" "${label_text}\n")
    file(COPY_FILE "${label_list}.new" "${label_list}" ONLY_IF_DIFFERENT)

    # GBA memory regions with the decomp's absolute-address symbols inside them.
    set(sym_args "")
    foreach(s IN LISTS G_SYMS)
        list(APPEND sym_args --sym "${s}")
    endforeach()
    set(rom_args "")
    if(G_ROM)
        get_filename_component(rom "${G_ROM}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
        set(rom_args --rom "${rom}")
    endif()

    if(AGB_GUEST STREQUAL "wasm")
        _agb_wasm_game()
        return()
    endif()

    add_custom_command(OUTPUT "${GEN_DIR}/agb_memory.s"
        COMMAND "${PY}" "${TOOLS}/gen_memory.py" --ldscript "${G_LDSCRIPT}" ${sym_args} ${rom_args}
                --labels "@${label_list}" --underscore --out "${GEN_DIR}/agb_memory.s"
        DEPENDS "${TOOLS}/gen_memory.py" "${G_LDSCRIPT}" "${label_list}"
        COMMENT "gen_memory" VERBATIM)

    # ---- targets ------------------------------------------------------------------------
    # Game code: decomp sources + game glue + the runtime parts that use game headers.
    set(game "${G_NAME}_game")
    add_library(${game} OBJECT ${C_UNITS} ${G_EXTRA_SOURCES} "${RT}/src/agb_m4a.c")
    target_compile_definitions(${game} PRIVATE ${G_DEFINES} PORT=1 NONMATCHING=1)
    target_include_directories(${game} BEFORE PRIVATE "${RT}/include" "${RT}/compat/libc" ${G_INCLUDES})
    # The GNU target gives GCC (ARM EABI-compatible) struct layout for bitfields. NULL and
    # low addresses are readable on the GBA (BIOS area) and games read through them: keep
    # those accesses (the host's safety net serves them) instead of letting the optimiser
    # treat them as unreachable.
    set(prelude "SHELL:-include \"${RT}/compat/agb_pret.h\"")
    if(G_PRELUDE)
        list(APPEND prelude "SHELL:-include \"${G_PRELUDE}\"")
    endif()
    target_compile_options(${game} PRIVATE --target=i686-pc-windows-gnu -mno-ms-bitfields
        -mno-stack-arg-probe -fno-delete-null-pointer-checks -std=gnu99 -ffreestanding -nostdlibinc
        -fno-builtin -fno-strict-aliasing -fwrapv -funsigned-char -Wno-everything -g -gcodeview
        -fno-omit-frame-pointer ${prelude})
    option(AGB_GAME_OPTIMIZE "Optimise the game code" ON)
    if(AGB_GAME_OPTIMIZE)
        target_compile_options(${game} PRIVATE -O2)
    else()
        target_compile_options(${game} PRIVATE -O0)
    endif()

    # Data units.
    set(data "${G_NAME}_data")
    add_library(${data} OBJECT ${ASM_OUT} "${GEN_DIR}/agb_memory.s")
    target_compile_options(${data} PRIVATE --target=i686-pc-windows-gnu)

    # Runtime core (hardware) and host: plain C with the CRT, no game headers.
    file(GLOB CORE_SOURCES CONFIGURE_DEPENDS "${RT}/src/*.c")
    list(REMOVE_ITEM CORE_SOURCES "${RT}/src/agb_m4a.c")
    file(GLOB HOST_SOURCES CONFIGURE_DEPENDS "${RT}/host/win32/*.c")
    set(host "${G_NAME}_host")
    add_library(${host} OBJECT ${CORE_SOURCES} ${HOST_SOURCES})
    target_include_directories(${host} PRIVATE "${RT}/include" "${RT}/src")
    target_compile_definitions(${host} PRIVATE "AGB_GAME_TITLE=\"${G_TITLE}\"" "AGB_GAME_NAME=\"${G_NAME}\""
        _CRT_SECURE_NO_WARNINGS)
    target_compile_options(${host} PRIVATE -O2 -g -gcodeview -Wall -Wno-unused-function)

    add_executable(${G_NAME} $<TARGET_OBJECTS:${game}> $<TARGET_OBJECTS:${data}> $<TARGET_OBJECTS:${host}>)
    target_link_libraries(${G_NAME} PRIVATE kernel32 user32 gdi32 winmm dbghelp xinput)
    target_link_options(${G_NAME} PRIVATE -fuse-ld=lld -Wl,/LARGEADDRESSAWARE -Wl,/STACK:0x800000 -Wl,/SAFESEH:NO
        -Wl,/debug -Wl,/errorlimit:0
        # The GBA has no memory protection: ROM data is read-only there only by convention.
        "SHELL:-Xlinker /SECTION:.rdata,RW")

    option(AGB_PUBLISH "Copy the built program to the game's PUBLISH_DIR" ON)
    if(G_PUBLISH_DIR AND AGB_PUBLISH)
        add_custom_command(TARGET ${G_NAME} POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${G_PUBLISH_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${G_NAME}>" "${G_PUBLISH_DIR}"
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_PDB_FILE:${G_NAME}>" "${G_PUBLISH_DIR}"
            VERBATIM)
    endif()
endfunction()
