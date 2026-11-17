# SPDX-FileCopyrightText: 2022 Carl Schwan <carlschwan@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

add_definitions(-DDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

ecm_add_test(referencetargetmodeltest.cpp ../referencetargetmodel.cpp
    TEST_NAME referencetargetmodeltest
    LINK_LIBRARIES Qt6::Core Qt6::Qml Qt6::Test
    NAME_PREFIX "arianna-"
)
target_include_directories(referencetargetmodeltest PRIVATE ..)

ecm_add_test(ainotetest.cpp
    TEST_NAME ainotetest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt6::Test KF6::Archive
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-ainotetest PROPERTIES TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu")
add_dependencies(ainotetest codexclienttest)
target_compile_definitions(ainotetest PRIVATE CODEX_TEST_PEER_PATH="$<TARGET_FILE:codexclienttest>")

# Standalone transport tests do not depend on document or speech components.
ecm_add_test(codexclienttest.cpp ../codexclient.cpp
    TEST_NAME codexclienttest
    LINK_LIBRARIES Qt6::Core Qt6::Qml Qt6::Network Qt6::Test KF6::I18n
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-codexclienttest PROPERTIES TIMEOUT 90)

ecm_add_test(readercontexttest.cpp
    TEST_NAME readercontexttest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt6::Test
    NAME_PREFIX "arianna-"
)
target_compile_definitions(readercontexttest PRIVATE ARIANNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_tests_properties(arianna-readercontexttest PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
)

ecm_add_test(codexqmltest.cpp
    TEST_NAME codexqmltest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt6::Test KF6::Archive
    NAME_PREFIX "arianna-"
)
add_dependencies(codexqmltest codexclienttest)
target_compile_definitions(codexqmltest PRIVATE CODEX_TEST_PEER_PATH="$<TARGET_FILE:codexclienttest>")
set_tests_properties(arianna-codexqmltest PROPERTIES
    TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
)

find_program(DBUS_RUN_SESSION_EXECUTABLE dbus-run-session REQUIRED)
add_executable(sigilnavigationtest sigilnavigationtest.cpp)
target_link_libraries(sigilnavigationtest PRIVATE arianna_static Qt6::Test Qt6::DBus)
add_test(NAME arianna-sigilnavigationtest COMMAND ${DBUS_RUN_SESSION_EXECUTABLE} -- $<TARGET_FILE:sigilnavigationtest>)
set_tests_properties(arianna-sigilnavigationtest PROPERTIES TIMEOUT 60)

ecm_add_test(epubmathtest.cpp
    TEST_NAME epubmathtest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt::Test KF6::Archive
    NAME_PREFIX "arianna-"
)

ecm_add_test(epubfootnotetest.cpp
    TEST_NAME epubfootnotetest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt6::Test KF6::Archive
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-epubfootnotetest PROPERTIES TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu")
set_tests_properties(arianna-epubmathtest PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
)

ecm_add_test(bookimporttest.cpp
    TEST_NAME bookimporttest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt::Test KF6::Archive
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-bookimporttest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

ecm_add_test(bookformatstest.cpp
    TEST_NAME bookformatstest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt::Test
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-bookformatstest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

ecm_add_test(librarymaintenancetest.cpp
    TEST_NAME librarymaintenancetest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt::Test KF6::Archive
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-librarymaintenancetest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

ecm_add_test(bookreferencechecktest.cpp
    TEST_NAME bookreferencechecktest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt::Test KF6::Archive
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-bookreferencechecktest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

ecm_add_test(tableofcontenttest.cpp
    TEST_NAME tableofcontenttest
    LINK_LIBRARIES arianna_static Qt::Test
    NAME_PREFIX "arianna-"
)

ecm_add_test(epubcontainertest.cpp
    TEST_NAME epubcontainertest
    LINK_LIBRARIES arianna_static Qt::Test KF6::Archive
    NAME_PREFIX "arianna-"
)
set_property(TARGET epubcontainertest APPEND PROPERTY AUTOMOC_MOC_OPTIONS -DQ_MOC_RUN)

ecm_add_test(epubimageinversiontest.cpp
    TEST_NAME epubimageinversiontest
    LINK_LIBRARIES arianna_static Qt::Test
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-epubimageinversiontest PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
)

ecm_add_test(bookserverreferencetest.cpp
    TEST_NAME bookserverreferencetest
    LINK_LIBRARIES arianna_static Qt::Test KF6::Archive
    NAME_PREFIX "arianna-"
)

ecm_add_test(bookservercachetest.cpp
    TEST_NAME bookservercachetest
    LINK_LIBRARIES arianna_static Qt::Test KF6::Archive
    NAME_PREFIX "arianna-"
)

ecm_add_test(epubreferenceanchortest.cpp
    TEST_NAME epubreferenceanchortest
    LINK_LIBRARIES arianna_static Qt::Test
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-epubreferenceanchortest PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
)

ecm_add_test(categoryentriesmodeltest.cpp
    TEST_NAME categoryentriesmodeltest
    LINK_LIBRARIES arianna_static Qt::Test
    NAME_PREFIX "arianna-"
)

ecm_add_test(booktruthstoretest.cpp
    TEST_NAME booktruthstoretest
    LINK_LIBRARIES arianna_static Qt::Test KF6::Archive
    NAME_PREFIX "arianna-"
)

ecm_add_test(bookstateimporttest.cpp
    TEST_NAME bookstateimporttest
    LINK_LIBRARIES arianna_static Qt::Test KF6::Archive
    NAME_PREFIX "arianna-"
)

ecm_add_test(pdfwatermarkfiltertest.cpp
    TEST_NAME pdfwatermarkfiltertest
    LINK_LIBRARIES arianna_static Qt::Test
    NAME_PREFIX "arianna-"
)

ecm_add_test(pdfoutlineinsertiontest.cpp
    TEST_NAME pdfoutlineinsertiontest
    LINK_LIBRARIES arianna_static Qt::Test
    NAME_PREFIX "arianna-"
)

ecm_add_test(pdflinkannotationtest.cpp
    TEST_NAME pdflinkannotationtest
    LINK_LIBRARIES arianna_static Qt::Test
    NAME_PREFIX "arianna-"
)

ecm_add_test(searchnavigationtest.cpp
    TEST_NAME searchnavigationtest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt::Test KF6::Archive
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-searchnavigationtest PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
)

ecm_add_test(pdfreferenceviewtest.cpp
    TEST_NAME pdfreferenceviewtest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt::Test
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-pdfreferenceviewtest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

ecm_add_test(pdfannotationviewtest.cpp
    TEST_NAME pdfannotationviewtest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt::Test
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-pdfannotationviewtest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")

ecm_add_test(pdfsecurityinfotest.cpp
    TEST_NAME pdfsecurityinfotest
    LINK_LIBRARIES arianna_static Qt::Test
    NAME_PREFIX "arianna-"
)

ecm_add_test(pdfversionupdatertest.cpp
    TEST_NAME pdfversionupdatertest
    LINK_LIBRARIES arianna_static Qt::Test
    NAME_PREFIX "arianna-"
)

ecm_add_test(pdfcheckcommandtest.cpp
    TEST_NAME pdfcheckcommandtest
    LINK_LIBRARIES arianna_static Qt::Test
    NAME_PREFIX "arianna-"
)

ecm_add_test(pdfvalidationreportparsertest.cpp
    TEST_NAME pdfvalidationreportparsertest
    LINK_LIBRARIES arianna_static Qt::Test
    NAME_PREFIX "arianna-"
)

ecm_add_test(readersettingstest.cpp
    TEST_NAME readersettingstest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt::Test
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-readersettingstest PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
add_test(NAME arianna-readersettingsmigrationtest COMMAND readersettingstest)
set_tests_properties(arianna-readersettingsmigrationtest PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;ARIANNA_TEST_LEGACY_DATABASE=1"
)

ecm_add_test(translatortest.cpp
    TEST_NAME translatortest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt::Test
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-translatortest PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
)

ecm_add_test(librarysearchtest.cpp
    TEST_NAME librarysearchtest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt::Test KF6::Archive
    NAME_PREFIX "arianna-"
)
add_dependencies(librarysearchtest arianna-pdf-search)
target_compile_definitions(librarysearchtest PRIVATE ARIANNA_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
set_tests_properties(arianna-librarysearchtest PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
)

ecm_add_test(speechcontrollertest.cpp
    TEST_NAME speechcontrollertest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt::Test KF6::Archive
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-speechcontrollertest PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
    TIMEOUT 120
)

ecm_add_test(workstructuretest.cpp
    TEST_NAME workstructuretest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt6::Test KF6::Archive
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-workstructuretest PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu"
)

# Session-only EPUB editing must not enter the library or the state-import path.
ecm_add_test(nolibraryimporttest.cpp
    TEST_NAME nolibraryimporttest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt6::Test KF6::Archive
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-nolibraryimporttest PROPERTIES TIMEOUT 60
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu")

ecm_add_test(readerhistorytest.cpp
    TEST_NAME readerhistorytest
    LINK_LIBRARIES arianna_static arianna_staticplugin Qt6::Test KF6::Archive
    NAME_PREFIX "arianna-"
)
set_tests_properties(arianna-readerhistorytest PROPERTIES TIMEOUT 90
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software;QTWEBENGINE_DISABLE_SANDBOX=1;QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu")

find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME arianna-readercontrolintegration
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/readercontrolintegration.py --binary $<TARGET_FILE:arianna>)
set_tests_properties(arianna-readercontrolintegration PROPERTIES TIMEOUT 160)

add_executable(pdfpasswordtest pdfpasswordtest.cpp)
target_link_libraries(pdfpasswordtest PRIVATE arianna_static arianna_staticplugin Qt6::Test Qt6::DBus)
add_test(NAME arianna-pdfpasswordtest
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_private_dbus_test.py $<TARGET_FILE:pdfpasswordtest>)
set_tests_properties(arianna-pdfpasswordtest PROPERTIES TIMEOUT 120
    ENVIRONMENT "ARIANNA_PRIVATE_WALLET_TEST=1;QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software")
