cmake_minimum_required(VERSION 3.20)
project(DumpCsExplorer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Concurrent)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

add_executable(DumpCsExplorer
		src/main.cpp
		src/ui/MainWindow.cpp
		src/ui/MainWindow.h
		src/parser/DumpCsParser.cpp
		src/parser/DumpCsParser.h
)

target_include_directories(DumpCsExplorer PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(DumpCsExplorer PRIVATE Qt6::Widgets Qt6::Concurrent)
