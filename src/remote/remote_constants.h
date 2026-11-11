#pragma once

namespace span::remote {

// Exit code the mutation scripts use when the target name already exists.
inline constexpr int kRemoteExistsExitCode = 17;
// remoteListScript failures: the directory cannot be entered, the host has
// no GNU find, or the directory cannot be read. Only exit 0 is a listing.
inline constexpr int kListEnterFailedExitCode = 2;
inline constexpr int kListMissingFindExitCode = 3;
inline constexpr int kListReadFailedExitCode = 4;

// For every ssh/sftp process. SSH_ASKPASS is this executable; seeing these
// variables, it forwards the prompt over the pipe instead of starting the GUI.
inline constexpr char kAskpassPipeEnv[] = "SPANTUI_ASKPASS_PIPE";
inline constexpr char kAskpassTokenEnv[] = "SPANTUI_ASKPASS_TOKEN";

} // namespace span::remote
