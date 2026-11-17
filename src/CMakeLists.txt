# SPDX-FileCopyrightText: 2021 Carl Schwan <carlschwan@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

add_library(arianna_static STATIC)
qt_add_resources(MATHJAX_RESOURCES thirdparty/mathjax.qrc)
target_sources(arianna_static PRIVATE ${MATHJAX_RESOURCES})

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
        qml/WorkStructureDialog.qml
        qml/CodexSessionPane.qml
        qml/RichNoteDialog.qml
        qml/AnnotationColorPalette.qml
        qml/CrossReferenceDialog.qml
        qml/CrossReferenceEditor.qml
        qml/ReferenceSearchList.qml
        qml/ReferenceTargetTree.qml
        qml/TargetAnchorRepair.qml
        qml/BookServerRequest.qml
        qml/reference-utils.mjs
        qml/NoteHtmlView.qml
        qml/CodexTestDialog.qml
        qml/ReaderContextPane.qml
        qml/ColorSchemeDelegate.qml
        qml/LibraryPage.qml
        qml/LibrarySearchPane.qml
        qml/LibrarySearchPage.qml
        qml/MaintaincePage.qml
        qml/ImportPageView.qml
        qml/BookEditor.qml
        qml/SubjectOverviewPage.qml
        qml/EpubViewerPage.qml
        qml/PdfViewerPage.qml
        qml/SpeechControlWindow.qml
        qml/MathJaxWindow.qml
        qml/FootnoteWindow.qml
        qml/MathJaxMenu.qml
        qml/MathJaxSettings.qml
        qml/PdfPasswordHandler.qml
        qml/PdfPasswordIndicator.qml
        qml/ReportDialog.qml
        qml/ReferenceReportDialog.qml
        qml/ReferenceTargetDialog.qml
        qml/ReferenceTitleDialog.qml
        qml/ReferenceRemovalDialog.qml
        qml/EpubImportRepairDialog.qml
        qml/EditionComparisonDialog.qml
        qml/OkularPdfView.qml
        qml/TableOfContentDrawer.qml
        qml/GridBrowserDelegate.qml
        qml/BookFormatDialog.qml
        qml/SearchModel.qml
    RESOURCES
        qml/main.html
        qml/note-view.html
        qml/note-view.js
        qml/reader.js
        qml/reader-interactions.js
        qml/footnote-preview.js
        qml/speech-blocks.js
        qml/work-structure.js
        qml/reader-context.js
        qml/markup.js

        qml/foliate-js/vendor/fflate.js
        qml/foliate-js/vendor/zip.js

        qml/foliate-js/comic-book.js
        qml/foliate-js/epub.js
        qml/foliate-js/epubcfi.js
        qml/foliate-js/fb2.js
        qml/foliate-js/fixed-layout.js
        qml/foliate-js/footnotes.js
        qml/foliate-js/mobi.js
        qml/foliate-js/overlayer.js
        qml/foliate-js/paginator.js
        qml/foliate-js/mathjax.js
        qml/foliate-js/progress.js
        qml/foliate-js/search.js
        qml/foliate-js/text-walker.js
        qml/foliate-js/view.js

        qml/jszip.min.js
        qml/utils.js

        qml/icons/bookmark-gold.svg
        qml/icons/reference-blue.svg
        qml/icons/read-aloud.svg
        qml/icons/speech-continuous.svg
        qml/icons/book.svg
        qml/icons/book.png
        qml/icons/epub_icon.png
        qml/icons/pdf_icon.png
        qml/icons/locked-pdf-cover.svg
        qml/icons/unlocked-pdf-cover.svg
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
    bookcursor.cpp
    bookcursor.h
    clipboard.cpp
    clipboard.h
    workstructure.cpp
    workstructure.h
    workstructurecommit.cpp
    workstructurecommit.h
    workstructurereview.cpp
    workstructurereview.h
    codexclient.cpp
    codexclient.h
    readercontextbuilder.cpp
    readercontextbuilder.h
    ariannatrace.cpp
    ariannatrace.h
    format.cpp
    format.h
    bookdatabase.cpp
    bookdatabase.h
    bookformats.cpp
    bookformats.h
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
    noterenderer.cpp
    noterenderer.h
    referencestore.cpp
    referencestore.h
    bookreferencecheck.cpp
    bookreferencecheck.h
    librarysearchmodel.cpp
    librarysearchmodel.h
    pdfsearchprocess.cpp
    pdfsearchprocess.h
    librarymaintenance.cpp
    librarymaintenance.h
    maintenancefiles.cpp
    maintenancefiles.h
    pdfreferenceindex.cpp
    pdfreferenceindex.h
    bookserverconfig.cpp
    bookserverconfig.h
    bookserver.cpp
    bookserver.h
    externalprocess.cpp
    externalprocess.h
    sigilnavigation.cpp
    sigilnavigation.h
    fileopener.cpp
    fileopener.h
    translator.cpp
    translator.h
    speechcontroller.cpp
    speechcontroller.h
    sanskritnumbers.cpp
    sanskritnumbers.h
    transliterator.cpp
    transliterator.h
    categoryentriesmodel.cpp
    categoryentriesmodel.h
    colorschemer.cpp
    colorschemer.h
    booklistmodel.cpp
    booklistmodel.h
    epubcontainer.cpp
    epubcontainer.h
    epubmath.cpp
    epubmath.h
    tableofcontentmodel.cpp
    tableofcontentmodel.h
    referencetargetmodel.cpp
    referencetargetmodel.h
    navigation.cpp
    navigation.h
    readerhistory.cpp
    readerhistory.h
    readercontrol.cpp
    readercontrol.h
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
    pdfpasswordstore.cpp
    pdfpasswordstore.h
    pdfsecurityinfo.cpp
    pdfsecurityinfo.h
    pdfwatermarkfilter.cpp
    pdfwatermarkfilter.h
    pdfversionupdater.cpp
    pdfversionupdater.h
    pdfbookidentifier.cpp
    pdfbookidentifier.h
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

set_source_files_properties(pdfsearchhelper.cpp okularpdfdocument.cpp pdfreferenceindex.cpp pdfpasswordstore.cpp pdfsecurityinfo.cpp pdfwatermarkfilter.cpp pdfversionupdater.cpp pdfbookidentifier.cpp PROPERTIES COMPILE_OPTIONS "-fexceptions")

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
    Qt6::DBus
    PoDoFo::PoDoFo
    PkgConfig::IAST
    PkgConfig::SPEECHD
    Okular::Core
    KF6::I18n
    KF6::Kirigami
    KF6::ConfigCore
    KF6::ConfigGui
    KF6::ConfigQml
    KF6::WindowSystem
    KF6::DBusAddons
    KF6::Wallet
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
    ${IAST_INCLUDE_DIR}
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

# Keep document parser crashes outside the reader process.
add_executable(arianna-pdf-search pdfsearchhelper.cpp)
target_link_libraries(arianna-pdf-search PRIVATE Qt6::Core KF6::I18n PoDoFo::PoDoFo)
install(TARGETS arianna-pdf-search DESTINATION ${KDE_INSTALL_LIBEXECDIR})
target_compile_definitions(arianna_static PRIVATE
    ARIANNA_PDF_SEARCH_HELPER="${KDE_INSTALL_FULL_LIBEXECDIR}/arianna-pdf-search"
)

if (BUILD_TESTING)
    add_subdirectory(autotests)
endif()

add_executable(arianna
    main.cpp
)

target_include_directories(arianna PRIVATE ${CMAKE_BINARY_DIR})
target_link_libraries(arianna PUBLIC arianna_static arianna_staticplugin)
add_dependencies(arianna arianna-pdf-search)

install(TARGETS arianna ${KDE_INSTALL_TARGETS_DEFAULT_ARGS})
