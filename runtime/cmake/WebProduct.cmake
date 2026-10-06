# The web product (included by Web.cmake). mkw_configure_product calls this instead of laying out
# the native release files.

# The link's -O level is what Binaryen's wasm-opt runs over the whole module.
set(MKW_WEB_LINK_OPT "-O3" CACHE STRING "Web product link optimization: -O2, -O3 (speed), -Os or -Oz (size)")
set_property(CACHE MKW_WEB_LINK_OPT PROPERTY STRINGS -O2 -O3 -Os -Oz)
option(MKW_WEB_PROFILING "Link the web product at -O1 with function names, for profiling and readable stacks" OFF)
if(MKW_WEB_PROFILING)
    set(MKW_WEB_LINK_OPTIMIZATION -O1 --profiling-funcs)
else()
    set(MKW_WEB_LINK_OPTIMIZATION ${MKW_WEB_LINK_OPT})
endif()

# Reading the player's disc image in the page (web/disc_image): nod, the library the native setup
# extracts discs with, built with cargo for wasm32-unknown-emscripten and linked into
# mkw_disc.{js,wasm}, which web/disc_image/disc_worker.js runs in a Web Worker. Without it the page
# still opens an extracted DATA folder or ?data=<url>.
option(MKW_WEB_DISC_IMAGES "Build the web page's disc image reader (needs cargo and the wasm32-unknown-emscripten Rust target)" ON)

function(mkw_add_web_disc_reader target)
    find_program(MKW_CARGO cargo HINTS "$ENV{HOME}/.cargo/bin")
    if(NOT MKW_CARGO)
        message(FATAL_ERROR "MKW_WEB_DISC_IMAGES needs cargo (rustup target add wasm32-unknown-emscripten), "
                            "or configure with -DMKW_WEB_DISC_IMAGES=OFF")
    endif()
    set(crate "${MKW_WEB_DIR}/disc_image")
    set(cargo_dir "${CMAKE_CURRENT_BINARY_DIR}/mkw_disc-cargo")
    set(library "${cargo_dir}/wasm32-unknown-emscripten/release/libmkw_disc.a")
    add_custom_command(
        OUTPUT "${library}"
        COMMAND ${CMAKE_COMMAND} -E env
            "CARGO_TARGET_DIR=${cargo_dir}"
            "CC_wasm32_unknown_emscripten=${CMAKE_C_COMPILER}"
            "AR_wasm32_unknown_emscripten=${CMAKE_AR}"
            ${MKW_CARGO} build --release --locked --target wasm32-unknown-emscripten
                --manifest-path "${crate}/Cargo.toml"
        DEPENDS "${crate}/Cargo.toml" "${crate}/Cargo.lock" "${crate}/src/lib.rs"
        COMMENT "Building the disc image reader (nod) for WebAssembly"
        VERBATIM)
    set(exports _mkw_disc_open _mkw_disc_error _mkw_disc_listing _mkw_disc_sys _mkw_disc_read _malloc _free)
    list(JOIN exports "," exports)
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/mkw_disc.js" "${CMAKE_CURRENT_BINARY_DIR}/mkw_disc.wasm"
               "${CMAKE_CURRENT_BINARY_DIR}/disc_worker.js"
        COMMAND ${CMAKE_C_COMPILER} "${library}" -o "${CMAKE_CURRENT_BINARY_DIR}/mkw_disc.js" -O3
            -sMODULARIZE -sEXPORT_NAME=createMkwDisc -sENVIRONMENT=worker -sALLOW_MEMORY_GROWTH
            "-sEXPORTED_FUNCTIONS=${exports}" -sEXPORTED_RUNTIME_METHODS=HEAPU8,HEAPU32
            --js-library "${crate}/disc_source.js"
        COMMAND ${CMAKE_COMMAND} -E copy "${crate}/disc_worker.js" "${CMAKE_CURRENT_BINARY_DIR}/disc_worker.js"
        DEPENDS "${library}" "${crate}/disc_source.js" "${crate}/disc_worker.js"
        COMMENT "Linking the disc image reader worker"
        VERBATIM)
    add_custom_target(${target}_disc_reader ALL DEPENDS
        "${CMAKE_CURRENT_BINARY_DIR}/mkw_disc.js" "${CMAKE_CURRENT_BINARY_DIR}/disc_worker.js")
    add_dependencies(${target} ${target}_disc_reader)
endfunction()

# The web build is one page (<target>.html/.js/.wasm/.data). The release layout's files beside the
# executable live in Emscripten's virtual file system instead: preloaded under /app (the web
# ExecutableDirectory()), and aurora's bundled pipeline cache at its default resources path "/".
# Game data and saves are mounted by the page (web/loader.js).
function(mkw_configure_web_product target)
    set(assets "${MKW_RUNTIME_SOURCE_DIR}/assets")
    file(SHA256 "${assets}/dsp/dsp_coef.bin" dsp_sha256)
    if(NOT dsp_sha256 STREQUAL "d7741279c2e8ec5c5fb318f8fbdd6de6bf583520d288e836a5383233a4238179")
        message(FATAL_ERROR "Wii DSP coefficient ROM hash mismatch: ${dsp_sha256}")
    endif()
    foreach(asset "${assets}/wii/shared2" "${assets}/dsp/dsp_coef.bin" "${assets}/certs/cacert.pem"
                  "${assets}/pipeline/initial_pipeline_cache.db" "${assets}/pipeline/web_pipeline_cache.db")
        if(NOT EXISTS "${asset}")
            message(FATAL_ERROR "Missing runtime asset: ${asset}")
        endif()
    endforeach()
    mkw_web_link_jspi(${target})
    target_link_options(${target} PRIVATE
        "SHELL:--js-library ${MKW_WEB_DISC_LIBRARY}"
        "SHELL:--pre-js ${MKW_WEB_LOADER}"
        "SHELL:--pre-js ${MKW_WEB_UNLOCK}"
        "SHELL:--shell-file ${MKW_WEB_SHELL}"
        -lidbfs.js -sFORCE_FILESYSTEM=1 -sENVIRONMENT=web
        "SHELL:--preload-file ${assets}/wii@/app/wii_bootstrap"
        "SHELL:--preload-file ${assets}/dsp/dsp_coef.bin@/app/dsp_coef.bin"
        "SHELL:--preload-file ${assets}/certs/cacert.pem@/app/cacert.pem"
        # Two pipeline seeds; the loader copies one to /initial_pipeline_cache.db (web/loader.js):
        # the native one, and the web one (the native one plus what the web build learned in play,
        # web/pipeline_seed.py). aurora compiles every seeded pipeline at startup.
        "SHELL:--preload-file ${assets}/pipeline/initial_pipeline_cache.db@/app/pipeline/native.db"
        "SHELL:--preload-file ${assets}/pipeline/web_pipeline_cache.db@/app/pipeline/web.db"
        # The native main thread has 8 MiB of stack; Emscripten's default is 64 KiB.
        -sSTACK_SIZE=8MB -sINITIAL_MEMORY=256MB -sMAXIMUM_MEMORY=4GB
        # Every translated symbol is declared as void(CpuContext*) by the registration and
        # dispatch TUs; wasm-ld only warns on a mismatched definition and links a trapping stub.
        -Wl,--fatal-warnings
        ${MKW_WEB_LINK_OPTIMIZATION})
    set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS
        "${MKW_WEB_DISC_LIBRARY}" "${MKW_WEB_LOADER}" "${MKW_WEB_UNLOCK}" "${MKW_WEB_SHELL}")
    set_target_properties(${target} PROPERTIES SUFFIX ".html")
    if(MKW_WEB_DISC_IMAGES)
        mkw_add_web_disc_reader(${target})
    endif()
endfunction()
