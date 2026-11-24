cmake_minimum_required(VERSION 3.21)
project(jscan VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Gui Network LinguistTools)

add_executable(jscan
    src/main.cpp
    src/mainwindow.cpp src/mainwindow.h
    src/scanworker.cpp src/scanworker.h
    src/theme.cpp src/theme.h
    src/previewwidget.cpp src/previewwidget.h
    src/pdfexport.cpp src/pdfexport.h
    src/esclclient.cpp src/esclclient.h
    src/imageops.cpp src/imageops.h
)
target_link_libraries(jscan PRIVATE Qt6::Widgets Qt6::Gui Qt6::Network)

# German translation of the shared core (scan/PDF messages); English is the
# source language. The .ts is shared with the Sailfish build, see
# translations/update.sh. Embedded as :/i18n/jscan-de.qm.
qt_add_lrelease(jscan TS_FILES translations/jscan-de.ts QM_FILES_OUTPUT_VARIABLE jscan_qm)
qt_add_resources(jscan "translations" PREFIX "/i18n" BASE "${CMAKE_CURRENT_BINARY_DIR}" FILES ${jscan_qm})

# Scanner access: one implementation of ScanWorker per platform.
# The GUI only talks to the ScanWorker interface (src/scanworker.h).
if(WIN32)
    # Windows backend (TWAIN/WIA) – still to be written: src/scanworker_win.cpp
    message(FATAL_ERROR "Windows scanner backend not implemented yet (src/scanworker_win.cpp)")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SANE REQUIRED IMPORTED_TARGET sane-backends)
    # own driver for the Epson Perfection V600 Photo (libusb)
    pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)
    find_package(Threads REQUIRED)
    target_sources(jscan PRIVATE src/scanworker_sane.cpp
        src/v600scanner.cpp src/v600scanner.h src/v600_tables_400.h)
    target_link_libraries(jscan PRIVATE PkgConfig::SANE PkgConfig::LIBUSB Threads::Threads)
endif()

include(GNUInstallDirs)
install(TARGETS jscan RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
if(UNIX AND NOT APPLE)
    install(FILES jscan.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
endif()
