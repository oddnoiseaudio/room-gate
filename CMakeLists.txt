cmake_minimum_required(VERSION 3.22)
# static MSVC runtime: no Visual C++ Redistributable needed
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(OddnoiseRoomGate VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---- JUCE: use -DJUCE_DIR=/path/to/JUCE for a local checkout, otherwise it is fetched from GitHub ----
if(DEFINED JUCE_DIR AND EXISTS "${JUCE_DIR}/CMakeLists.txt")
    add_subdirectory("${JUCE_DIR}" JUCE)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(JUCE)
endif()

juce_add_gui_app(OddnoiseRoomGate
    PRODUCT_NAME "oddnoise Room Gate"
    COMPANY_NAME "oddnoise audio")

target_sources(OddnoiseRoomGate PRIVATE
    App/Main.cpp
    App/RoomNet.h
    Source/RoomEngine.h
    Source/AutomixCore.h)

target_include_directories(OddnoiseRoomGate PRIVATE Source App)

target_compile_definitions(OddnoiseRoomGate PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:OddnoiseRoomGate,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="${PROJECT_VERSION}")

target_link_libraries(OddnoiseRoomGate PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_devices
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# ---- tests (not built by default) ----
# offline simulation of two rooms with a simulated network (no JUCE needed)
add_executable(room_test EXCLUDE_FROM_ALL Tests/test_room.cpp)
target_include_directories(room_test PRIVATE Source)

# real-time smoke test: two engines talking over localhost with the real UDP transport
juce_add_console_app(NetSmoke PRODUCT_NAME "net_smoke")
target_sources(NetSmoke PRIVATE Tests/net_smoke.cpp)
target_include_directories(NetSmoke PRIVATE Source App Tests)
target_compile_definitions(NetSmoke PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(NetSmoke PRIVATE juce::juce_core juce::juce_events PUBLIC juce::juce_recommended_config_flags)
set_target_properties(NetSmoke PROPERTIES EXCLUDE_FROM_ALL TRUE)
