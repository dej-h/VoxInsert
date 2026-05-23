cmake_minimum_required(VERSION 3.25)

project(VoiceAgentTyper
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

add_executable(VoiceAgentTyper WIN32
    src/audio/audio_recorder.cpp
    src/audio/wav_writer.cpp
    src/config/app_config.cpp
    src/input/hotkey_manager.cpp
    src/insertion/text_injector.cpp
    src/main.cpp
    src/observability/logging.cpp
    src/runtime/app_host.cpp
    src/security/openai_credential_store.cpp
    src/testing/app_host_smoke_test.cpp
    src/transcription/transcription_client.cpp
    src/ui/status_pill.cpp
    src/VoiceAgentTyper.rc
)

target_include_directories(VoiceAgentTyper PRIVATE
    src
)

target_link_libraries(VoiceAgentTyper PRIVATE
    Advapi32
    Comctl32
    cpr::cpr
    Gdiplus
    nlohmann_json::nlohmann_json
    Ole32
    portaudio
    Shell32
    spdlog::spdlog
)

target_compile_definitions(VoiceAgentTyper PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if (MSVC)
    target_compile_options(VoiceAgentTyper PRIVATE
        /W4
        /permissive-
        /utf-8
    )

    target_link_options(VoiceAgentTyper PRIVATE
        /MANIFEST:NO
    )
endif()
