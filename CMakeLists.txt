cmake_minimum_required(VERSION 3.25)

project(VoxInsert
    VERSION 0.1.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(cpr CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(portaudio CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

find_path(OPUSENC_INCLUDE_DIR NAMES opus/opusenc.h REQUIRED)
find_library(OPUSENC_LIBRARY NAMES opusenc REQUIRED)
find_path(OPUS_INCLUDE_DIR NAMES opus/opus.h REQUIRED)
find_library(OPUS_LIBRARY NAMES opus REQUIRED)

add_executable(VoxInsert WIN32
    src/archive/archive_service.cpp
    src/audio/audio_recorder.cpp
    src/audio/wav_writer.cpp
    src/config/app_config.cpp
    src/input/hotkey_manager.cpp
    src/input/smtc_controller.cpp
    src/insertion/text_injector.cpp
    src/main.cpp
    src/observability/logging.cpp
    src/runtime/app_host.cpp
    src/runtime/app_host_recording.cpp
    src/runtime/app_host_runtime.cpp
    src/runtime/app_host_settings.cpp
    src/runtime/post_recording_workflow.cpp
    src/runtime/runtime_config_applier.cpp
    src/runtime/app_host_tray.cpp
    src/runtime/startup_registration.cpp
    src/security/api_credential_store.cpp
    src/testing/app_host_smoke_test.cpp
    src/transcription/mistral_transcription_service.cpp
    src/transcription/openai_transcription_service.cpp
    src/transcription/transcription_client.cpp
    src/ui/settings_dialog.cpp
    src/ui/status_pill.cpp
    src/VoxInsert.rc
)

target_include_directories(VoxInsert PRIVATE
    src
    ${OPUSENC_INCLUDE_DIR}
    ${OPUS_INCLUDE_DIR}
)

target_link_libraries(VoxInsert PRIVATE
    Advapi32
    Comctl32
    cpr::cpr
    Gdiplus
    nlohmann_json::nlohmann_json
    Ole32
    ${OPUSENC_LIBRARY}
    ${OPUS_LIBRARY}
    portaudio
    Shell32
    spdlog::spdlog
    WindowsApp
)

target_compile_definitions(VoxInsert PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if (MSVC)
    target_compile_options(VoxInsert PRIVATE
        /W4
        /permissive-
        /utf-8
    )

    target_link_options(VoxInsert PRIVATE
        /MANIFEST:NO
    )
endif()
