include_guard(GLOBAL)

if (WINDOWS_STORE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(MELEE_XBOX_GCC "" CACHE FILEPATH "MinGW x64 GCC for the Melee game C sources")
    if (NOT MELEE_XBOX_GCC)
        unset(MELEE_XBOX_GCC CACHE)
        find_program(MELEE_XBOX_GCC NAMES x86_64-w64-mingw32-gcc gcc
                PATHS C:/msys64/mingw64/bin NO_DEFAULT_PATH)
    endif ()
    if (NOT MELEE_XBOX_GCC)
        message(FATAL_ERROR "Xbox UWP needs MinGW x64 GCC for game disc structures")
    endif ()
    execute_process(COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_LIST_DIR}/../platforms/xbox/gcc_bridge.py"
            --gcc "${MELEE_XBOX_GCC}" --probe
            RESULT_VARIABLE _melee_xbox_gcc_probe
            OUTPUT_VARIABLE _melee_xbox_gcc_stdout
            ERROR_VARIABLE _melee_xbox_gcc_stderr)
    if (NOT _melee_xbox_gcc_probe EQUAL 0)
        message(FATAL_ERROR "Xbox GCC bridge cannot compile scalar_storage_order to COFF: ${_melee_xbox_gcc_probe}\n${_melee_xbox_gcc_stdout}\n${_melee_xbox_gcc_stderr}")
    endif ()
    return()
endif ()

if (EMSCRIPTEN OR ANDROID OR APPLE OR (WIN32 AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$"))
    return()
endif ()

set(_melee_toolchain_fix
    "  fix:      CC=gcc CXX=g++ cmake -B build -G Ninja   (delete the stale cache first)\n"
    "            python3 tools/preflight.py                # checks the whole toolchain\n"
    "            docs/building.md lists the supported versions.")

if (NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
    message(FATAL_ERROR
        "melee-pc requires GCC as the C compiler.\n"
        "  found:    ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} (${CMAKE_C_COMPILER})\n"
        "  required: GNU (GCC 12 or newer)\n"
        "  why:      the decomp's on-disc structs use\n"
        "            __attribute__((scalar_storage_order(\"big-endian\"))), which no other\n"
        "            compiler implements; Clang ignores it and every disc field comes\n"
        "            back byte-swapped at runtime.\n"
        ${_melee_toolchain_fix})
endif ()

# A GCC built without the attribute (or with it warning-only) would otherwise
# produce thousands of -Wattributes warnings and wrong data at runtime, so
# prove it works instead of trusting the compiler id. -Werror turns the
# "attribute ignored" warning into the configure failure it deserves to be.
include(CheckCSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-Werror")
check_c_source_compiles([[
struct __attribute__((scalar_storage_order("big-endian"))) S { int v; };
int main(void) { struct S s = { 1 }; return s.v - 1; }
]] MELEE_HAVE_SCALAR_STORAGE_ORDER)
unset(CMAKE_REQUIRED_FLAGS)

if (NOT MELEE_HAVE_SCALAR_STORAGE_ORDER)
    message(FATAL_ERROR
        "the selected C compiler rejects scalar_storage_order.\n"
        "  found:    ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION} (${CMAKE_C_COMPILER})\n"
        "  required: a GCC that accepts\n"
        "            __attribute__((scalar_storage_order(\"big-endian\"))) without warning\n"
        "  why:      every on-disc struct depends on it; without it the game reads\n"
        "            byte-swapped data and crashes in unrelated places.\n"
        ${_melee_toolchain_fix})
endif ()

unset(_melee_toolchain_fix)
