# SPDX-FileCopyrightText: 2021 Carl Schwan <carlschwan@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

add_library(arianna_static STATIC)

ecm_qt_declare_logging_category(arianna_static
    HEADER arianna_debug.h
    IDENTIFIER ARIANNA_LOG
    CATEGORY_NAME org.kde.arianna
    DESCRIPTION "arianna"
    DEFAULT_SEVERITY Warning
    EXPORT ARIANNA
)

target_sources(arianna_static PRIVATE
    clipboard.cpp
    clipboard.h
    format.cpp
    format.h
    bookdatabase.cpp
    bookdatabase.h
    bookserver.cpp
    bookserver.h
    categoryentriesmodel.cpp
    categoryentriesmodel.h
    colorschemer.cpp
    colorschemer.h
    propertycontainer.cpp
    propertycontainer.h
    booklistmodel.cpp
    booklistmodel.h
    epubcontainer.cpp
    epubcontainer.h
    tableofcontentmodel.cpp
    tableofcontentmodel.h
    navigation.cpp
    navigation.h
    contentlist/contentlist.cpp
    contentlist/contentlist.h
    contentlist/contentquery.cpp
    contentlist/contentquery.h
    contentlist/contentlisterbase.cpp
    contentlist/contentlisterbase.h
    contentlist/filesystemcontentlister.cpp
    contentlist/filesystemcontentlister.h
    contentlist/manualcontentlister.cpp
    contentlist/manualcontentlister.h
    ${debug_files_SRCS}
)

kconfig_add_kcfg_files(arianna_static GENERATE_MOC config.kcfgc)

target_link_libraries(arianna_static PUBLIC
    Qt6::Quick
    Qt6::Qml
    Qt6::Gui
    Qt6::HttpServer
    Qt6::Network
    Qt6::WebSockets
    Qt6::QuickControls2
    Qt6::Svg
    Qt6::Sql
    Qt6::Xml
    Qt6::Widgets
    KF6::I18n
    KF6::Kirigami
    KF6::ConfigCore
    KF6::ConfigGui
    KF6::WindowSystem
    KF6::DBusAddons
    KF6::FileMetaData
    KF6::Archive
    KF6::CoreAddons
    KF6::Crash
    KF6::ColorScheme
)

target_link_libraries(arianna_static PUBLIC Qt::WebEngineQuick)

if (NOT WIN32)
    target_link_libraries(arianna_static PUBLIC KF6::Baloo)
    target_sources(arianna_static PRIVATE contentlist/baloocontentlister.cpp contentlist/baloocontentlister.h)
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

install(TARGETS arianna ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
