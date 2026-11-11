# Writes spantui-build.json next to a built executable so the update check
# (InstallerSource) can compare version + build time without parsing PE headers.
if(NOT DEFINED OUT OR NOT DEFINED VERSION)
    message(FATAL_ERROR "write_build_stamp.cmake requires -DOUT=... and -DVERSION=...")
endif()

string(TIMESTAMP BUILT_AT_UTC "%Y-%m-%dT%H:%M:%SZ" UTC)
set(EXE_NAME "")
if(DEFINED EXE)
    set(EXE_NAME "${EXE}")
endif()

file(WRITE "${OUT}"
"{
  \"version\": \"${VERSION}\",
  \"builtAtUtc\": \"${BUILT_AT_UTC}\",
  \"exe\": \"${EXE_NAME}\"
}
")
