cmake_minimum_required(VERSION 3.16)

project(ESP_UART
    VERSION 0.2.0
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 6.8 REQUIRED COMPONENTS Widgets SerialPort)

qt_standard_project_setup(REQUIRES 6.8)

qt_add_executable(esp_uart
    main.cpp
    mainwindow.h
    mainwindow.cpp
    colormap.h
    colormap.cpp
    frameparser.h
    frameparser.cpp
    thermalwidget.h
    thermalwidget.cpp
)

set_target_properties(esp_uart PROPERTIES
    WIN32_EXECUTABLE TRUE
    MACOSX_BUNDLE TRUE
)

target_link_libraries(esp_uart PRIVATE Qt6::Widgets Qt6::SerialPort)
