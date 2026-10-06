# The WebAssembly (Emscripten) build. runtime/CMakeLists.txt includes this when MKW_PLATFORM_WEB,
# before the vendored libraries are added: the compile options below are directory-wide, because
# wasm-ld rejects objects built with different exception-handling models and the ISA headers need
# SIMD128. Web-only sources live in runtime/web/; see docs/wasm-port.md.

set(MKW_WEB_FMA "relaxed" CACHE STRING
    "WebAssembly FMA: relaxed (relaxed-SIMD madd, verified fused at startup) or soft (musl fma: exact, slow)")
set_property(CACHE MKW_WEB_FMA PROPERTY STRINGS relaxed soft)
add_compile_options(-fwasm-exceptions -msimd128)
add_link_options(-fwasm-exceptions)
# Everything links statically into one module; aurora's extern/ only builds static
# third-party libraries when BUILD_SHARED_LIBS is defined.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# Guest RAM is allocated at startup (web/guest_flat_memory_wasm.cpp) on top of the runtime and
# aurora; let the heap grow on demand instead of guessing a fixed size.
add_link_options(-sALLOW_MEMORY_GROWTH=1)
if(MKW_WEB_FMA STREQUAL "relaxed")
    add_compile_options(-mrelaxed-simd)
elseif(MKW_WEB_FMA STREQUAL "soft")
    add_compile_definitions(MKW_WASM_SOFT_FMA)
else()
    message(FATAL_ERROR "MKW_WEB_FMA must be relaxed or soft, not '${MKW_WEB_FMA}'")
endif()

get_filename_component(MKW_WEB_RUNTIME_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(MKW_WEB_DIR "${MKW_WEB_RUNTIME_DIR}/web")
# Guest threads run as JSPI coroutines (web/host_context_jspi.cpp and .js).
set(MKW_WEB_JSPI_LIBRARY "${MKW_WEB_DIR}/host_context_jspi.js")
# The DATA directory mount, the page loader, the save unlocker and the HTML shell.
set(MKW_WEB_DISC_LIBRARY "${MKW_WEB_DIR}/disc_fs.js")
set(MKW_WEB_LOADER "${MKW_WEB_DIR}/loader.js")
set(MKW_WEB_UNLOCK "${MKW_WEB_DIR}/unlock.js")
set(MKW_WEB_SHELL "${MKW_WEB_DIR}/shell.html")

function(mkw_web_link_jspi target)
    target_link_options(${target} PRIVATE
        -sJSPI -sJSPI_EXPORTS=mkw_jspi_context_entry "SHELL:--js-library ${MKW_WEB_JSPI_LIBRARY}")
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${MKW_WEB_JSPI_LIBRARY}")
endfunction()

# Web counterparts of what a browser cannot do natively: no fixed-address reservation, page
# protection, Discord IPC or native stack switching.
macro(mkw_web_select_sources sources)
    list(REMOVE_ITEM ${sources}
        "${MKW_WEB_RUNTIME_DIR}/src/guest_flat_memory.cpp"
        "${MKW_WEB_RUNTIME_DIR}/src/guest_flat_memory_macos.cpp"
        "${MKW_WEB_RUNTIME_DIR}/src/discord_presence.cpp"
        "${MKW_WEB_RUNTIME_DIR}/src/host_context.cpp")
    file(GLOB _mkw_web_sources CONFIGURE_DEPENDS "${MKW_WEB_DIR}/*.cpp")
    list(APPEND ${sources} ${_mkw_web_sources})
endmacro()

function(mkw_web_add_tests)
    # Run through Node like every other web test (Emscripten's toolchain sets the emulator);
    # JSPI is still behind a flag in Node 24.
    foreach(test host_context_tests host_context_stress_tests)
        add_executable(mkw_web_${test}
            "${MKW_WEB_RUNTIME_DIR}/tests/${test}.cpp"
            "${MKW_WEB_DIR}/host_context_jspi.cpp")
        target_include_directories(mkw_web_${test} PRIVATE "${MKW_WEB_RUNTIME_DIR}/include")
        target_compile_features(mkw_web_${test} PRIVATE cxx_std_17)
        target_link_options(mkw_web_${test} PRIVATE -sEXIT_RUNTIME=1 -sSTACK_SIZE=1MB)
        mkw_web_link_jspi(mkw_web_${test})
        add_test(NAME mkw_web_${test}
            COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} --experimental-wasm-jspi $<TARGET_FILE:mkw_web_${test}>)
    endforeach()

    # Browser smoke test for the graphics stack: aurora's own example (clears the screen through
    # GX) on emdawnwebgpu, single-threaded, yielding to the browser every frame through JSPI.
    # Serve the build directory over HTTP and open mkw_web_aurora_smoke.html.
    add_executable(mkw_web_aurora_smoke "${MKW_AURORA_DIR}/examples/simple.c")
    # This fork's GX headers overload GXSetArray under TARGET_PC, which is C++ only.
    set_source_files_properties("${MKW_AURORA_DIR}/examples/simple.c" PROPERTIES LANGUAGE CXX)
    target_link_libraries(mkw_web_aurora_smoke PRIVATE aurora::core aurora::gx aurora::main aurora::vi)
    target_link_options(mkw_web_aurora_smoke PRIVATE -sJSPI -sSTACK_SIZE=1MB)
    set_target_properties(mkw_web_aurora_smoke PROPERTIES SUFFIX ".html" EXCLUDE_FROM_ALL TRUE)

    # The DATA directory mount (web/disc_fs.js) through the runtime's own read path.
    add_executable(mkw_web_disc_fs_tests "${MKW_WEB_RUNTIME_DIR}/tests/web_disc_fs_tests.cpp")
    target_include_directories(mkw_web_disc_fs_tests PRIVATE "${MKW_WEB_RUNTIME_DIR}/include")
    target_compile_features(mkw_web_disc_fs_tests PRIVATE cxx_std_17)
    target_link_options(mkw_web_disc_fs_tests PRIVATE -sJSPI -sEXIT_RUNTIME=1 -sFORCE_FILESYSTEM=1
        "SHELL:--js-library ${MKW_WEB_DISC_LIBRARY}"
        "SHELL:--pre-js ${MKW_WEB_RUNTIME_DIR}/tests/web_disc_fs_tests.pre.js")
    set_property(TARGET mkw_web_disc_fs_tests APPEND PROPERTY LINK_DEPENDS
        "${MKW_WEB_DISC_LIBRARY}" "${MKW_WEB_RUNTIME_DIR}/tests/web_disc_fs_tests.pre.js")
    add_test(NAME mkw_web_disc_fs_tests
        COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR} --experimental-wasm-jspi $<TARGET_FILE:mkw_web_disc_fs_tests>)

    # Page loader in a real browser (no GPU needed): serve with web/serve.py --data
    # <DATA dir> and open mkw_web_loader_smoke.html?data=data.
    add_executable(mkw_web_loader_smoke "${MKW_WEB_RUNTIME_DIR}/tests/web_loader_smoke.cpp")
    target_include_directories(mkw_web_loader_smoke PRIVATE "${MKW_WEB_RUNTIME_DIR}/include")
    target_compile_features(mkw_web_loader_smoke PRIVATE cxx_std_17)
    target_link_options(mkw_web_loader_smoke PRIVATE -sJSPI -sFORCE_FILESYSTEM=1 -lidbfs.js
        "SHELL:--js-library ${MKW_WEB_DISC_LIBRARY}" "SHELL:--pre-js ${MKW_WEB_LOADER}"
        "SHELL:--pre-js ${MKW_WEB_UNLOCK}" "SHELL:--shell-file ${MKW_WEB_SHELL}")
    set_property(TARGET mkw_web_loader_smoke APPEND PROPERTY LINK_DEPENDS
        "${MKW_WEB_DISC_LIBRARY}" "${MKW_WEB_LOADER}" "${MKW_WEB_UNLOCK}" "${MKW_WEB_SHELL}")
    set_target_properties(mkw_web_loader_smoke PROPERTIES SUFFIX ".html" EXCLUDE_FROM_ALL TRUE)

    add_executable(mkw_web_guest_flat_memory_tests
        "${MKW_WEB_RUNTIME_DIR}/tests/wasm_guest_flat_memory_tests.cpp"
        "${MKW_WEB_DIR}/guest_flat_memory_wasm.cpp")
    target_include_directories(mkw_web_guest_flat_memory_tests PRIVATE "${MKW_WEB_RUNTIME_DIR}/include")
    target_compile_features(mkw_web_guest_flat_memory_tests PRIVATE cxx_std_17)
    target_link_options(mkw_web_guest_flat_memory_tests PRIVATE -sEXIT_RUNTIME=1)
    add_test(NAME mkw_web_guest_flat_memory_tests COMMAND mkw_web_guest_flat_memory_tests)
endfunction()

# The compile-only audit (MKW_BUILD_PRODUCTS=OFF): every runtime source compiled for WebAssembly
# against the real aurora and third-party targets.
function(mkw_web_add_compile_audit sources)
    set(MKW_WEB_COMPILE_AUDIT_SOURCES ${sources})
    list(REMOVE_ITEM MKW_WEB_COMPILE_AUDIT_SOURCES
        "${MKW_WEB_RUNTIME_DIR}/src/abi_bridge.cpp"
        "${MKW_WEB_RUNTIME_DIR}/src/hle/os/os_alarm.cpp")
    add_library(mkw_web_compile OBJECT ${MKW_WEB_COMPILE_AUDIT_SOURCES})
    target_include_directories(mkw_web_compile PRIVATE
        "${MKW_WEB_RUNTIME_DIR}/include" "${MKW_WEB_RUNTIME_DIR}/src"
        "${MKW_WEB_RUNTIME_DIR}/.." "${MKW_WEB_RUNTIME_DIR}/../aurora-main/include")
    target_compile_features(mkw_web_compile PRIVATE cxx_std_20)
    target_compile_definitions(mkw_web_compile PRIVATE SDL_MAIN_HANDLED TARGET_PC)
    target_compile_options(mkw_web_compile PRIVATE -O2 -w)
    target_link_libraries(mkw_web_compile PRIVATE
        aurora::gx aurora::pad aurora::si aurora::vi aurora::mtx
        mkw_platform mkw::pugixml mkw::toml11 mkw::cryptopp mkw::mbedtls)
    set_target_properties(mkw_web_compile PROPERTIES UNITY_BUILD OFF)
endfunction()

include("${CMAKE_CURRENT_LIST_DIR}/WebProduct.cmake")
