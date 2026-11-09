/*==========================================================================================================================================
                                                         FLUIDSPACECODEC.CPP
==========================================================================================================================================*/
// 🧩 TOML serializer and parser for .fluidspace sidecars and upstream .gasscene.toml parity documents (SceneTomlCodec.js 1:1).

#include "Engine/PhysicalDynamics/FluidDynamics/FluidSpaceCodec.h"

#include <toml++/toml.hpp>

#include <cstdio>
#include <fstream>
#include <sstream>

namespace Frontier::FluidDynamics
{

namespace
{

std::string EscapeTomlString(std::string_view Text)
{
    std::string Out = "\"";
    for (char Ch : Text)
    {
        if (Ch == '\\') Out += "\\\\";
        else if (Ch == '"') Out += "\\\"";
        else if (Ch == '\n') Out += "\\n";
        else if (Ch == '\r') Out += "\\r";
        else if (Ch == '\t') Out += "\\t";
        else Out.push_back(Ch);
    }
    Out.push_back('"');
    return Out;
}

std::string FormatFloatToml(double Value)
{
    char Buffer[64];
    std::snprintf(Buffer, sizeof(Buffer), "%.6g", Value);
    std::string Text = Buffer;
    if (Text.find('.') == std::string::npos && Text.find('e') == std::string::npos && Text.find('E') == std::string::npos)
    {
        Text += ".0";
    }
    return Text;
}

const char* EncodeInteractionMode(FluidInteractionTool Tool) noexcept
{
    switch (Tool)
    {
        case FluidInteractionTool::Brush:    return "brush";
        case FluidInteractionTool::Detonate: return "detonate";
        default:                             return "orbit";
    }
}

FluidInteractionTool DecodeInteractionMode(std::string_view Text) noexcept
{
    if (Text == "brush") return FluidInteractionTool::Brush;
    if (Text == "detonate") return FluidInteractionTool::Detonate;
    return FluidInteractionTool::Orbit;
}

} // namespace

std::string WriteFluidSpaceToml(const FluidSceneDocument& Scene)
{
    const FluidParameters& P = Scene.Parameters;
    std::ostringstream     Out;
    Out << "# Frontier gas scene. Written by Frontier/FluidDynamics; compatible with Experimental/Fluid.\n";
    Out << "# Settings are sorted by name so the file diffs cleanly; order carries no meaning.\n\n";
    Out << "format = \"frontier-fluid-scene\"\n";
    Out << "version = 1\n";
    Out << "name = " << EscapeTomlString(Scene.Name.empty() ? "Untitled scene" : Scene.Name) << "\n\n";

    Out << "[names]\n";
    Out << "collider = " << EscapeTomlString(Scene.Names.Collider) << "\n";
    Out << "domain = " << EscapeTomlString(Scene.Names.Domain) << "\n";
    Out << "emitter = " << EscapeTomlString(Scene.Names.Source) << "\n";
    Out << "sun = " << EscapeTomlString(Scene.Names.Sun) << "\n\n";

    Out << "[camera]\n";
    Out << "theta = " << FormatFloatToml(Scene.Camera.TargetTheta) << "\n";
    Out << "phi = " << FormatFloatToml(Scene.Camera.TargetPhi) << "\n";
    Out << "distance = " << FormatFloatToml(Scene.Camera.TargetDistance) << "\n";
    Out << "centre = [" << FormatFloatToml(Scene.Camera.TargetCentre[0]) << ", "
        << FormatFloatToml(Scene.Camera.TargetCentre[1]) << ", "
        << FormatFloatToml(Scene.Camera.TargetCentre[2]) << "]\n\n";

    Out << "[settings]\n";
    Out << "ambientIntensity = " << FormatFloatToml(P.AmbientIntensity) << "\n";
    Out << "atlasMinimapField = " << P.AtlasMinimapField << "\n";
    Out << "autoGpuGovernor = " << (P.AutoGpuGovernor ? "true" : "false") << "\n";
    Out << "autoTurntable = " << (P.AutoTurntable ? "true" : "false") << "\n";
    Out << "blastFuel = " << FormatFloatToml(P.BlastFuel) << "\n";
    Out << "blastLobes = " << P.BlastLobes << "\n";
    Out << "blastRadius = " << FormatFloatToml(P.BlastRadius) << "\n";
    Out << "blastSmoke = " << FormatFloatToml(P.BlastSmoke) << "\n";
    Out << "blastStrength = " << FormatFloatToml(P.BlastStrength) << "\n";
    Out << "blastTemperature = " << FormatFloatToml(P.BlastTemperature) << "\n";
    Out << "bloomIntensity = " << FormatFloatToml(P.BloomIntensity) << "\n";
    Out << "boundsHeight = " << FormatFloatToml(P.BoundsHeight) << "\n";
    Out << "boundsWidth = " << FormatFloatToml(P.BoundsWidth) << "\n";
    Out << "buoyancy = " << FormatFloatToml(P.Buoyancy) << "\n";
    Out << "burnHeat = " << FormatFloatToml(P.BurnHeat) << "\n";
    Out << "burnRate = " << FormatFloatToml(P.BurnRate) << "\n";
    Out << "colliderAutoMove = " << (P.ColliderAutoMove ? "true" : "false") << "\n";
    Out << "colliderSpeed = " << FormatFloatToml(P.ColliderSpeed) << "\n";
    Out << "colorPalette = " << P.ColorPalette << "\n";
    Out << "combustionExpansion = " << FormatFloatToml(P.CombustionExpansion) << "\n";
    Out << "coolingRate = " << FormatFloatToml(P.CoolingRate) << "\n";
    Out << "densityExtinction = " << FormatFloatToml(P.DensityExtinction) << "\n";
    Out << "dynamicBounds = " << (P.DynamicBounds ? "true" : "false") << "\n";
    Out << "dynamicBoundsMax = " << FormatFloatToml(P.DynamicBoundsMax) << "\n";
    Out << "emberAshiness = " << FormatFloatToml(P.EmberAshiness) << "\n";
    Out << "emberCount = " << P.EmberCount << "\n";
    Out << "emberIntensity = " << FormatFloatToml(P.EmberIntensity) << "\n";
    Out << "emberLifetime = " << FormatFloatToml(P.EmberLifetime) << "\n";
    Out << "emberSize = " << FormatFloatToml(P.EmberSize) << "\n";
    Out << "emitterEnabled = " << (P.SourceEnabled ? "true" : "false") << "\n";
    Out << "emitterFuel = " << FormatFloatToml(P.SourceFuel) << "\n";
    Out << "emitterHeight = " << FormatFloatToml(P.SourceHeight) << "\n";
    Out << "emitterRadius = " << FormatFloatToml(P.SourceRadius) << "\n";
    Out << "emitterRate = " << FormatFloatToml(P.SourceRate) << "\n";
    Out << "emitterSmoke = " << FormatFloatToml(P.SourceSmoke) << "\n";
    Out << "emitterSwirl = " << FormatFloatToml(P.SourceSwirl) << "\n";
    Out << "emitterTemperature = " << FormatFloatToml(P.SourceTemperature) << "\n";
    Out << "emitterUpwardVelocity = " << FormatFloatToml(P.SourceUpwardVelocity) << "\n";
    Out << "enclosedBox = " << (P.EnclosedBox ? "true" : "false") << "\n";
    Out << "exposure = " << FormatFloatToml(P.Exposure) << "\n";
    Out << "fireIntensity = " << FormatFloatToml(P.FireIntensity) << "\n";
    Out << "godRaysIntensity = " << FormatFloatToml(P.GodRaysIntensity) << "\n";
    Out << "gridResolution = " << P.GridResolution << "\n";
    Out << "interactionMode = " << EscapeTomlString(EncodeInteractionMode(P.InteractionTool)) << "\n";
    Out << "internalScattering = " << FormatFloatToml(P.InternalScattering) << "\n";
    Out << "macCormackAdvection = " << (P.MacCormackAdvection ? "true" : "false") << "\n";
    Out << "obstacleRadius = " << FormatFloatToml(P.ObstacleRadius) << "\n";
    Out << "obstacleType = " << P.ObstacleCategory << "\n";
    Out << "obstacleX = " << FormatFloatToml(P.ObstacleX) << "\n";
    Out << "obstacleY = " << FormatFloatToml(P.ObstacleY) << "\n";
    Out << "obstacleZ = " << FormatFloatToml(P.ObstacleZ) << "\n";
    Out << "paused = " << (P.Paused ? "true" : "false") << "\n";
    Out << "phaseAnisotropy = " << FormatFloatToml(P.PhaseAnisotropy) << "\n";
    Out << "pressureIterations = " << P.PressureIterations << "\n";
    Out << "raymarchSteps = " << P.RaymarchSteps << "\n";
    Out << "renderChannel = " << P.RenderChannel << "\n";
    Out << "renderScale = " << FormatFloatToml(P.RenderScale) << "\n";
    Out << "shadowDensity = " << FormatFloatToml(P.ShadowDensity) << "\n";
    Out << "shadowSteps = " << P.ShadowSteps << "\n";
    Out << "shockwaveStrength = " << FormatFloatToml(P.ShockwaveStrength) << "\n";
    Out << "showActiveVoxelCells = " << (P.ShowActiveVoxelCells ? "true" : "false") << "\n";
    Out << "showAtlasMinimap = " << (P.ShowAtlasMinimap ? "true" : "false") << "\n";
    Out << "showBoundingBox = " << (P.ShowBoundingBox ? "true" : "false") << "\n";
    Out << "showEmbers = " << (P.ShowEmbers ? "true" : "false") << "\n";
    Out << "showFloorGrid = " << (P.ShowFloorGrid ? "true" : "false") << "\n";
    Out << "showVoxelGridLines = " << (P.ShowVoxelGridLines ? "true" : "false") << "\n";
    Out << "shrapnelEnabled = " << (P.ShrapnelEnabled ? "true" : "false") << "\n";
    Out << "sliceAxis = " << P.SliceAxis << "\n";
    Out << "slicePosition = " << FormatFloatToml(P.SlicePosition) << "\n";
    Out << "smokeAlbedo = " << FormatFloatToml(P.SmokeAlbedo) << "\n";
    Out << "smokeDissipation = " << FormatFloatToml(P.SmokeDissipation) << "\n";
    Out << "smokeWeight = " << FormatFloatToml(P.SmokeWeight) << "\n";
    Out << "sootGeneration = " << FormatFloatToml(P.SootGeneration) << "\n";
    Out << "sunAzimuth = " << FormatFloatToml(P.SunAzimuth) << "\n";
    Out << "sunElevation = " << FormatFloatToml(P.SunElevation) << "\n";
    Out << "sunIntensity = " << FormatFloatToml(P.SunIntensity) << "\n";
    Out << "temperatureScale = " << FormatFloatToml(P.TemperatureScale) << "\n";
    Out << "timeScale = " << FormatFloatToml(P.TimeScale) << "\n";
    Out << "turbulenceScale = " << FormatFloatToml(P.TurbulenceScale) << "\n";
    Out << "turbulenceStrength = " << FormatFloatToml(P.TurbulenceStrength) << "\n";
    Out << "velocityDamping = " << FormatFloatToml(P.VelocityDamping) << "\n";
    Out << "vorticityConfinement = " << FormatFloatToml(P.VorticityConfinement) << "\n";
    Out << "voxelQuantization = " << FormatFloatToml(P.VoxelQuantization) << "\n";
    Out << "windX = " << FormatFloatToml(P.WindX) << "\n";
    Out << "windZ = " << FormatFloatToml(P.WindZ) << "\n";
    return Out.str();
}

bool ReadFluidSpaceToml(std::string_view Content, FluidSceneDocument& OutScene, std::string* OutDiagnostic)
{
    try
    {
        const toml::table Table = toml::parse(Content);
        const std::string Format = Table["format"].value_or(std::string(""));
        if (Format != "frontier-fluid-scene")
        {
            if (OutDiagnostic) *OutDiagnostic = "Fluid scene format must be 'frontier-fluid-scene'.";
            return false;
        }
        const int64_t Version = Table["version"].value_or<int64_t>(0);
        if (Version != 1)
        {
            if (OutDiagnostic) *OutDiagnostic = "Unsupported fluid scene version.";
            return false;
        }

        FluidSceneDocument Parsed{};
        Parsed.Format     = Format;
        Parsed.Version    = 1;
        Parsed.Name       = Table["name"].value_or(std::string("Untitled scene"));
        Parsed.Parameters = FluidParameters::ConstructInitialParameters();

        if (const toml::table* Names = Table["names"].as_table())
        {
            Parsed.Names.Domain   = (*Names)["domain"].value_or(Parsed.Names.Domain);
            Parsed.Names.Source   = (*Names)["emitter"].value_or(Parsed.Names.Source);
            Parsed.Names.Collider = (*Names)["collider"].value_or(Parsed.Names.Collider);
            Parsed.Names.Sun      = (*Names)["sun"].value_or(Parsed.Names.Sun);
        }

        if (const toml::table* Camera = Table["camera"].as_table())
        {
            Parsed.Camera.TargetTheta    = static_cast<float>((*Camera)["theta"].value_or<double>(Parsed.Camera.TargetTheta));
            Parsed.Camera.TargetPhi      = static_cast<float>((*Camera)["phi"].value_or<double>(Parsed.Camera.TargetPhi));
            Parsed.Camera.TargetDistance = static_cast<float>((*Camera)["distance"].value_or<double>(Parsed.Camera.TargetDistance));
            if (const toml::array* Centre = (*Camera)["centre"].as_array(); Centre && Centre->size() == 3)
            {
                for (size_t Axis = 0; Axis < 3; ++Axis)
                {
                    Parsed.Camera.TargetCentre[Axis] = static_cast<float>((*Centre)[Axis].value_or<double>(0.0));
                }
            }
            Parsed.Camera.SnapToTarget();
        }

        if (const toml::table* Settings = Table["settings"].as_table())
        {
            FluidParameters& P = Parsed.Parameters;
            auto ReadFloat = [&](std::string_view Key, float& Target) {
                if (auto Value = (*Settings)[Key].value<double>()) Target = static_cast<float>(*Value);
            };
            auto ReadInt = [&](std::string_view Key, int& Target) {
                if (auto Value = (*Settings)[Key].value<int64_t>()) Target = static_cast<int>(*Value);
                else if (auto FloatVal = (*Settings)[Key].value<double>()) Target = static_cast<int>(std::lround(*FloatVal));
            };
            auto ReadBool = [&](std::string_view Key, bool& Target) {
                if (auto Value = (*Settings)[Key].value<bool>()) Target = *Value;
            };

            ReadInt("gridResolution", P.GridResolution);
            ReadInt("pressureIterations", P.PressureIterations);
            ReadFloat("timeScale", P.TimeScale);
            ReadBool("paused", P.Paused);
            ReadBool("macCormackAdvection", P.MacCormackAdvection);
            ReadBool("enclosedBox", P.EnclosedBox);
            ReadFloat("voxelQuantization", P.VoxelQuantization);
            ReadBool("autoGpuGovernor", P.AutoGpuGovernor);
            ReadFloat("boundsWidth", P.BoundsWidth);
            ReadFloat("boundsHeight", P.BoundsHeight);
            ReadBool("dynamicBounds", P.DynamicBounds);
            ReadFloat("dynamicBoundsMax", P.DynamicBoundsMax);
            if (auto Mode = (*Settings)["interactionMode"].value<std::string>())
            {
                P.InteractionTool = DecodeInteractionMode(*Mode);
            }
            ReadBool("autoTurntable", P.AutoTurntable);
            ReadFloat("vorticityConfinement", P.VorticityConfinement);
            ReadFloat("buoyancy", P.Buoyancy);
            ReadFloat("smokeWeight", P.SmokeWeight);
            ReadFloat("burnRate", P.BurnRate);
            ReadFloat("burnHeat", P.BurnHeat);
            ReadFloat("sootGeneration", P.SootGeneration);
            ReadFloat("combustionExpansion", P.CombustionExpansion);
            ReadFloat("coolingRate", P.CoolingRate);
            ReadFloat("smokeDissipation", P.SmokeDissipation);
            ReadFloat("velocityDamping", P.VelocityDamping);
            ReadFloat("turbulenceStrength", P.TurbulenceStrength);
            ReadFloat("turbulenceScale", P.TurbulenceScale);
            ReadFloat("windX", P.WindX);
            ReadFloat("windZ", P.WindZ);
            ReadBool("emitterEnabled", P.SourceEnabled);
            ReadFloat("emitterRate", P.SourceRate);
            ReadFloat("emitterRadius", P.SourceRadius);
            ReadFloat("emitterHeight", P.SourceHeight);
            ReadFloat("emitterUpwardVelocity", P.SourceUpwardVelocity);
            ReadFloat("emitterSwirl", P.SourceSwirl);
            ReadFloat("emitterTemperature", P.SourceTemperature);
            ReadFloat("emitterFuel", P.SourceFuel);
            ReadFloat("emitterSmoke", P.SourceSmoke);
            ReadFloat("blastStrength", P.BlastStrength);
            ReadFloat("blastRadius", P.BlastRadius);
            ReadFloat("blastTemperature", P.BlastTemperature);
            ReadFloat("blastFuel", P.BlastFuel);
            ReadFloat("blastSmoke", P.BlastSmoke);
            ReadInt("blastLobes", P.BlastLobes);
            ReadBool("shrapnelEnabled", P.ShrapnelEnabled);
            ReadInt("renderChannel", P.RenderChannel);
            ReadInt("colorPalette", P.ColorPalette);
            ReadFloat("renderScale", P.RenderScale);
            ReadInt("raymarchSteps", P.RaymarchSteps);
            ReadInt("shadowSteps", P.ShadowSteps);
            ReadFloat("densityExtinction", P.DensityExtinction);
            ReadFloat("smokeAlbedo", P.SmokeAlbedo);
            ReadFloat("shadowDensity", P.ShadowDensity);
            ReadFloat("fireIntensity", P.FireIntensity);
            ReadFloat("temperatureScale", P.TemperatureScale);
            ReadFloat("internalScattering", P.InternalScattering);
            ReadFloat("phaseAnisotropy", P.PhaseAnisotropy);
            ReadFloat("ambientIntensity", P.AmbientIntensity);
            ReadFloat("sunIntensity", P.SunIntensity);
            ReadFloat("sunAzimuth", P.SunAzimuth);
            ReadFloat("sunElevation", P.SunElevation);
            ReadFloat("exposure", P.Exposure);
            ReadFloat("bloomIntensity", P.BloomIntensity);
            ReadFloat("godRaysIntensity", P.GodRaysIntensity);
            ReadFloat("shockwaveStrength", P.ShockwaveStrength);
            ReadInt("sliceAxis", P.SliceAxis);
            ReadFloat("slicePosition", P.SlicePosition);
            ReadInt("obstacleType", P.ObstacleCategory);
            ReadFloat("obstacleX", P.ObstacleX);
            ReadFloat("obstacleY", P.ObstacleY);
            ReadFloat("obstacleZ", P.ObstacleZ);
            ReadFloat("obstacleRadius", P.ObstacleRadius);
            ReadBool("colliderAutoMove", P.ColliderAutoMove);
            ReadFloat("colliderSpeed", P.ColliderSpeed);
            ReadBool("showBoundingBox", P.ShowBoundingBox);
            ReadBool("showVoxelGridLines", P.ShowVoxelGridLines);
            ReadBool("showActiveVoxelCells", P.ShowActiveVoxelCells);
            ReadBool("showFloorGrid", P.ShowFloorGrid);
            ReadBool("showEmbers", P.ShowEmbers);
            ReadInt("emberCount", P.EmberCount);
            ReadFloat("emberSize", P.EmberSize);
            ReadFloat("emberIntensity", P.EmberIntensity);
            ReadFloat("emberLifetime", P.EmberLifetime);
            ReadFloat("emberAshiness", P.EmberAshiness);
            ReadBool("showAtlasMinimap", P.ShowAtlasMinimap);
            ReadInt("atlasMinimapField", P.AtlasMinimapField);
        }

        Parsed.Parameters.ClampToValidRanges();
        Parsed.Camera.AutoTurntable = Parsed.Parameters.AutoTurntable;
        OutScene                    = std::move(Parsed);
        return true;
    }
    catch (const std::exception& Failure)
    {
        if (OutDiagnostic) *OutDiagnostic = Failure.what();
        return false;
    }
}

bool SaveFluidSpaceFile(const std::filesystem::path& FilePath, const FluidSceneDocument& Scene, std::string* OutDiagnostic)
{
    try
    {
        if (FilePath.has_parent_path())
        {
            std::filesystem::create_directories(FilePath.parent_path());
        }
        std::ofstream Stream(FilePath, std::ios::binary | std::ios::trunc);
        if (!Stream)
        {
            if (OutDiagnostic) *OutDiagnostic = "Unable to open fluid scene file for writing: " + FilePath.string();
            return false;
        }
        const std::string Serialized = WriteFluidSpaceToml(Scene);
        Stream.write(Serialized.data(), static_cast<std::streamsize>(Serialized.size()));
        return Stream.good();
    }
    catch (const std::exception& Failure)
    {
        if (OutDiagnostic) *OutDiagnostic = Failure.what();
        return false;
    }
}

bool LoadFluidSpaceFile(const std::filesystem::path& FilePath, FluidSceneDocument& OutScene, std::string* OutDiagnostic)
{
    try
    {
        std::ifstream Stream(FilePath, std::ios::binary);
        if (!Stream)
        {
            if (OutDiagnostic) *OutDiagnostic = "Unable to open fluid scene file: " + FilePath.string();
            return false;
        }
        std::ostringstream Buffer;
        Buffer << Stream.rdbuf();
        return ReadFluidSpaceToml(Buffer.str(), OutScene, OutDiagnostic);
    }
    catch (const std::exception& Failure)
    {
        if (OutDiagnostic) *OutDiagnostic = Failure.what();
        return false;
    }
}

} // namespace Frontier::FluidDynamics
