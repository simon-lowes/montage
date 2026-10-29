# UI library: every panel except main.cpp, so individual files can be
# compiled (and unit tested) without linking the whole application.
file(GLOB MONTAGE_UI_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
list(FILTER MONTAGE_UI_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

add_library(montage_ui STATIC ${MONTAGE_UI_SOURCES})
target_link_libraries(montage_ui PUBLIC montage_engine Qt6::Widgets Qt6::Multimedia Qt6::Concurrent Qt6::Network)
target_include_directories(montage_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(montage_ui PROPERTIES AUTOMOC ON)
# Help › Licences…: Montage's licence and the third-party notices, built in (LicencesDialog).
qt_add_resources(montage_ui "licences" PREFIX "/montage/licences" BASE "${PROJECT_SOURCE_DIR}"
  FILES ${PROJECT_SOURCE_DIR}/LICENSE ${PROJECT_SOURCE_DIR}/LICENSE-EXCEPTION ${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md)
# Window › Language: the translations in translations/montage_<language>.ts (edited in Qt Linguist), built in as .qm
# files when Qt's lrelease is found, each with how far it is done (cmake/TranslationStatus.cmake).
# `cmake --build build --target montage_lupdate` brings every .ts file up to date with the tr() strings in the source
# (a new language: an empty translations/montage_<language>.ts first; see translations/README.md).
file(GLOB MONTAGE_TS_FILES CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/translations/montage_*.ts)
find_package(Qt6 QUIET COMPONENTS LinguistTools)
if(TARGET Qt6::lrelease)
  set(MONTAGE_LRELEASE $<TARGET_FILE:Qt6::lrelease>)
  set(MONTAGE_LUPDATE $<TARGET_FILE:Qt6::lupdate>)
else()
  set(_qt_bins "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_BINS}" "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_LIBEXECS}" /usr/lib/qt6/bin)
  find_program(MONTAGE_LRELEASE NAMES lrelease-qt6 lrelease HINTS ${_qt_bins})
  find_program(MONTAGE_LUPDATE NAMES lupdate-qt6 lupdate HINTS ${_qt_bins})
endif()
if(MONTAGE_LRELEASE AND MONTAGE_TS_FILES)
  set(_qm_files)
  foreach(ts ${MONTAGE_TS_FILES})
    get_filename_component(_name ${ts} NAME_WE)
    set(qm ${CMAKE_CURRENT_BINARY_DIR}/i18n/${_name}.qm)
    set(status ${CMAKE_CURRENT_BINARY_DIR}/i18n/${_name}.status)
    add_custom_command(OUTPUT ${qm} COMMAND ${MONTAGE_LRELEASE} -silent ${ts} -qm ${qm} DEPENDS ${ts} VERBATIM)
    add_custom_command(OUTPUT ${status} COMMAND ${CMAKE_COMMAND} -DTS=${ts} -DOUT=${status} -P ${PROJECT_SOURCE_DIR}/cmake/TranslationStatus.cmake
      DEPENDS ${ts} ${PROJECT_SOURCE_DIR}/cmake/TranslationStatus.cmake VERBATIM)
    list(APPEND _qm_files ${qm} ${status})
  endforeach()
  qt_add_resources(montage_ui "translations" PREFIX "/montage/i18n" BASE "${CMAKE_CURRENT_BINARY_DIR}/i18n" FILES ${_qm_files})
  list(LENGTH MONTAGE_TS_FILES _ts_count)
  message(STATUS "Translations: ${_ts_count}")
elseif(MONTAGE_TS_FILES)
  message(STATUS "Translations: off (Qt's lrelease was not found; Montage is in English)")
endif()
if(MONTAGE_LUPDATE)
  add_custom_target(montage_lupdate
    COMMAND ${MONTAGE_LUPDATE} -silent -locations none -no-obsolete ${PROJECT_SOURCE_DIR}/src -ts ${MONTAGE_TS_FILES}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR} VERBATIM)
endif()
target_compile_options(montage_ui PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wno-unused-parameter>)
# The HDR viewer (HdrSurface.h): Qt's RHI and its HDR swap chain information, from Qt 6.7 (a private module, which
# Qt 6.9 and later only load when asked for by name). Older Qt builds show HDR tone mapped to SDR, as before.
if(Qt6_VERSION VERSION_GREATER_EQUAL 6.7)
  if(NOT TARGET Qt6::GuiPrivate)
    set(QT_NO_PRIVATE_MODULE_WARNING ON)
    find_package(Qt6 QUIET COMPONENTS GuiPrivate)
  endif()
  if(TARGET Qt6::GuiPrivate)
    target_link_libraries(montage_ui PRIVATE Qt6::GuiPrivate)
    target_compile_definitions(montage_ui PUBLIC MONTAGE_HDR_VIEWER)
    if(WIN32)
      target_link_libraries(montage_ui PRIVATE dxgi)  # the viewer's own display, asked directly
    endif()
    message(STATUS "HDR viewer: on (Qt ${Qt6_VERSION})")
  else()
    message(STATUS "HDR viewer: off (Qt's private GUI module was not found)")
  endif()
else()
  message(STATUS "HDR viewer: off (Qt ${Qt6_VERSION}; it needs 6.7 or later)")
endif()

set(MONTAGE_PACKAGING_DIR ${PROJECT_SOURCE_DIR}/packaging)
set(MONTAGE_APP_SOURCES main.cpp ${MONTAGE_PACKAGING_DIR}/montage.qrc)
if(APPLE)
  set(MONTAGE_ICNS ${MONTAGE_PACKAGING_DIR}/macos/Montage.icns)
  set_source_files_properties(${MONTAGE_ICNS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
  list(APPEND MONTAGE_APP_SOURCES ${MONTAGE_ICNS})
elseif(WIN32)
  configure_file(${MONTAGE_PACKAGING_DIR}/windows/montage.rc.in ${CMAKE_CURRENT_BINARY_DIR}/montage.rc @ONLY)
  list(APPEND MONTAGE_APP_SOURCES ${CMAKE_CURRENT_BINARY_DIR}/montage.rc)
endif()

add_executable(montage ${MONTAGE_APP_SOURCES})
target_link_libraries(montage PRIVATE montage_ui)
montage_copy_runtime(montage)
set_target_properties(montage PROPERTIES
  AUTOMOC ON
  AUTORCC ON
  # A Montage.app bundle on macOS, a windowed (no console) montage.exe on Windows.
  MACOSX_BUNDLE ON
  WIN32_EXECUTABLE ON
  MACOSX_BUNDLE_INFO_PLIST ${MONTAGE_PACKAGING_DIR}/macos/Info.plist.in
  MACOSX_BUNDLE_BUNDLE_NAME Montage
  MACOSX_BUNDLE_GUI_IDENTIFIER com.simonlowes.montage
  MACOSX_BUNDLE_ICON_FILE Montage.icns
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
  MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
if(APPLE)
  set_target_properties(montage PROPERTIES OUTPUT_NAME Montage)
endif()
install(TARGETS montage RUNTIME DESTINATION bin BUNDLE DESTINATION .)
