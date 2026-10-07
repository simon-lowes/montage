# UI library: every panel except main.cpp, so individual files can be
# compiled (and unit tested) without linking the whole application.
file(GLOB MONTAGE_UI_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
list(FILTER MONTAGE_UI_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

add_library(montage_ui STATIC ${MONTAGE_UI_SOURCES})
target_link_libraries(montage_ui PUBLIC montage_engine Qt6::Widgets Qt6::Multimedia Qt6::Concurrent Qt6::Network)
target_include_directories(montage_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/.. ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(montage_ui PROPERTIES AUTOMOC ON)
target_compile_options(montage_ui PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wno-unused-parameter>)

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
