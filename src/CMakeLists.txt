# Domain static libraries, declared bottom-up. Link edges below are the
# enforced layering: a module can only include what it links, and nothing
# links SpanApplication except the executable and its tests.
#   core -> platform -> terminal -> monitoring
#   core -> git | documents
#   core + platform -> workspace | update
#   core -> remote
#   application -> all of the above
add_subdirectory(core)
add_subdirectory(platform)
add_subdirectory(terminal)
add_subdirectory(monitoring)
add_subdirectory(git)
add_subdirectory(documents)
add_subdirectory(workspace)
add_subdirectory(update)
add_subdirectory(remote)
add_subdirectory(application)
