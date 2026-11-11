add_library(SpanUpdate STATIC
    github_release.cpp
    github_release.h
    installer_source.cpp
    installer_source.h
    update_service.cpp
    update_service.h
)
target_link_libraries(SpanUpdate PUBLIC SpanCore SpanPlatform Qt6::Concurrent Qt6::Network)
# QML-facing types (src/application/runtime/qml_types.h).
qt_extract_metatypes(SpanUpdate)
