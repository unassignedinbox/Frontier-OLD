#============================================================================================================================================
#                                                           PHOTOMETRICWORKSPACE.CMAKE
#============================================================================================================================================
# Photometric workspace validation: headless interaction checks and PPM evidence for the U5 UI shell.

if(BUILD_TESTING)
    add_executable(PhotometricWorkspaceValidation
        Exhibits/PhotometricWorkspace/Code/PhotometricWorkspaceValidation.cpp)
    target_include_directories(PhotometricWorkspaceValidation PRIVATE "${CMAKE_SOURCE_DIR}")
    target_link_libraries(PhotometricWorkspaceValidation PRIVATE FrontierSceneEditorUI)
    add_test(NAME PhotometricWorkspaceValidation COMMAND PhotometricWorkspaceValidation
        "${CMAKE_BINARY_DIR}/PhotometricWorkspaceValidation.ppm" "${CMAKE_SOURCE_DIR}/EngineContent/FontArchives"
        "${CMAKE_SOURCE_DIR}/Exhibits/PhotometricWorkspace/Fixtures/TypeC_Imported.ies")
    set_tests_properties(PhotometricWorkspaceValidation PROPERTIES TIMEOUT 180)
endif()
