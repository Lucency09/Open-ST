add_library(open_st_export STATIC)
add_library(OpenST::Export ALIAS open_st_export)
target_sources(open_st_export
    PRIVATE
        source/clipboard_writer.cpp
        source/image_encoder.cpp
        source/image_file_writer.cpp
        private/clipboard_api.h
        private/file_write_api.h
        private/image_encoder.h
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES
        include/sdr_image_view.h
        include/clipboard_writer.h
        include/image_file_writer.h
)
target_include_directories(open_st_export PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/private")
target_link_libraries(open_st_export PUBLIC open_st_project_options PRIVATE windowscodecs ole32 oleaut32 user32)
