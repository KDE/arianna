# SPDX-FileCopyrightText: 2021 Carl Schwan <carlschwan@kde.org>
# SPDX-License-Identifier: BSD-2-Clause

cmake_minimum_required(VERSION 3.16)

set(PROJECT_VERSION "0.1")
project(arianna VERSION ${PROJECT_VERSION})

set(KF5_MIN_VERSION "5.98.0")
set(QT_MIN_VERSION "5.15.0")

find_package(ECM ${KF5_MIN_VERSION} REQUIRED NO_MODULE)

set(CMAKE_MODULE_PATH ${ECM_MODULE_PATH} ${CMAKE_SOURCE_DIR}/cmake)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FeatureSummary)
include(ECMSetupVersion)
include(KDEInstallDirs)
include(ECMQMLModules)
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
    VERSION_HEADER ${CMAKE_CURRENT_BINARY_DIR}/arianna-version.h
)

find_package(Qt${QT_MAJOR_VERSION} ${QT_MIN_VERSION} NO_MODULE COMPONENTS Widgets Core Quick Gui QuickControls2 WebSockets Test Svg Sql WebEngine)
set_package_properties(Qt${QT_MAJOR_VERSION} PROPERTIES
    TYPE REQUIRED
    PURPOSE "Basic application components"
)
find_package(KF5 ${KF5_MIN_VERSION} COMPONENTS Kirigami2 I18n Config CoreAddons DBusAddons WindowSystem FileMetaData Baloo Archive)
set_package_properties(KF5 PROPERTIES
    TYPE REQUIRED
    PURPOSE "Basic application components"
)
set_package_properties(KF5Kirigami2 PROPERTIES
    TYPE REQUIRED
    PURPOSE "Kirigami application UI framework"
)
find_package(KF5KirigamiAddons 0.6 REQUIRED)
find_package(KF5QQC2DesktopStyle REQUIRED)

add_definitions(-DQT_NO_FOREACH -DQT_NO_KEYWORDS)

add_subdirectory(src)

ki18n_install(po)

install(FILES org.kde.arianna.desktop DESTINATION ${KDE_INSTALL_APPDIR})
install(FILES org.kde.arianna.appdata.xml DESTINATION ${KDE_INSTALL_METAINFODIR})

ecm_qt_install_logging_categories(
    EXPORT ARIANNA
    FILE tokodon.categories
    SORT DESTINATION ${KDE_INSTALL_LOGGINGCATEGORIESDIR}
)

feature_summary(WHAT ALL INCLUDE_QUIET_PACKAGES FATAL_ON_MISSING_REQUIRED_PACKAGES)

file(GLOB_RECURSE ALL_CLANG_FORMAT_SOURCE_FILES src/*.cpp src/*.h)
kde_clang_format(${ALL_CLANG_FORMAT_SOURCE_FILES})
kde_configure_git_pre_commit_hook(CHECKS CLANG_FORMAT)
