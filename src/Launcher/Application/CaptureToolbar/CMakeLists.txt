add_library(open_st_capture_toolbar STATIC)
add_library(OpenST::CaptureToolbar ALIAS open_st_capture_toolbar)
target_sources(open_st_capture_toolbar PRIVATE
    source/capture_toolbar.cpp source/toolbar_layout.cpp private/toolbar_layout.h
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/capture_toolbar.h)
target_include_directories(open_st_capture_toolbar PRIVATE private)
target_link_libraries(open_st_capture_toolbar PUBLIC open_st_project_options PRIVATE user32 gdi32 comctl32)
