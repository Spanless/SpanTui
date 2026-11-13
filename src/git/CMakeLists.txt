add_library(SpanGit STATIC
    git_constants.h
    git_diff_store.cpp
    git_diff_store.h
    git_lane_scheduler.cpp
    git_lane_scheduler.h
    git_repository_diff.cpp
    git_repository_parsers.cpp
    git_repository_refresh.cpp
    git_repository_service.cpp
    git_repository_service.h
    git_worktree_commands.cpp
    git_worktree_commands.h
    git_worktree_layout.cpp
    git_worktree_layout.h
)
target_link_libraries(SpanGit PUBLIC SpanCore SpanPlatform Qt6::Concurrent)
# QML-facing types (src/application/runtime/qml_types.h).
qt_extract_metatypes(SpanGit)
