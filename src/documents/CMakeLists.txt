add_library(SpanDocuments STATIC
    code_preview_line_model.cpp
    code_preview_line_model.h
    document_manager.cpp
    document_manager.h
    document_monitoring.cpp
    document_persistence.cpp
    editor_controller.cpp
    editor_controller.h
    markdown_document_renderer.cpp
    markdown_document_renderer.h
    markdown_gfm_adapter.cpp
    markdown_gfm_adapter.h
    markdown_network_policy.cpp
    markdown_network_policy.h
    markdown_remote_image_loader.cpp
    markdown_remote_image_loader.h
)
target_link_libraries(SpanDocuments
    PUBLIC SpanCore Qt6::Concurrent Qt6::Gui Qt6::Network Qt6::Quick Qt6::Sql
)
# QML-facing types (src/application/runtime/qml_types.h).
qt_extract_metatypes(SpanDocuments)
