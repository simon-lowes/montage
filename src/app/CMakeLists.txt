# UI library: every panel except main.cpp, so individual files can be
# compiled (and unit tested) without linking the whole application.
file(GLOB MONTAGE_UI_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.h)
list(FILTER MONTAGE_UI_SOURCES EXCLUDE REGEX ".*/main\\.cpp$")

add_library(montage_ui STATIC ${MONTAGE_UI_SOURCES})
target_link_libraries(montage_ui PUBLIC montage_engine Qt6::Widgets Qt6::Multimedia Qt6::Concurrent)
target_include_directories(montage_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
set_target_properties(montage_ui PROPERTIES AUTOMOC ON)
target_compile_options(montage_ui PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -Wno-unused-parameter>)

add_executable(montage main.cpp)
target_link_libraries(montage PRIVATE montage_ui)
set_target_properties(montage PROPERTIES AUTOMOC ON)
install(TARGETS montage RUNTIME DESTINATION bin)
