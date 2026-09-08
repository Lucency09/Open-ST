add_library(open_st_window_renderer STATIC)
add_library(OpenST::WindowRenderer ALIAS open_st_window_renderer)
target_sources(open_st_window_renderer
    PRIVATE source/window_renderer.cpp source/renderer_model.cpp private/renderer_model.h
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/window_renderer.h
)
target_link_libraries(open_st_window_renderer
    PUBLIC open_st_project_options nlohmann_json::nlohmann_json
    PRIVATE comctl32 user32 gdi32 dwmapi
)
target_include_directories(open_st_window_renderer PRIVATE private)
