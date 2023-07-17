# SPDX-FileCopyrightText: 2021 Carl Schwan <carlschwan@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

cmake_minimum_required(VERSION 3.16)

# KDE Applications version, managed by release script.
set(RELEASE_SERVICE_VERSION_MAJOR "23")
set(RELEASE_SERVICE_VERSION_MINOR "11")
set(RELEASE_SERVICE_VERSION_MICRO "70")
set(RELEASE_SERVICE_VERSION "${RELEASE_SERVICE_VERSION_MAJOR}.${RELEASE_SERVICE_VERSION_MINOR}.${RELEASE_SERVICE_VERSION_MICRO}")

project(arianna VERSION ${RELEASE_SERVICE_VERSION})

set(KF_MIN_VERSION "5.98.0")
set(QT_MIN_VERSION "5.15.0")

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)

set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FeatureSummary)
include(ECMSetupVersion)
include(KDEInstallDirs)
include(ECMFindQmlModule)
include(KDEClangFormat)
include(ECMQtDeclareLoggingCategory)
include(KDECMakeSettings)
include(KDECompilerSettings NO_POLICY_SCOPE)
include(ECMAddAppIcon)
include(KDEGitCommitHooks)
include(ECMAddTests)
include(ECMCheckOutboundLicense)
include(KDEClangFormat)

ecm_setup_version(${PROJECT_VERSION}
    VARIABLE_PREFIX ARIANNA
    SOVERSION ${PROJECT_VERSION_MAJOR}
    VERSION_HEADER ${CMAKE_CURRENT_BINARY_DIR}/arianna-version.h
)

find_package(Qt${QT_MAJOR_VERSION} ${QT_MIN_VERSION} NO_MODULE COMPONENTS Widgets Core Quick Gui QuickControls2 WebSockets WebChannel Test Svg Sql Xml)
set_package_properties(Qt${QT_MAJOR_VERSION} PROPERTIES
    TYPE REQUIRED
    PURPOSE "Basic application components"
)
find_package(KF${QT_MAJOR_VERSION} ${KF_MIN_VERSION} COMPONENTS Kirigami2 I18n Config CoreAddons DBusAddons WindowSystem FileMetaData Archive QuickCharts)
set_package_properties(KF${QT_MAJOR_VERSION} PROPERTIES
    TYPE REQUIRED
    PURPOSE "Basic application components"
)
set_package_properties(KF${QT_MAJOR_VERSION}Kirigami2 PROPERTIES
    TYPE REQUIRED
    PURPOSE "Kirigami application UI framework"
)
find_package(KF${QT_MAJOR_VERSION}KirigamiAddons 0.8 REQUIRED)
find_package(KF${QT_MAJOR_VERSION}QQC2DesktopStyle REQUIRED)

if (${QT_MAJOR_VERSION} STREQUAL 5)
    find_package(Qt${QT_MAJOR_VERSION} ${QT_MIN_VERSION} NO_MODULE COMPONENTS WebEngine)
else()
    find_package(Qt${QT_MAJOR_VERSION} ${QT_MIN_VERSION} NO_MODULE COMPONENTS WebEngineQuick)
endif()

if (NOT WIN32)
    find_package(KF${QT_MAJOR_VERSION} ${KF_MIN_VERSION} COMPONENTS Baloo)
endif()

add_definitions(-DQT_NO_FOREACH -DQT_NO_KEYWORDS)

add_subdirectory(src)

ki18n_install(po)

install(FILES org.kde.arianna.desktop DESTINATION ${KDE_INSTALL_APPDIR})
install(FILES org.kde.arianna.appdata.xml DESTINATION ${KDE_INSTALL_METAINFODIR})
install(FILES org.kde.arianna.svg DESTINATION ${KDE_INSTALL_FULL_ICONDIR}/hicolor/scalable/apps)

ecm_qt_install_logging_categories(
    EXPORT ARIANNA
    FILE arianna.categories
    SORT DESTINATION ${KDE_INSTALL_LOGGINGCATEGORIESDIR}
)

feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)

file(GLOB_RECURSE ALL_CLANG_FORMAT_SOURCE_FILES src/*.cpp src/*.h)
kde_clang_format(${ALL_CLANG_FORMAT_SOURCE_FILES})
kde_configure_git_pre_commit_hook(CHECKS CLANG_FORMAT)
