# 配置独立标准库标注对象模块。
add_library(open_st_annotation STATIC)
add_library(OpenST::Annotation ALIAS open_st_annotation)
target_sources(open_st_annotation
    PRIVATE source/annotation.cpp source/annotation_document.cpp source/annotation_text.cpp
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/annotation.h include/annotation_document.h
)
target_link_libraries(open_st_annotation PUBLIC open_st_project_options)
