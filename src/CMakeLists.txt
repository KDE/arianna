# SPDX-FileCopyrightText: 2021 Carl Schwan <carlschwan@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

add_library(arianna_static STATIC)

ecm_add_qml_module(arianna_static
    GENERATE_PLUGIN_SOURCE
    OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/src/org/kde/arianna # Shuts up an annoying upstream Qt warning
    URI org.kde.arianna
)

qt_target_qml_sources(arianna_static
    QML_FILES
        qml/Main.qml
        qml/BookDetailsPage.qml
        qml/SettingsPage.qml
        qml/ColorSchemeDelegate.qml
        qml/LibraryPage.qml
        qml/EpubViewerPage.qml
        qml/TableOfContentDrawer.qml
        qml/GridBrowserDelegate.qml
        qml/SearchModel.qml
    RESOURCES
        qml/main.html
        qml/reader.js
        qml/foliate-js/reader.html
        qml/markup.js

        qml/foliate-js/vendor/pdfjs/pdf.js
        qml/foliate-js/vendor/pdfjs/pdf.worker.js
        qml/foliate-js/vendor/fflate.js
        qml/foliate-js/vendor/zip.js

        qml/foliate-js/comic-book.js
        qml/foliate-js/dict.js
        qml/foliate-js/epub.js
        qml/foliate-js/epubcfi.js
        qml/foliate-js/fb2.js
        qml/foliate-js/fixed-layout.js
        qml/foliate-js/footnotes.js
        qml/foliate-js/mobi.js
        qml/foliate-js/opds.js
        qml/foliate-js/overlayer.js
        qml/foliate-js/paginator.js
        qml/foliate-js/pdf.js
        qml/foliate-js/progress.js
        qml/foliate-js/reader.js
        qml/foliate-js/search.js
        qml/foliate-js/text-walker.js
        qml/foliate-js/tts.js
        qml/foliate-js/uri-template.js
        qml/foliate-js/view.js
        qml/foliate-js/quote-image.js

        qml/jszip.min.js
        qml/utils.js
)

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
)

target_include_directories(arianna PRIVATE ${CMAKE_BINARY_DIR})
target_link_libraries(arianna PUBLIC arianna_static arianna_staticplugin)

install(TARGETS arianna ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
