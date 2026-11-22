#pragma once

#include "monitoring/resource_monitor.h"

#include <memory>

// The operating system's sampler behind ResourceMonitor: CPU and memory of
// SpanTui and of each session's process tree, plus GPU where the platform
// exposes per-process engine utilisation (Windows only; elsewhere 0).
std::shared_ptr<ResourceSampleProvider> createPlatformResourceSampleProvider();

// Installed RAM in bytes; 0 when unknown.
qint64 installedPhysicalMemoryBytes();

// SpanTui's own process, for the diagnostics resource log. -1 where the
// platform could not report a value.
struct OwnProcessCounters {
    // Windows: private commit; macOS: physical footprint.
    qint64 privateBytes = -1;
    // Windows: working set; macOS: resident size.
    qint64 workingSetBytes = -1;
    // Windows: kernel handles; macOS: open file descriptors.
    int handles = -1;
    int threads = -1;
};
OwnProcessCounters ownProcessCounters();
