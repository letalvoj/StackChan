cmake_minimum_required(VERSION 3.16)
project(stack-chan-wasm C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
# -Wno-unused-private-field: upstream xiaozhi settings.h keeps fields the WASM build
# never reads. It lives in a non-SYSTEM include dir (our application_core.h is its
# neighbour), so it cannot be silenced by include classification alone.
add_compile_options(-O2 -Wall -Wno-missing-field-initializers -Wno-format -Wno-mismatched-tags -Wno-unused-lambda-capture -Wno-unused-variable -Wno-unused-function -Wno-reorder-ctor -Wno-sign-compare -Wno-c++20-designator -Wno-delete-non-abstract-non-virtual-dtor -Wno-implicit-function-declaration -Wno-unused-private-field)

add_definitions(
    -DLV_CONF_INCLUDE_SIMPLE=1
    -DCONFIG_IDF_TARGET_WASM=1
    -DFIRMWARE_VERSION="1.4.3-wasm"
    -DCONFIG_STACKCHAN_FIRMWARE=1
    -DBOARD_TYPE="wasm-emulator"
    -DBOARD_NAME="wasm-emulator"
)

set(FW_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../firmware")
set(LVGL_DIR "${FW_DIR}/managed_components/lvgl__lvgl")

file(GLOB_RECURSE LVGL_SOURCES "${LVGL_DIR}/src/*.c")
add_library(lvgl STATIC ${LVGL_SOURCES})
# SYSTEM: LVGL is vendored, so its diagnostics are not actionable for us and would
# otherwise drown out warnings in our own code.
target_include_directories(lvgl SYSTEM PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/shims"
    "${LVGL_DIR}"
    "${LVGL_DIR}/src"
)

file(GLOB_RECURSE MOONCAKE_SRCS "${FW_DIR}/components/mooncake/src/*.cpp" "${FW_DIR}/components/mooncake/src/*.cc" "${FW_DIR}/components/mooncake/src/*.c")
file(GLOB_RECURSE MCLOG_SRCS "${FW_DIR}/components/mooncake_log/src/*.cpp" "${FW_DIR}/components/mooncake_log/src/*.cc" "${FW_DIR}/components/mooncake_log/src/*.c")
file(GLOB_RECURSE SMOOTH_SRCS "${FW_DIR}/components/smooth_ui_toolkit/src/*.cpp" "${FW_DIR}/components/smooth_ui_toolkit/src/*.cc" "${FW_DIR}/components/smooth_ui_toolkit/src/*.c")
file(GLOB_RECURSE STACKCHAN_SRCS "${FW_DIR}/main/stackchan/*.cpp" "${FW_DIR}/main/stackchan/*.cc" "${FW_DIR}/main/stackchan/*.c")
file(GLOB_RECURSE APPS_SRCS "${FW_DIR}/main/apps/*.cpp" "${FW_DIR}/main/apps/*.cc" "${FW_DIR}/main/apps/*.c")
file(GLOB_RECURSE ARDUINOJSON_SRCS "${FW_DIR}/components/ArduinoJson/src/*.cpp" "${FW_DIR}/components/ArduinoJson/src/*.cc" "${FW_DIR}/components/ArduinoJson/src/*.c")
file(GLOB_RECURSE ASSET_SRCS "${FW_DIR}/main/assets/*.cpp" "${FW_DIR}/main/assets/*.c" "${FW_DIR}/main/assets/*.cc")
file(GLOB PROTOCOL_SRCS "${FW_DIR}/xiaozhi-esp32/main/protocols/protocol.cc")
file(GLOB CJSON_SRCS "${CMAKE_CURRENT_SOURCE_DIR}/shims/cjson/cJSON.c")
file(GLOB DISPLAY_SRCS "${FW_DIR}/xiaozhi-esp32/main/display/display.cc")

set(APP_SOURCES
    main_wasm.cpp
    hal/hal_wasm.cpp
    hal/wasm_assets_stub.cpp
    hal/wasm_settings.cpp
    hal/wasm_system_info.cpp
    hal/wasm_protocol.cpp
    hal/wasm_display.cpp
    hal/wasm_audio_processor.cpp
    hal/wasm_application.cpp
    hal/wasm_camera.cpp
    "${FW_DIR}/xiaozhi-esp32/main/application_core.cc"
    "${FW_DIR}/xiaozhi-esp32/main/device_state_machine.cc"
    "${FW_DIR}/xiaozhi-esp32/main/display/lvgl_display/lvgl_image.cc"
    "${FW_DIR}/managed_components/78__xiaozhi-fonts/src/cbin_font.c"
    ${DISPLAY_SRCS}
    ${MOONCAKE_SRCS}
    ${MCLOG_SRCS}
    ${SMOOTH_SRCS}
    ${STACKCHAN_SRCS}
    ${APPS_SRCS}
    ${ARDUINOJSON_SRCS}
    ${ASSET_SRCS}
    ${PROTOCOL_SRCS}
    ${CJSON_SRCS}
)

add_executable(stack-chan ${APP_SOURCES})
set_target_properties(stack-chan PROPERTIES SUFFIX ".html")

# Our code: full warnings. `xiaozhi-esp32/main` and `.../protocols` stay non-SYSTEM
# because that is where our own application_core.h and usb_protocol.h live.
target_include_directories(stack-chan PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/shims"
    "${CMAKE_CURRENT_SOURCE_DIR}/hal"
    "${FW_DIR}/xiaozhi-esp32/main"
    "${FW_DIR}/xiaozhi-esp32/main/protocols"
    "${FW_DIR}/main"
    "${FW_DIR}/main/hal"
    "${FW_DIR}/main/apps"
    "${FW_DIR}/main/stackchan"
    "${FW_DIR}/main/assets"
)

# Vendored third-party and untouched upstream xiaozhi subtrees. SYSTEM suppresses
# their diagnostics (roller.hpp VLAs, lvgl_image.h missing-override) without
# desensitising our own sources.
target_include_directories(stack-chan SYSTEM PUBLIC
    "${FW_DIR}/components/ArduinoJson/src"
    "${FW_DIR}/components/mooncake/src"
    "${FW_DIR}/components/mooncake/src/mooncake"
    "${FW_DIR}/components/mooncake_log/src"
    "${FW_DIR}/components/mooncake_log/src/mooncake_log"
    "${FW_DIR}/components/smooth_ui_toolkit/src"
    "${FW_DIR}/components/smooth_ui_toolkit/src/lvgl"
    "${FW_DIR}/components/smooth_ui_toolkit/src/uitk"
    "${LVGL_DIR}"
    "${FW_DIR}/xiaozhi-esp32/main/display"
    "${FW_DIR}/xiaozhi-esp32/main/display/lvgl_display"
    "${FW_DIR}/xiaozhi-esp32/main/boards/common"
    "${FW_DIR}/xiaozhi-esp32/main/audio"
    "${FW_DIR}/xiaozhi-esp32/main/audio/demuxer"
    "${FW_DIR}/managed_components/78__xiaozhi-fonts/include"
)

# lvgl_image.cc includes its own header by quoted path, so it resolves relative to the
# source file and bypasses the SYSTEM classification above. Suppress at the source.
set_source_files_properties(
    "${FW_DIR}/xiaozhi-esp32/main/display/lvgl_display/lvgl_image.cc"
    PROPERTIES COMPILE_OPTIONS "-Wno-inconsistent-missing-override"
)

# HEAPU8/HEAP16 are read from JS (ui/camera_bridge.js, ui/audio_pipeline.js) and from
# EM_ASM bodies that use the `Module.` prefix. Under ASSERTIONS Emscripten installs an
# abort() getter on unexported runtime symbols, so merely *testing* `Module.HEAPU8`
# killed the runtime on the first camera frame. Exporting them is growth-safe:
# updateMemoryViews() reassigns Module['HEAPU8'] after every memory growth.
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} \
    -s ALLOW_MEMORY_GROWTH=1 \
    -s EXPORTED_FUNCTIONS=['_malloc','_free','_main','_wasm_camera_test_capture','_wasm_open_app_by_name'] \
    -s EXPORTED_RUNTIME_METHODS=['ccall','cwrap','HEAPU8','HEAP16'] \
    -s MODULARIZE=0 \
    -s EXPORT_ES6=0 \
    --js-library ${CMAKE_CURRENT_SOURCE_DIR}/hal/hal_bridge.js \
    --shell-file ${CMAKE_CURRENT_SOURCE_DIR}/shell.html \
    --embed-file ${FW_DIR}/main/assets/assets_bin@/assets_bin \
    -s ASYNCIFY=1 \
    -s ASYNCIFY_STACK_SIZE=65536 \
")

target_link_libraries(stack-chan lvgl)
set_property(TARGET stack-chan APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/shell.html")
