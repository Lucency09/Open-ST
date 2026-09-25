# OCR 独占模型验证、像素转换与任务线程，白名单只从版本控制清单生成。
file(READ "${PROJECT_SOURCE_DIR}/packaging/ocr/models.json" OPEN_ST_OCR_MANIFEST)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/packaging/ocr/models.json")
string(JSON OPEN_ST_OCR_SCHEMA GET "${OPEN_ST_OCR_MANIFEST}" schemaVersion)
if(NOT OPEN_ST_OCR_SCHEMA EQUAL 1)
    message(FATAL_ERROR "Unsupported OCR model manifest schema")
endif()
string(JSON OPEN_ST_OCR_MODEL_COUNT LENGTH "${OPEN_ST_OCR_MANIFEST}" models)
if(NOT OPEN_ST_OCR_MODEL_COUNT EQUAL 6)
    message(FATAL_ERROR "OCR model manifest must contain exactly six models")
endif()
set(OPEN_ST_OCR_MODEL_RECORDS "")
set(OPEN_ST_OCR_MODEL_KEYS "")
foreach(index RANGE 0 5)
    foreach(field family language file sha256 size)
        string(JSON model_${field} GET "${OPEN_ST_OCR_MANIFEST}" models ${index} ${field})
    endforeach()
    string(LENGTH "${model_sha256}" hash_length)
    if(NOT model_family MATCHES "^(fast|best)$" OR NOT model_language MATCHES "^(chi_sim|eng|jpn)$"
        OR NOT model_file STREQUAL "resources/ocr/${model_family}/${model_language}.traineddata"
        OR NOT hash_length EQUAL 64 OR NOT model_sha256 MATCHES "^[0-9a-f]+$"
        OR NOT model_size MATCHES "^[1-9][0-9]*$" OR model_size GREATER 100000000)
        message(FATAL_ERROR "Invalid OCR model manifest entry")
    endif()
    set(model_key "${model_family}/${model_language}")
    if(model_key IN_LIST OPEN_ST_OCR_MODEL_KEYS)
        message(FATAL_ERROR "Duplicate OCR model manifest entry")
    endif()
    list(APPEND OPEN_ST_OCR_MODEL_KEYS "${model_key}")
    string(APPEND OPEN_ST_OCR_MODEL_RECORDS
        "    {\"${model_family}\", \"${model_language}\", \"${model_file}\", \"${model_sha256}\", ${model_size}ULL},\n")
endforeach()
configure_file(private/ocr_model_table.h.in "${CMAKE_CURRENT_BINARY_DIR}/generated/ocr_model_table.h" @ONLY)
add_library(open_st_ocr STATIC)
add_library(OpenST::OCR ALIAS open_st_ocr)
target_sources(open_st_ocr PRIVATE source/ocr_client.cpp source/ocr_engine.cpp source/ocr_models.cpp
    private/ocr_engine.h private/ocr_models.h
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/ocr_client.h)
target_include_directories(open_st_ocr PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/private"
    "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(open_st_ocr PUBLIC open_st_project_options PRIVATE OpenST::Common Tesseract::libtesseract)
