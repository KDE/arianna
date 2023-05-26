# SPDX-FileCopyrightText: 2022 Carl Schwan <carlschwan@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

add_definitions(-DDATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

ecm_add_test(tableofcontenttest.cpp
    TEST_NAME tableofcontenttest
    LINK_LIBRARIES arianna_static Qt::Test
    NAME_PREFIX "arianna-"
)
