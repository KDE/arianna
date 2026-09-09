# SPDX-FileCopyrightText: 2021 Carl Schwan <carlschwan@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

cmake_minimum_required(VERSION 3.16)

# KDE Applications version, managed by release script.
set(RELEASE_SERVICE_VERSION_MAJOR "26")
set(RELEASE_SERVICE_VERSION_MINOR "07")
set(RELEASE_SERVICE_VERSION_MICRO "70")
set(RELEASE_SERVICE_VERSION "${RELEASE_SERVICE_VERSION_MAJOR}.${RELEASE_SERVICE_VERSION_MINOR}.${RELEASE_SERVICE_VERSION_MICRO}")

project(arianna VERSION ${RELEASE_SERVICE_VERSION})

set(KF_MIN_VERSION "6.12.0")
set(QT_MIN_VERSION "6.8.0")

find_package(ECM ${KF_MIN_VERSION} REQUIRED NO_MODULE)
find_package(PkgConfig REQUIRED)
pkg_check_modules(PODOFO REQUIRED libpodofo>=1.1.1)
find_library(PODOFO_LIBRARY
    NAMES podofo
    HINTS ${PODOFO_LIBRARY_DIRS}
    NO_DEFAULT_PATH
)
if (NOT PODOFO_LIBRARY)
    message(FATAL_ERROR "PoDoFo ${PODOFO_VERSION} was found, but its library could not be resolved in ${PODOFO_LIBRARY_DIRS}.")
endif()

add_library(PoDoFo::PoDoFo UNKNOWN IMPORTED)
set_target_properties(PoDoFo::PoDoFo PROPERTIES
    IMPORTED_LOCATION "${PODOFO_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PODOFO_INCLUDE_DIRS}"
)

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
include(ECMDeprecationSettings)
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

find_package(Qt6 ${QT_MIN_VERSION} NO_MODULE COMPONENTS Widgets Core Quick Gui QuickControls2 WebSockets WebChannel Test Svg Sql Xml HttpServer PrintSupport)
set_package_properties(Qt6 PROPERTIES
    TYPE REQUIRED
    PURPOSE "Basic application components"
)
qt_policy(SET QTP0001 NEW)
qt_policy(SET QTP0004 NEW)
find_package(KF6 ${KF_MIN_VERSION} COMPONENTS Kirigami I18n Config CoreAddons DBusAddons WindowSystem FileMetaData Archive QuickCharts Crash ColorScheme KIO XmlGui)
set_package_properties(KF6 PROPERTIES
    TYPE REQUIRED
    PURPOSE "Basic application components"
)
find_package(KF6KirigamiAddons 1.4.0 REQUIRED)
find_package(KF6QQC2DesktopStyle REQUIRED)

find_package(Qt6 ${QT_MIN_VERSION} NO_MODULE COMPONENTS WebEngineQuick)

if (NOT WIN32)
    find_package(KF6 ${KF_MIN_VERSION} COMPONENTS Baloo)
endif()

ecm_find_qmlmodule(QtWebChannel)
ecm_set_disabled_deprecation_versions(QT 6.11.0
    KF 6.26.0
)

set(ARIANNA_OKULAR_SOURCE_DIR "" CACHE PATH "Path to an Okular source checkout used by the experimental PDF reader")
if (NOT ARIANNA_OKULAR_SOURCE_DIR OR NOT EXISTS "${ARIANNA_OKULAR_SOURCE_DIR}/core/document.h")
    unset(ARIANNA_OKULAR_SOURCE_DIR CACHE)
    find_path(ARIANNA_OKULAR_SOURCE_DIR
        NAMES core/document.h core/page.h
        HINTS "$ENV{OKULAR_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/../okular" "/home/markus/Projekte/okular"
    )
endif()

set(ARIANNA_OKULAR_BUILD_DIR "" CACHE PATH "Path to the matching Okular build tree")
if ((NOT ARIANNA_OKULAR_BUILD_DIR OR NOT EXISTS "${ARIANNA_OKULAR_BUILD_DIR}/core/okularcore_export.h") AND ARIANNA_OKULAR_SOURCE_DIR)
    unset(ARIANNA_OKULAR_BUILD_DIR CACHE)
    find_path(ARIANNA_OKULAR_BUILD_DIR
        NAMES core/okularcore_export.h settings_core.h
        HINTS "$ENV{OKULAR_BUILD_DIR}" "${ARIANNA_OKULAR_SOURCE_DIR}/build"
    )
endif()

find_library(ARIANNA_OKULAR_CORE_LIBRARY
    NAMES Okular6Core
    HINTS
        "${ARIANNA_OKULAR_BUILD_DIR}/bin"
        "${ARIANNA_OKULAR_BUILD_DIR}/lib"
        "${ARIANNA_OKULAR_BUILD_DIR}/lib/x86_64-linux-gnu"
)

if (NOT ARIANNA_OKULAR_SOURCE_DIR OR NOT ARIANNA_OKULAR_BUILD_DIR OR NOT ARIANNA_OKULAR_CORE_LIBRARY)
    message(FATAL_ERROR "The Okular PDF reader needs ARIANNA_OKULAR_SOURCE_DIR, ARIANNA_OKULAR_BUILD_DIR and libOkular6Core.")
endif()

add_library(Okular::Core SHARED IMPORTED GLOBAL)
set_target_properties(Okular::Core PROPERTIES
    IMPORTED_LOCATION "${ARIANNA_OKULAR_CORE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${ARIANNA_OKULAR_SOURCE_DIR};${ARIANNA_OKULAR_BUILD_DIR};${ARIANNA_OKULAR_BUILD_DIR}/core"
)

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

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES)

file(GLOB_RECURSE ALL_CLANG_FORMAT_SOURCE_FILES src/*.cpp src/*.h)
kde_clang_format(${ALL_CLANG_FORMAT_SOURCE_FILES})
kde_configure_git_pre_commit_hook(CHECKS CLANG_FORMAT)
