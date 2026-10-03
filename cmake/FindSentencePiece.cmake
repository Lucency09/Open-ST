# 将固定 vcpkg SentencePiece 端口的静态库与原始依赖闭包导入为唯一目标。
# 该端口没有 Config 文件；只在当前 installed tree 内解析，避免误用宿主其他版本。
find_path(SentencePiece_INCLUDE_DIR sentencepiece_processor.h
    PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/include" NO_DEFAULT_PATH)
find_library(SentencePiece_LIBRARY_RELEASE sentencepiece
    PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib" NO_DEFAULT_PATH)
find_library(SentencePiece_LIBRARY_DEBUG sentencepiece
    PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib" NO_DEFAULT_PATH)
include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(SentencePiece REQUIRED_VARS
    SentencePiece_INCLUDE_DIR SentencePiece_LIBRARY_RELEASE SentencePiece_LIBRARY_DEBUG)
if(SentencePiece_FOUND AND NOT TARGET open_st_sentencepiece)
    find_package(absl CONFIG REQUIRED)
    find_package(Protobuf CONFIG REQUIRED)
    find_package(Threads REQUIRED)
    add_library(open_st_sentencepiece STATIC IMPORTED GLOBAL)
    set_target_properties(open_st_sentencepiece PROPERTIES
        IMPORTED_CONFIGURATIONS "DEBUG;RELEASE"
        IMPORTED_LOCATION_DEBUG "${SentencePiece_LIBRARY_DEBUG}"
        IMPORTED_LOCATION_RELEASE "${SentencePiece_LIBRARY_RELEASE}"
        MAP_IMPORTED_CONFIG_RELWITHDEBINFO RELEASE
        MAP_IMPORTED_CONFIG_MINSIZEREL RELEASE
        INTERFACE_INCLUDE_DIRECTORIES "${SentencePiece_INCLUDE_DIR}"
        INTERFACE_LINK_LIBRARIES "absl::strings;absl::flags;absl::flags_parse;absl::log;absl::check;protobuf::libprotobuf-lite;Threads::Threads")
endif()
mark_as_advanced(SentencePiece_INCLUDE_DIR SentencePiece_LIBRARY_RELEASE SentencePiece_LIBRARY_DEBUG)
