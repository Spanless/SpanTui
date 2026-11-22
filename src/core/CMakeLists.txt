add_library(SpanCore STATIC
    core_constants.h
    external_tool_locations.h
    external_tools.cpp
    external_tools.h
    operation_outcome.h
    platform_folders.h
    product_identity.cpp
    product_identity.h
    product_paths.cpp
    product_paths.h
    workspace_database_migration.cpp
    workspace_database_migration.h
)
target_include_directories(SpanCore PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(SpanCore PUBLIC Qt6::Core PRIVATE Qt6::Sql)
# Each module keeps one implementation per platform under windows/ and
# macos/ behind a neutral header (docs/architecture/overview.md).
if(WIN32)
    target_sources(SpanCore PRIVATE
        windows/external_tool_locations.cpp
        windows/platform_folders.cpp
    )
    # One Windows SDK floor for every module that includes Win32 headers.
    target_compile_definitions(SpanCore PUBLIC _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006)
else()
    target_sources(SpanCore PRIVATE
        macos/external_tool_locations.cpp
        macos/platform_folders.cpp
    )
endif()
