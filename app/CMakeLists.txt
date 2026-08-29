cmake_minimum_required(VERSION 3.21)
project(cortex_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.4 REQUIRED COMPONENTS Quick QuickControls2 Concurrent)
qt_standard_project_setup(REQUIRES 6.4)

include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
set(CMAKE_DISABLE_FIND_PACKAGE_Doxygen TRUE CACHE BOOL "" FORCE)

FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03
)
set(JSON_BuildTests OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(json)

FetchContent_Declare(
    zycore
    GIT_REPOSITORY https://github.com/zyantific/zycore-c.git
    GIT_TAG        74620eefd233bec20daeb66e78e744ff06e273b7
)
FetchContent_MakeAvailable(zycore)

FetchContent_Declare(
    zydis
    GIT_REPOSITORY https://github.com/zyantific/zydis.git
    GIT_TAG        569320ad3c4856da13b9dbf1f0d9e20bda63870e
)
set(ZYDIS_BUILD_TOOLS OFF CACHE INTERNAL "")
set(ZYDIS_BUILD_EXAMPLES OFF CACHE INTERNAL "")
set(ZYDIS_BUILD_TESTS OFF CACHE INTERNAL "")
set(ZYDIS_BUILD_DOXYGEN OFF CACHE INTERNAL "")
set(ZYDIS_BUILD_MAN OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(zydis)

set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(cortex WIN32
    main.cpp
    mcp_mode.cpp
    mcp_mode.h
    app_controller.cpp
    app_controller.h
    debugger_controller.cpp
    debugger_controller.h
    disassembly_controller.cpp
    disassembly_controller.h
    feature_controller.cpp
    feature_controller.h
    payload_controller.cpp
    payload_controller.h
    prompt_controller.cpp
    prompt_controller.h
    runtime_controller.cpp
    runtime_controller.h
    ../core/target/local_backend.cpp
    ../core/target/local_backend.h
    ../core/target/module_provider.cpp
    ../core/target/module_provider.h
    ../core/target/session_manager.cpp
    ../core/target/session_manager.h
    ../core/target/thread_provider.cpp
    ../core/target/thread_provider.h
    ../core/services/debugger_service.cpp
    ../core/services/debugger_service.h
    ../core/services/disassembly_service.cpp
    ../core/services/disassembly_service.h
    ../core/services/memory_service.cpp
    ../core/services/memory_service.h
    ../core/services/module_service.cpp
    ../core/services/module_service.h
    ../core/services/payload_client.cpp
    ../core/services/payload_client.h
    ../core/services/scan_service.cpp
    ../core/services/scan_service.h
)

qt_add_qml_module(cortex
    URI Cortex
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/Theme.qml
        qml/components/Button.qml
        qml/components/TextField.qml
        qml/components/ComboBox.qml
        qml/components/TopBar.qml
        qml/components/ProcessPicker.qml
        qml/components/Sidebar.qml
        qml/components/Workspace.qml
        qml/components/OverviewView.qml
        qml/components/MemoryView.qml
        qml/components/ScannerView.qml
        qml/components/ModulesView.qml
        qml/components/DebuggerView.qml
        qml/components/DisassemblyView.qml
        qml/components/BreakpointsView.qml
        qml/components/TracesView.qml
        qml/components/StructuresView.qml
        qml/components/SymbolsView.qml
        qml/components/HooksView.qml
        qml/components/NetworkView.qml
        qml/components/ScreenshotsView.qml
        qml/components/DiagnosticsView.qml
        qml/components/ScriptsView.qml
        qml/components/InputView.qml
        qml/components/ActionsView.qml
        qml/components/SessionsView.qml
        qml/components/McpView.qml
        qml/components/GenericToolView.qml
        qml/components/BottomPanel.qml
        qml/components/CommandPalette.qml
        qml/components/PromptSurface.qml
)

target_include_directories(cortex PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core
)

target_link_libraries(cortex PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Concurrent
    nlohmann_json::nlohmann_json
    Zydis
)

if(WIN32)
    target_compile_definitions(cortex PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(cortex PRIVATE user32)
endif()

set_target_properties(cortex PROPERTIES
    OUTPUT_NAME "cortex"
    MACOSX_BUNDLE TRUE
)
