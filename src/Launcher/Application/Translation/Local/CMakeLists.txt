# 本地引擎只有固定模型和计算依赖，无父级接口、网络或设置依赖。
file(READ "${PROJECT_SOURCE_DIR}/packaging/translation/models.json" manifest)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${PROJECT_SOURCE_DIR}/packaging/translation/models.json")
string(JSON schema GET "${manifest}" schemaVersion)
string(JSON count LENGTH "${manifest}" models)
if(NOT schema EQUAL 1 OR NOT count EQUAL 2)
    message(FATAL_ERROR "Local translation requires the fixed bilingual model manifest")
endif()
set(OPEN_ST_TRANSLATION_FILE_COUNT 0)
set(OPEN_ST_TRANSLATION_FILE_RECORDS "")
set(OPEN_ST_TRANSLATION_MODEL_RECORDS "")
set(model_directions "")
foreach(index RANGE 0 1)
    foreach(field direction sourceLanguage targetLanguage sourcePrefix maxSourceTokens maxOutputTokens)
        string(JSON model_${field} GET "${manifest}" models ${index} ${field})
    endforeach()
    if(NOT model_direction MATCHES "^(en-zh|zh-en)$" OR NOT model_maxSourceTokens EQUAL 512
        OR NOT model_maxOutputTokens EQUAL 512)
        message(FATAL_ERROR "Unreviewed local model identity or token budget")
    endif()
    if(model_direction IN_LIST model_directions)
        message(FATAL_ERROR "Duplicate local model direction")
    endif()
    list(APPEND model_directions "${model_direction}")
    if(model_direction STREQUAL "en-zh")
        if(NOT model_sourceLanguage STREQUAL "en" OR NOT model_targetLanguage STREQUAL "zh-CN"
            OR NOT model_sourcePrefix STREQUAL ">>cmn_Hans<<")
            message(FATAL_ERROR "Invalid simplified Chinese model language marker")
        endif()
    elseif(NOT model_sourceLanguage STREQUAL "zh-CN" OR NOT model_targetLanguage STREQUAL "en"
        OR NOT model_sourcePrefix STREQUAL "")
        message(FATAL_ERROR "Invalid Chinese to English model language marker")
    endif()
    string(APPEND OPEN_ST_TRANSLATION_MODEL_RECORDS
        "    {\"${model_direction}\", \"${model_sourceLanguage}\", \"${model_targetLanguage}\", \"${model_sourcePrefix}\", ${model_maxSourceTokens}, ${model_maxOutputTokens}},\n")
    string(JSON files LENGTH "${manifest}" models ${index} files)
    if(NOT files EQUAL 7)
        message(FATAL_ERROR "Local model must include five runtime files and both license records")
    endif()
    set(file_names "")
    math(EXPR last "${files} - 1")
    foreach(f RANGE 0 ${last})
        foreach(field file sha256 size)
            string(JSON file_${field} GET "${manifest}" models ${index} files ${f} ${field})
        endforeach()
        get_filename_component(name "${file_file}" NAME)
        string(LENGTH "${file_sha256}" hashLength)
        if(NOT file_file STREQUAL "resources/translation/${model_direction}/${name}"
            OR NOT name MATCHES "^(model.bin|config.json|shared_vocabulary.json|source.spm|target.spm|LICENSE|README.md)$"
            OR NOT hashLength EQUAL 64 OR NOT file_sha256 MATCHES "^[0-9a-f]+$"
            OR NOT file_size MATCHES "^[1-9][0-9]*$" OR file_size GREATER 200000000)
            message(FATAL_ERROR "Invalid fixed translation model file")
        endif()
        if(name IN_LIST file_names)
            message(FATAL_ERROR "Duplicate fixed translation model file")
        endif()
        list(APPEND file_names "${name}")
        string(APPEND OPEN_ST_TRANSLATION_FILE_RECORDS
            "    {\"${model_direction}\", \"${name}\", \"${file_file}\", \"${file_sha256}\", ${file_size}ULL},\n")
        math(EXPR OPEN_ST_TRANSLATION_FILE_COUNT "${OPEN_ST_TRANSLATION_FILE_COUNT} + 1")
    endforeach()
endforeach()
configure_file(private/translation_model_table.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/generated/translation_model_table.h" @ONLY)
add_library(open_st_translation_local STATIC)
add_library(OpenST::TranslationLocal ALIAS open_st_translation_local)
target_sources(open_st_translation_local PRIVATE source/translation_local.cpp
    source/translation_lines.cpp private/translation_lines.h
    source/translation_language.cpp private/translation_language.h
    PUBLIC FILE_SET HEADERS BASE_DIRS include FILES include/translation_local.h)
target_include_directories(open_st_translation_local PRIVATE
    "${CMAKE_CURRENT_BINARY_DIR}/generated" "${CMAKE_CURRENT_SOURCE_DIR}/private")
target_link_libraries(open_st_translation_local PUBLIC open_st_project_options
    PRIVATE OpenST::Common CTranslate2::ctranslate2 open_st_sentencepiece Elscore Normaliz)
