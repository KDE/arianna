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
        qml/SubjectOverviewPage.qml
        qml/EpubViewerPage.qml
        qml/PdfViewerPage.qml
        qml/ReportDialog.qml
        qml/OkularPdfView.qml
        qml/TableOfContentDrawer.qml
        qml/GridBrowserDelegate.qml
        qml/SearchModel.qml
    RESOURCES
        qml/main.html
        qml/reader.js
        qml/foliate-js/reader.html
        qml/markup.js

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

        qml/icons/bookmark-gold.svg
        qml/icons/reference-blue.svg
        qml/icons/book.svg
        qml/icons/epub_icon.png
        qml/icons/pdf_icon.png
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
    ariannatrace.cpp
    ariannatrace.h
    format.cpp
    format.h
    bookdatabase.cpp
    bookdatabase.h
    bookstate.h
    booktruthstore.cpp
    booktruthstore.h
    editorsession.h
    editorsessionstore.cpp
    editorsessionstore.h
    bookstateimport.cpp
    bookstateimport.h
    annotationstore.cpp
    annotationstore.h
    referencestore.cpp
    referencestore.h
    bookserverconfig.cpp
    bookserverconfig.h
    bookserver.cpp
    bookserver.h
    externalprocess.cpp
    externalprocess.h
    fileopener.cpp
    fileopener.h
    translator.cpp
    translator.h
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
    okularpdfdocument.cpp
    okularpdfdocument.h
    okularpdfsupport.cpp
    okularpdfsupport.h
    okularpdfpageitem.cpp
    okularpdfpageitem.h
    okularpdfpagepainter.cpp
    okularpdfpagepainter.h
    pdfiodocument.cpp
    pdfiodocument.h
    pdfwatermarkfilter.cpp
    pdfwatermarkfilter.h
    pdfversionupdater.cpp
    pdfversionupdater.h
    pdfvalidationreportparser.cpp
    pdfvalidationreportparser.h
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
)

set_source_files_properties(okularpdfdocument.cpp pdfwatermarkfilter.cpp pdfversionupdater.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")

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
    Qt6::PrintSupport
    PoDoFo::PoDoFo
    Okular::Core
    KF6::I18n
    KF6::Kirigami
    KF6::ConfigCore
    KF6::ConfigGui
    KF6::ConfigQml
    KF6::WindowSystem
    KF6::DBusAddons
    KF6::FileMetaData
    KF6::Archive
    KF6::CoreAddons
    KF6::Crash
    KF6::ColorScheme
    KF6::I18nQml
    KF6::KIOGui
    KF6::XmlGui
)

target_include_directories(arianna_static PRIVATE
    ${ARIANNA_OKULAR_SOURCE_DIR}
    ${ARIANNA_OKULAR_BUILD_DIR}
    ${ARIANNA_OKULAR_BUILD_DIR}/core
)

target_compile_definitions(arianna_static PRIVATE
    ARIANNA_OKULAR_PLUGIN_DIR="${ARIANNA_OKULAR_BUILD_DIR}/bin"
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
