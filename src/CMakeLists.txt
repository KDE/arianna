# SPDX-FileCopyrightText: 2021 Carl Schwan <carlschwan@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

set(debug_files_SRCS)

ecm_qt_declare_logging_category(debug_files_SRCS
    HEADER arianna_debug.h
    IDENTIFIER ARIANNA_LOG
    CATEGORY_NAME org.kde.arianna
    DESCRIPTION "arianna"
    DEFAULT_SEVERITY Warning
    EXPORT ARIANNA
)

set(arianna_src
    clipboard.cpp
    format.cpp
    bookdatabase.cpp
    categoryentriesmodel.cpp
    propertycontainer.cpp
    booklistmodel.cpp
    epubcontainer.cpp

    navigation.h
    navigation.cpp

    contentlist/contentlist.cpp
    contentlist/contentquery.cpp
    contentlist/contentlisterbase.cpp
    contentlist/filesystemcontentlister.cpp
    contentlist/manualcontentlister.cpp
    ${debug_files_SRCS}
)

add_library(arianna_static STATIC ${arianna_src})
target_link_libraries(arianna_static PUBLIC
    Qt::Quick
    Qt::Qml
    Qt::Gui
    Qt::Network
    Qt::WebSockets
    Qt::WebEngine
    Qt::QuickControls2
    Qt::Svg
    Qt::Sql
    KF5::I18n
    KF5::Kirigami2
    KF5::ConfigCore
    KF5::ConfigGui
    KF5::WindowSystem
    KF5::DBusAddons
    KF5::FileMetaData
    KF5::Archive
    KF5::CoreAddons)

if (NOT WIN32)
    target_link_libraries(arianna_static PUBLIC KF5::Baloo)
    target_sources(arianna_static PRIVATE contentlist/baloocontentlister.cpp)
    target_compile_definitions(arianna_static PUBLIC -DHAVE_BALOO)
endif()

if (BUILD_TESTING)
    add_subdirectory(autotests)
endif()

add_executable(arianna
    main.cpp
    resources.qrc
)

target_include_directories(arianna PRIVATE ${CMAKE_BINARY_DIR})
target_link_libraries(arianna PRIVATE arianna_static)
kconfig_add_kcfg_files(arianna GENERATE_MOC config.kcfgc)

install(TARGETS arianna ${KF5_INSTALL_TARGETS_DEFAULT_ARGS})
