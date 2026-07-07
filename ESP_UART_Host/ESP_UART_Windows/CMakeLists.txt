cmake_minimum_required(VERSION 3.16)

project(ESP_UART_Windows
    VERSION 0.2.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets SerialPort Network)

qt_standard_project_setup(REQUIRES 6.8)

set(CMAKE_AUTORCC ON)

qt_add_executable(esp_uart
    main.cpp
    mainwindow.h
    mainwindow.cpp
    serialworker.h
    serialworker.cpp
    colormap.h
    colormap.cpp
    frameparser.h
    frameparser.cpp
    thermalwidget.h
    thermalwidget.cpp
    resources.qrc
)

set_target_properties(esp_uart PROPERTIES
    WIN32_EXECUTABLE TRUE
)

target_link_libraries(esp_uart PRIVATE
    Qt6::Widgets
    Qt6::SerialPort
    Qt6::Network
)

qt_add_executable(frameparser_selftest
    tests/frameparser_selftest.cpp
    frameparser.h
    frameparser.cpp
)

target_link_libraries(frameparser_selftest PRIVATE
    Qt6::Core
)

qt_add_executable(thermalwidget_selftest
    tests/thermalwidget_selftest.cpp
    colormap.h
    colormap.cpp
)

target_link_libraries(thermalwidget_selftest PRIVATE
    Qt6::Gui
)
