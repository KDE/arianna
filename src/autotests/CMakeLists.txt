# SPDX-FileCopyrightText: 2022 Carl Schwan <carlschwan@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

add_definitions(-DDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

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
