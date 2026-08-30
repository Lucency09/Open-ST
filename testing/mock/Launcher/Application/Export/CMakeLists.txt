add_library(open_st_export_mocks INTERFACE)
add_library(OpenST::ExportMocks ALIAS open_st_export_mocks)
target_include_directories(open_st_export_mocks INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${PROJECT_SOURCE_DIR}/src/Launcher/Application/Export/private"
)
target_link_libraries(open_st_export_mocks INTERFACE OpenST::Export)
