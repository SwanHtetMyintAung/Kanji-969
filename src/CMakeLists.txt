# This is the cmake file of src folder

# 1. Collect all .cpp files in this folder 
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS
    *.cpp
    *.c
    *.h
    *.hpp
)

# 2. Define the executable (using the project name from root)
add_executable(${PROJECT_NAME} ${SOURCES} ${CMAKE_SOURCE_DIR}/Dependencies/qrgen/qrcodegen.c ${CMAKE_SOURCE_DIR}/resources/app.rc )

# NOTE: This line forces cmake to generate exe file without a console popping up in release mode.
target_link_options(${PROJECT_NAME} PRIVATE
    "$<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>"
)

# NOTE: This forces exe to output in modified path
set_target_properties(${PROJECT_NAME} PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/${PROJECT_NAME}/$<CONFIG>-$<PLATFORM_ID>"
)


# NOTE: XML EMBEDDING
set(XML_EMBED_SCRIPT
    "${CMAKE_SOURCE_DIR}/emb_xml.py"
)

set(XML_SOURCE
    "${CMAKE_SOURCE_DIR}/assets/window.xml"
)

set(XML_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/Release"
)

set(XML_GENERATED_HEADER
    "${XML_GENERATED_DIR}/window_embedded.h"
)

# NOTE: JSON EMBEDDING
set(JSON_EMBED_SCRIPT
    "${CMAKE_SOURCE_DIR}/json_to_c.py"
)

set(JSON_SOURCE
    "${CMAKE_SOURCE_DIR}/assets/data/soumatome_n3_kanji_with_hiragana.json"
)

set(JSON_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/Release"
)

set(JSON_GENERATED_HEADER
    "${JSON_GENERATED_DIR}/soumatome_json.h"
)

# NOTE: WALLPAPER EMBEDDING
set(IMAGE_EMBED_SCRIPT
    "${CMAKE_SOURCE_DIR}/embed_image.py"
)

set(IMAGE_SOURCE
    "${CMAKE_SOURCE_DIR}/assets/wallpaper.jpg"
)

set(IMAGE_GENERATED_DIR
    "${CMAKE_CURRENT_BINARY_DIR}/generated/Release"
)

set(IMAGE_GENERATED_HEADER
    "${IMAGE_GENERATED_DIR}/wallpaper.h"
)

add_custom_command(
    OUTPUT "${XML_GENERATED_HEADER}"

    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${XML_GENERATED_DIR}"

    COMMAND "${Python3_EXECUTABLE}"
            "${XML_EMBED_SCRIPT}"
            --source "${XML_SOURCE}"
            --output "${XML_GENERATED_HEADER}"

    MAIN_DEPENDENCY "${XML_SOURCE}"

    DEPENDS
        "${XML_EMBED_SCRIPT}"

    COMMENT "Embedding window.xml into C header..."

    VERBATIM
)

add_custom_command(
    OUTPUT "${JSON_GENERATED_HEADER}"

    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${JSON_GENERATED_DIR}"

    COMMAND "${Python3_EXECUTABLE}"
            "${JSON_EMBED_SCRIPT}"
            "${JSON_SOURCE}"
            "${JSON_GENERATED_HEADER}"
            --name "${soumatome_json}"

    MAIN_DEPENDENCY "${JSON_SOURCE}"

    DEPENDS
        "${JSON_EMBED_SCRIPT}"

    COMMENT "Embedding json into C header..."

    VERBATIM
)

add_custom_command(
    OUTPUT "${IMAGE_GENERATED_HEADER}"

    COMMAND "${CMAKE_COMMAND}" -E make_directory
            "${IMAGE_GENERATED_DIR}"

    COMMAND "${Python3_EXECUTABLE}"
            "${IMAGE_EMBED_SCRIPT}"
            "${IMAGE_SOURCE}"
            -o "${IMAGE_GENERATED_HEADER}"

    MAIN_DEPENDENCY "${IMAGE_SOURCE}"

    DEPENDS
        "${IMAGE_EMBED_SCRIPT}"

    COMMENT "Embedding wallpaper into C header..."

    VERBATIM
)

# Only add the generated header to Release builds.
target_sources(${PROJECT_NAME} PRIVATE
    "$<$<CONFIG:Release>:${XML_GENERATED_HEADER}>"
    "$<$<CONFIG:Release>:${JSON_GENERATED_HEADER}>"
    "$<$<CONFIG:Release>:${IMAGE_GENERATED_HEADER}>"
)

# Make the generated header discoverable by #include.
target_include_directories(${PROJECT_NAME} PRIVATE
    "$<$<CONFIG:Release>:${XML_GENERATED_DIR}>"
    "$<$<CONFIG:Release>:${JSON_GENERATED_DIR}>"
    "$<$<CONFIG:Release>:${IMAGE_GENERATED_DIR}>"
)

# Custom compile definition for the embedded XML build.
target_compile_definitions(${PROJECT_NAME} PRIVATE
    "$<$<CONFIG:Release>:APP_EMBED_XML>"
)
# 3. Include directories
# We use PARENT_SCOPE or absolute paths to find the Dependencies folder
# NOTE: This is include path, you put the directory of header here when you want the compiler to automatically find the header files, not necessary for now
#target_include_directories(${PROJECT_NAME} PRIVATE 
#    "${CMAKE_SOURCE_DIR}/Dependencies/Raven"
#)

# 4. Link everything
target_link_libraries(${PROJECT_NAME} PRIVATE 
    raven
)

# NOTE: I put this to make sure just in case you didn't run emb_xml.py, this copy the entire assets folder into the executable directory
add_custom_target(copy_assets ALL
   COMMAND ${CMAKE_COMMAND} -E copy_directory
           "${CMAKE_SOURCE_DIR}/assets"
           "${CMAKE_BINARY_DIR}/${PROJECT_NAME}/$<CONFIG>-$<PLATFORM_ID>/assets"
)

add_dependencies(${PROJECT_NAME} copy_assets)
