# SSH remote hosts: argument building for the system OpenSSH client, the
# askpass bridge, the persistent remote shell channel behind the remote file
# browser and its sftp transfer queue. No SSH protocol code lives here — every
# connection is a Windows OpenSSH process.
add_library(SpanRemote STATIC
    remote_constants.h
    remote_file_listing.cpp
    remote_file_listing.h
    remote_file_model.cpp
    remote_file_model.h
    remote_host_context.cpp
    remote_host_context.h
    remote_host_registry.cpp
    remote_host_registry.h
    remote_shell_channel.cpp
    remote_shell_channel.h
    remote_system_monitor.cpp
    remote_system_monitor.h
    remote_transfer_queue.cpp
    remote_transfer_queue.h
    ssh_askpass.cpp
    ssh_askpass.h
    ssh_command.cpp
    ssh_command.h
    tailscale_devices.cpp
    tailscale_devices.h
)
target_link_libraries(SpanRemote
    PUBLIC SpanCore Qt6::Gui Qt6::Network
    PRIVATE SpanPlatform Qt6::Sql shell32
)
# QML-facing types (src/application/runtime/qml_types.h).
qt_extract_metatypes(SpanRemote)
