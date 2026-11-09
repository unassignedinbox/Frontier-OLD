#============================================================================================================================================
#                                                   FLUIDSIMULATIONRENDERING.CMAKE
#============================================================================================================================================
# 🧩 The Fluid editor's GPU engine: 1:1 port of Experimental/Fluid's 3D Eulerian Navier-Stokes + combustion solver, volumetric
#    raymarcher, GPU embers/ash, and 2D tiled atlas minimap on DeviceExchange, compiled one SPIR-V file per entry point.

add_library(FrontierFluidSimulation STATIC
    Frontier/Engine/Rendering/FluidSimulation/FluidSimulationSequence.cpp)
target_link_libraries(FrontierFluidSimulation PUBLIC FrontierGlobalIllumination FrontierFluidDynamics)

find_program(FRONTIER_GLOBAL_ILLUMINATION_SLANGC
    NAMES slangc
    PATHS "${CMAKE_SOURCE_DIR}/ExternalPackages/VulkanCompute/Slang/slangtorch/bin"
    NO_DEFAULT_PATH)
set(FRONTIER_FLUID_SIMULATION_SHADER_SRC "${CMAKE_SOURCE_DIR}/Frontier/Engine/Shaders/FluidSimulation")
set(FRONTIER_FLUID_SIMULATION_SHADER_OUT "${CMAKE_BINARY_DIR}/ShaderArchives/FluidSimulation")
file(GLOB FRONTIER_FLUID_SIMULATION_SLANG_SOURCES CONFIGURE_DEPENDS "${FRONTIER_FLUID_SIMULATION_SHADER_SRC}/*.slang"
     "${CMAKE_SOURCE_DIR}/Frontier/Engine/Shaders/GlobalIllumination/IntrinsicParity.slang")

# Unit:Entry:Stage — one SPIR-V file per entry point, <Unit>.<Entry>.spv.
set(FRONTIER_FLUID_SIMULATION_ENTRIES
    "FluidSimulation:InjectSources:compute"
    "FluidSimulation:AdvectFields:compute"
    "FluidSimulation:EvaluateCurl:compute"
    "FluidSimulation:IntegrateForces:compute"
    "FluidSimulation:EvaluateDivergence:compute"
    "FluidSimulation:RelaxPressure:compute"
    "FluidSimulation:ProjectGradient:compute"
    "FluidRaymarch:RaymarchVertex:vertex"
    "FluidRaymarch:RaymarchFragment:fragment"
    "FluidOverlayRasterization:EmberVertex:vertex"
    "FluidOverlayRasterization:EmberFragment:fragment"
    "FluidOverlayRasterization:AtlasVertex:vertex"
    "FluidOverlayRasterization:AtlasFragment:fragment")

set(FRONTIER_FLUID_SIMULATION_SPV_OUTPUTS)
if(FRONTIER_GLOBAL_ILLUMINATION_SLANGC)
    foreach(ENTRY_SPEC IN LISTS FRONTIER_FLUID_SIMULATION_ENTRIES)
        string(REPLACE ":" ";" ENTRY_PARTS "${ENTRY_SPEC}")
        list(GET ENTRY_PARTS 0 ENTRY_UNIT)
        list(GET ENTRY_PARTS 1 ENTRY_NAME)
        list(GET ENTRY_PARTS 2 ENTRY_STAGE)
        set(ENTRY_SPV "${FRONTIER_FLUID_SIMULATION_SHADER_OUT}/${ENTRY_UNIT}.${ENTRY_NAME}.spv")
        list(APPEND FRONTIER_FLUID_SIMULATION_SPV_OUTPUTS "${ENTRY_SPV}")
        add_custom_command(
            OUTPUT "${ENTRY_SPV}"
            COMMAND "${CMAKE_COMMAND}" -E make_directory "${FRONTIER_FLUID_SIMULATION_SHADER_OUT}"
            COMMAND "${FRONTIER_GLOBAL_ILLUMINATION_SLANGC}" "${FRONTIER_FLUID_SIMULATION_SHADER_SRC}/${ENTRY_UNIT}.slang"
                -I "${FRONTIER_FLUID_SIMULATION_SHADER_SRC}"
                -I "${CMAKE_SOURCE_DIR}/Frontier/Engine/Shaders/GlobalIllumination"
                -entry "${ENTRY_NAME}"
                -stage "${ENTRY_STAGE}"
                -target spirv
                -profile spirv_1_3
                -emit-spirv-directly
                -matrix-layout-row-major
                -fvk-use-entrypoint-name
                -O0
                -o "${ENTRY_SPV}"
            DEPENDS ${FRONTIER_FLUID_SIMULATION_SLANG_SOURCES}
            VERBATIM)
    endforeach()
    add_custom_target(FrontierFluidSimulationShaders ALL DEPENDS ${FRONTIER_FLUID_SIMULATION_SPV_OUTPUTS})
endif()
if(TARGET FrontierFluidSimulationShaders)
    add_dependencies(FrontierFluidSimulation FrontierFluidSimulationShaders)
endif()
