add_library(SpanWorkspace STATIC
    file_icon_theme.cpp
    file_icon_theme.h
    file_tree_model.cpp
    file_tree_model.h
    file_tree_storage.cpp
    scope.h
    session_model.cpp
    session_model.h
    tile_grid.cpp
    tile_grid.h
    workspace_constants.h
    workspace_layout.cpp
    workspace_layout.h
    workspace_layout_json.cpp
    workspace_layout_json.h
)
target_link_libraries(SpanWorkspace
    PUBLIC SpanCore SpanPlatform Qt6::Concurrent Qt6::Gui
)
# QML-facing types (src/application/runtime/qml_types.h).
qt_extract_metatypes(SpanWorkspace)
