#============================================================================================================================================
#                                                           FRACTUREWORKSPACE.CMAKE
#============================================================================================================================================
# Fracture workspace validation: headless interaction checks and PPM evidence for the U4 UI shell.

if(BUILD_TESTING)
    add_executable(FractureWorkspaceValidation
        Exhibits/FractureWorkspace/Code/FractureWorkspaceValidation.cpp)
    target_include_directories(FractureWorkspaceValidation PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(FractureWorkspaceValidation PRIVATE FrontierSceneEditorUI)
    add_test(NAME FractureWorkspaceValidation COMMAND FractureWorkspaceValidation "${CMAKE_BINARY_DIR}/FractureWorkspaceValidation.ppm" "${CMAKE_SOURCE_DIR}/EngineContent/FontArchives")
    set_tests_properties(FractureWorkspaceValidation PROPERTIES TIMEOUT 180)
endif()
