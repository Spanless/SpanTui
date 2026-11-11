# Remove QML copies that no longer exist in the source tree. qt_add_qml_module
# copies listed files into the module output dir but never deletes leftovers,
# so a removed QML file would keep shipping and shadow the source tree.

if(NOT DEFINED QML_MODULE_DIR OR NOT DEFINED SOURCE_QML_DIR)
    message(FATAL_ERROR "QML_MODULE_DIR and SOURCE_QML_DIR are required")
endif()

if(NOT IS_DIRECTORY "${QML_MODULE_DIR}" OR NOT IS_DIRECTORY "${SOURCE_QML_DIR}")
    return()
endif()

file(GLOB_RECURSE _copied "${QML_MODULE_DIR}/*.qml")
foreach(_file IN LISTS _copied)
    file(RELATIVE_PATH _rel "${QML_MODULE_DIR}" "${_file}")
    if(NOT EXISTS "${SOURCE_QML_DIR}/${_rel}")
        file(REMOVE "${_file}")
        message(STATUS "Removed stale QML module file: ${_rel}")
    endif()
endforeach()
