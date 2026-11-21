add_library(SpanMonitoring STATIC
    gpu_engine_metrics.cpp
    gpu_engine_metrics.h
    platform_resource_sampler.h
    resource_monitor.cpp
    resource_monitor.h
)
target_link_libraries(SpanMonitoring PUBLIC SpanCore)
if(WIN32)
    target_sources(SpanMonitoring PRIVATE windows/platform_resource_sampler.cpp)
    target_link_libraries(SpanMonitoring PRIVATE psapi pdh)
else()
    target_sources(SpanMonitoring PRIVATE macos/platform_resource_sampler.cpp)
endif()
# QML-facing types (src/application/runtime/qml_types.h).
qt_extract_metatypes(SpanMonitoring)
