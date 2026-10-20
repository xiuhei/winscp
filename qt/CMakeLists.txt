cmake_minimum_required(VERSION 3.21)

project(WinSCPQt VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_INCLUDE_CURRENT_DIR ON)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets Network Svg SvgWidgets Concurrent Test)
find_package(libssh2 CONFIG REQUIRED)

qt_standard_project_setup()

set(WINSCPQT_CORE_SOURCES
    src/core/AppPaths.cpp
    src/core/BackendFactory.cpp
    src/core/FileSystemController.cpp
    src/core/KnownHosts.cpp
    src/core/Logger.cpp
    src/core/PasswordStore.cpp
    src/core/PathUtils.cpp
    src/core/SessionData.cpp
    src/core/Settings.cpp
    src/core/Types.cpp
    src/proto/FtpBackend.cpp
    src/proto/LocalBackend.cpp
    src/proto/ScpBackend.cpp
    src/proto/SftpBackend.cpp
    src/proto/WebDavBackend.cpp
    src/ssh/SshConnection.cpp
    src/sync/SyncPlanner.cpp
    src/terminal/SshShellSession.cpp
    src/terminal/TerminalScreen.cpp
    src/terminal/TerminalWidget.cpp
    src/transfer/TransferManager.cpp
    src/transfer/TransferQueueModel.cpp
    src/transfer/TransferTypes.cpp
    src/ui/FilePane.cpp
    src/ui/FileTableModel.cpp
    src/ui/IconProvider.cpp
    src/ui/LoginDialog.cpp
    src/ui/MainWindow.cpp
    src/ui/RemoteFileOpener.cpp
    src/ui/PreferencesDialog.cpp
    src/ui/PropertiesDialog.cpp
    src/ui/SessionEditor.cpp
    src/ui/SyncDialog.cpp
    src/ui/TerminalPanel.cpp
    src/ui/TextEditorDialog.cpp
    src/ui/Theme.cpp
    src/ui/TransferQueueWidget.cpp
    resources/resources.qrc
)

add_library(winscpqt_lib STATIC ${WINSCPQT_CORE_SOURCES})
target_include_directories(winscpqt_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(winscpqt_lib PUBLIC
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Svg
    Qt6::SvgWidgets
    Qt6::Concurrent
    libssh2::libssh2_shared
)

if(WIN32)
    target_link_libraries(winscpqt_lib PUBLIC ws2_32 crypt32)
    target_compile_options(winscpqt_lib PRIVATE /utf-8 /W3)
    target_compile_definitions(winscpqt_lib PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(WIN32)
    configure_file(resources/app.rc.in "${CMAKE_CURRENT_BINARY_DIR}/app.rc" @ONLY)
    set(WINSCPQT_WIN_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/app.rc")
else()
    set(WINSCPQT_WIN_RESOURCE)
endif()

qt_add_executable(WinSCPQt WIN32 src/main.cpp ${WINSCPQT_WIN_RESOURCE})
target_link_libraries(WinSCPQt PRIVATE winscpqt_lib)
if(WIN32)
    target_compile_options(WinSCPQt PRIVATE /utf-8 /W3)
    target_compile_definitions(WinSCPQt PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_executable(WinSCPQtBackendSmoke tests/backend_smoke.cpp)
target_link_libraries(WinSCPQtBackendSmoke PRIVATE winscpqt_lib)

add_executable(WinSCPQtTransferSmoke tests/transfer_smoke.cpp)
target_link_libraries(WinSCPQtTransferSmoke PRIVATE winscpqt_lib)

add_executable(WinSCPQtTerminalSmoke tests/terminal_smoke.cpp)
target_link_libraries(WinSCPQtTerminalSmoke PRIVATE winscpqt_lib Qt6::Test)

add_executable(WinSCPQtTerminalScreenTest tests/terminal_screen_test.cpp)
target_link_libraries(WinSCPQtTerminalScreenTest PRIVATE winscpqt_lib Qt6::Test)

add_executable(WinSCPQtTerminalWidgetTest tests/terminal_widget_test.cpp)
target_link_libraries(WinSCPQtTerminalWidgetTest PRIVATE winscpqt_lib Qt6::Test)

add_executable(WinSCPQtWindowTest tests/window_test.cpp)
target_link_libraries(WinSCPQtWindowTest PRIVATE winscpqt_lib Qt6::Test)

add_executable(WinSCPQtUiWorkflowTest tests/ui_workflow_test.cpp)
target_link_libraries(WinSCPQtUiWorkflowTest PRIVATE winscpqt_lib Qt6::Test)

add_executable(WinSCPQtSyncSmoke tests/sync_smoke.cpp)
target_link_libraries(WinSCPQtSyncSmoke PRIVATE winscpqt_lib)

enable_testing()
add_test(NAME terminal_screen COMMAND WinSCPQtTerminalScreenTest)
add_test(NAME terminal_keyboard COMMAND WinSCPQtTerminalWidgetTest)
add_test(NAME ui_workflows COMMAND WinSCPQtUiWorkflowTest)
set_tests_properties(ui_workflows PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
set_tests_properties(terminal_keyboard PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

if(WIN32)
    target_compile_options(WinSCPQtTerminalWidgetTest PRIVATE /utf-8 /W3)
    target_compile_options(WinSCPQtWindowTest PRIVATE /utf-8 /W3)
    target_compile_options(WinSCPQtUiWorkflowTest PRIVATE /utf-8 /W3)
    target_compile_options(WinSCPQtBackendSmoke PRIVATE /utf-8 /W3)
    target_compile_definitions(WinSCPQtBackendSmoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(WinSCPQtTransferSmoke PRIVATE /utf-8 /W3)
    target_compile_definitions(WinSCPQtTransferSmoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(WinSCPQtTerminalSmoke PRIVATE /utf-8 /W3)
    target_compile_definitions(WinSCPQtTerminalSmoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(WinSCPQtTerminalScreenTest PRIVATE /utf-8 /W3)
    target_compile_definitions(WinSCPQtTerminalScreenTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(WinSCPQtSyncSmoke PRIVATE /utf-8 /W3)
    target_compile_definitions(WinSCPQtSyncSmoke PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

install(TARGETS WinSCPQt RUNTIME DESTINATION .)


