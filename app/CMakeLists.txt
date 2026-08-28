cmake_minimum_required(VERSION 3.21)
project(cortex_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Quick QuickControls2 Concurrent)
qt_standard_project_setup(REQUIRES 6.5)

set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

qt_add_executable(cortex WIN32
    main.cpp
    app_controller.cpp
    app_controller.h
    ../core/target/local_backend.cpp
    ../core/target/local_backend.h
    ../core/target/session_manager.cpp
    ../core/target/session_manager.h
    ../core/services/memory_service.cpp
    ../core/services/memory_service.h
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
        qml/components/DebuggerView.qml
        qml/components/DisassemblyView.qml
        qml/components/GenericToolView.qml
        qml/components/BottomPanel.qml
        qml/components/CommandPalette.qml
)

target_include_directories(cortex PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../core
)

target_link_libraries(cortex PRIVATE
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::Concurrent
)

if(WIN32)
    target_compile_definitions(cortex PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_link_libraries(cortex PRIVATE user32)
endif()

set_target_properties(cortex PROPERTIES
    OUTPUT_NAME "cortex"
    MACOSX_BUNDLE TRUE
)
