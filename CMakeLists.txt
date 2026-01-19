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

if (WIN32)
	set_target_properties(DumpCsExplorer PROPERTIES WIN32_EXECUTABLE TRUE)
	if (TARGET Qt6::WinMain)
		target_link_libraries(DumpCsExplorer PRIVATE Qt6::WinMain)
	elseif (TARGET Qt6::EntryPoint)
		target_link_libraries(DumpCsExplorer PRIVATE Qt6::EntryPoint)
	endif()

	set(_ICON_FILE "")
	foreach(_cand
			"${CMAKE_SOURCE_DIR}/Icon.ico"
	)
		if (EXISTS "${_cand}")
			set(_ICON_FILE "${_cand}")
			break()
		endif()
	endforeach()

	if (_ICON_FILE)
		file(TO_NATIVE_PATH "${_ICON_FILE}" _ICON_FILE_NATIVE)
		set(_RC_FILE "${CMAKE_CURRENT_BINARY_DIR}/DumpCsExplorerIcon.rc")
		file(WRITE "${_RC_FILE}" "IDI_ICON1 ICON \"${_ICON_FILE_NATIVE}\"\n")
		target_sources(DumpCsExplorer PRIVATE "${_RC_FILE}")
	endif()
endif()
