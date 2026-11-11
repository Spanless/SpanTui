add_library(SpanMonitoring STATIC
    gpu_engine_metrics.cpp
    gpu_engine_metrics.h
    resource_monitor.cpp
    resource_monitor.h
)
target_link_libraries(SpanMonitoring
    PUBLIC SpanCore
    PRIVATE psapi pdh
)
# QML-facing types (src/application/runtime/qml_types.h).
qt_extract_metatypes(SpanMonitoring)
