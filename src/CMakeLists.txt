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
    tableofcontentmodel.cpp

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
    Qt::QuickControls2
    Qt::Svg
    Qt::Sql
    Qt::Xml
    Qt::Widgets
    KF${QT_MAJOR_VERSION}::I18n
    KF${QT_MAJOR_VERSION}::Kirigami2
    KF${QT_MAJOR_VERSION}::ConfigCore
    KF${QT_MAJOR_VERSION}::ConfigGui
    KF${QT_MAJOR_VERSION}::WindowSystem
    KF${QT_MAJOR_VERSION}::DBusAddons
    KF${QT_MAJOR_VERSION}::FileMetaData
    KF${QT_MAJOR_VERSION}::Archive
    KF${QT_MAJOR_VERSION}::CoreAddons)

if (${QT_MAJOR_VERSION} STREQUAL 5)
    target_link_libraries(arianna_static PUBLIC Qt::WebEngine)
else()
    target_link_libraries(arianna_static PUBLIC Qt::WebEngineQuick)
endif()

if (NOT WIN32)
    target_link_libraries(arianna_static PUBLIC KF${QT_MAJOR_VERSION}::Baloo)
    target_sources(arianna_static PRIVATE contentlist/baloocontentlister.cpp)
    target_compile_definitions(arianna_static PUBLIC -DHAVE_BALOO)
endif()

if (BUILD_TESTING)
    add_subdirectory(autotests)
endif()

if(QT_MAJOR_VERSION STREQUAL "5")
       set(QML_QTGRAPHICAL_EFFECTS_IMPORT "QtGraphicalEffects 1.15")
       set(KIRIGAMI_PLACEITEM_ICON "icon")
       set(KIRIGAMI_ACTIONS "actions.main")
else()
       set(QML_QTGRAPHICAL_EFFECTS_IMPORT "Qt5Compat.GraphicalEffects")
       set(KIRIGAMI_PLACEITEM_ICON "icon.name")
       set(KIRIGAMI_ACTIONS "actions")
endif()

configure_file(content/ui/GridBrowserDelegate.qml ${CMAKE_CURRENT_BINARY_DIR}/content/ui/GridBrowserDelegate.qml)
configure_file(content/ui/main.qml ${CMAKE_CURRENT_BINARY_DIR}/content/ui/main.qml)
configure_file(content/ui/LibraryPage.qml ${CMAKE_CURRENT_BINARY_DIR}/content/ui/LibraryPage.qml)
configure_file(resources.qrc.in ${CMAKE_CURRENT_SOURCE_DIR}/resources.qrc)

add_executable(arianna
    main.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/resources.qrc
)

target_include_directories(arianna PRIVATE ${CMAKE_BINARY_DIR})
target_link_libraries(arianna PRIVATE arianna_static)
kconfig_add_kcfg_files(arianna GENERATE_MOC config.kcfgc)

install(TARGETS arianna ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
