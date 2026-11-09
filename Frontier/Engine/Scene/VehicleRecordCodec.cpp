/*==========================================================================================================================================
                                                          VEHICLERECORDCODEC.CPP
==========================================================================================================================================*/
// 🧩 Vehicle and Tyre records to and from TOML through the specification's figure registry.

#include "Engine/Scene/VehicleRecordCodec.h"

#include <toml++/toml.hpp>

#include <cmath>
#include <sstream>
#include <string>

namespace Frontier
{

namespace
{

// 📝 "suspension.front" → the nested table, created on the way down.
toml::table& ResolveSection(toml::table& Root, const std::string& Dotted)
{
    toml::table*      Current = &Root;
    std::stringstream Parts(Dotted);
    std::string       Part;
    while (std::getline(Parts, Part, '.'))
    {
        if (!Current->contains(Part) || !(*Current)[Part].is_table()) Current->insert_or_assign(Part, toml::table{});
        Current = (*Current)[Part].as_table();
    }
    return *Current;
}

const toml::table* FindSection(const toml::table& Root, const std::string& Dotted)
{
    const toml::table* Current = &Root;
    std::stringstream  Parts(Dotted);
    std::string        Part;
    while (std::getline(Parts, Part, '.'))
    {
        const toml::node* Next = Current->get(Part);
        if (Next == nullptr || !Next->is_table()) return nullptr;
        Current = Next->as_table();
    }
    return Current;
}

double Rounded(double Scalar)
{
    if (Scalar == 0.0 || !std::isfinite(Scalar)) return Scalar;
    const double Scale = std::pow(10.0, 6 - static_cast<int>(std::ceil(std::log10(std::fabs(Scalar)))));
    return std::round(Scalar * Scale) / Scale;
}

void WriteTriple(toml::table& Table, const char* Key, const float Coordinates[3])
{
    toml::array Array;
    for (int Axis = 0; Axis < 3; ++Axis) Array.push_back(Rounded(Coordinates[Axis]));
    Table.insert_or_assign(Key, std::move(Array));
}

void ReadTriple(const toml::table& Table, const char* Key, float Coordinates[3])
{
    const toml::array* Array = Table[Key].as_array();
    if (Array == nullptr || Array->size() != 3) return;
    for (size_t Axis = 0; Axis < 3; ++Axis)
        if (auto Coordinate = (*Array)[Axis].value<double>()) Coordinates[Axis] = static_cast<float>(*Coordinate);
}

} // namespace

void EncodeVehicleConfiguration(const VehicleConfiguration& Configuration, toml::table& OutTable)
{
    OutTable.insert_or_assign("visible", Configuration.Visible);
    OutTable.insert_or_assign("physics", Configuration.Physics);
    OutTable.insert_or_assign("possessed", Configuration.Possessed);
    OutTable.insert_or_assign("chase_camera", Configuration.ChaseCamera);
    OutTable.insert_or_assign("threaded_physics", Configuration.ThreadedPhysics);
    OutTable.insert_or_assign("equilibrium_placement", Configuration.EquilibriumPlacement);
    OutTable.insert_or_assign("engage_brakes_on_placement", Configuration.EngageBrakesOnPlacement);
    OutTable.insert_or_assign("physics_step_hz", Rounded(Configuration.PhysicsStepHz));
    OutTable.insert_or_assign("collision_steps", static_cast<int64_t>(Configuration.CollisionSteps));
    OutTable.insert_or_assign("solver_substeps", static_cast<int64_t>(Configuration.SolverSubsteps));
    OutTable.insert_or_assign("group", Configuration.GroupName);
    OutTable.insert_or_assign("vehicle_name", Configuration.Specification.Name);
    WriteTriple(OutTable, "position_m", Configuration.Spatial.PositionMetres);
    WriteTriple(OutTable, "orientation_deg", Configuration.Spatial.OrientationDegrees);
    // 📝 The registry reads through accessors bound to a specification it may also write, so it walks a copy.
    Vehicle::VehicleSpecification Specification = Configuration.Specification;
    for (const Vehicle::VehicleFigure& Figure : Vehicle::EnumerateVehicleFigures(Specification))
    {
        toml::table& Section = ResolveSection(OutTable, Figure.Section);
        const double Stored  = Figure.Read();
        switch (Figure.Category)
        {
            case Vehicle::VehicleFigureCategory::Switch: Section.insert_or_assign(Figure.Key, Stored > 0.5); break;
            case Vehicle::VehicleFigureCategory::Whole:
            case Vehicle::VehicleFigureCategory::Choice: Section.insert_or_assign(Figure.Key, static_cast<int64_t>(std::llround(Stored))); break;
            case Vehicle::VehicleFigureCategory::Real:   Section.insert_or_assign(Figure.Key, Rounded(Stored)); break;
        }
    }
    for (const Vehicle::VehicleCurve& Entry : Vehicle::EnumerateVehicleCurves(Specification))
    {
        toml::array Points;
        for (size_t Index = 0; Index < Entry.Target->X.size() && Index < Entry.Target->Y.size(); ++Index)
            Points.push_back(toml::array{ Rounded(Entry.Target->X[Index]), Rounded(Entry.Target->Y[Index]) });
        ResolveSection(OutTable, Entry.Section).insert_or_assign(Entry.Key, std::move(Points));
    }
    toml::array Ratios;
    for (const float Ratio : Vehicle::QueryForwardRatios(Specification)) Ratios.push_back(Rounded(Ratio));
    ResolveSection(OutTable, "gearbox").insert_or_assign("ratios", std::move(Ratios));
}

void DecodeVehicleConfiguration(const toml::table& InTable, VehicleConfiguration& Configuration)
{
    Configuration.Visible         = InTable["visible"].value_or(Configuration.Visible);
    Configuration.Physics         = InTable["physics"].value_or(Configuration.Physics);
    Configuration.Possessed       = InTable["possessed"].value_or(Configuration.Possessed);
    Configuration.ChaseCamera             = InTable["chase_camera"].value_or(Configuration.ChaseCamera);
    Configuration.ThreadedPhysics         = InTable["threaded_physics"].value_or(Configuration.ThreadedPhysics);
    Configuration.EquilibriumPlacement    = InTable["equilibrium_placement"].value_or(Configuration.EquilibriumPlacement);
    Configuration.EngageBrakesOnPlacement = InTable["engage_brakes_on_placement"].value_or(Configuration.EngageBrakesOnPlacement);
    Configuration.PhysicsStepHz           = static_cast<float>(InTable["physics_step_hz"].value_or(static_cast<double>(Configuration.PhysicsStepHz)));
    Configuration.CollisionSteps  = static_cast<int>(InTable["collision_steps"].value_or(static_cast<int64_t>(Configuration.CollisionSteps)));
    Configuration.SolverSubsteps  = static_cast<int>(InTable["solver_substeps"].value_or(static_cast<int64_t>(Configuration.SolverSubsteps)));
    Configuration.GroupName       = InTable["group"].value_or(Configuration.GroupName);
    Configuration.Specification.Name = InTable["vehicle_name"].value_or(Configuration.Specification.Name);
    ReadTriple(InTable, "position_m", Configuration.Spatial.PositionMetres);
    ReadTriple(InTable, "orientation_deg", Configuration.Spatial.OrientationDegrees);
    for (const Vehicle::VehicleFigure& Figure : Vehicle::EnumerateVehicleFigures(Configuration.Specification))
    {
        const toml::table* Section = FindSection(InTable, Figure.Section);
        if (Section == nullptr) continue;
        const toml::node* StoredEntry = Section->get(Figure.Key);
        if (StoredEntry == nullptr && std::string(Figure.Section) == "carcass" && std::string(Figure.Key) == "formulation")
            StoredEntry = Section->get("model");
        if (StoredEntry == nullptr) continue;
        if (auto SwitchReading = StoredEntry->value<bool>(); SwitchReading && Figure.Category == Vehicle::VehicleFigureCategory::Switch)
            Figure.Write(*SwitchReading ? 1.0 : 0.0);
        else if (auto Number = StoredEntry->value<double>()) Figure.Write(*Number);
    }
    for (const Vehicle::VehicleCurve& Entry : Vehicle::EnumerateVehicleCurves(Configuration.Specification))
    {
        const toml::table* Section = FindSection(InTable, Entry.Section);
        const toml::array* Samples = Section != nullptr ? (*Section)[Entry.Key].as_array() : nullptr;
        if (Samples == nullptr || Samples->size() < 2) continue;
        Vehicle::Curve Loaded;
        bool Valid = true;
        for (const toml::node& Sample : *Samples)
        {
            const toml::array* Pair = Sample.as_array();
            if (Pair == nullptr || Pair->size() != 2)
            {
                Valid = false;
                break;
            }
            const auto X = (*Pair)[0].value<double>(), Y = (*Pair)[1].value<double>();
            if (!X || !Y || !std::isfinite(*X) || !std::isfinite(*Y))
            {
                Valid = false;
                break;
            }
            const float RuntimeX = static_cast<float>(*X), RuntimeY = static_cast<float>(*Y);
            if (!std::isfinite(RuntimeX) || !std::isfinite(RuntimeY) || (!Loaded.X.empty() && !(RuntimeX > Loaded.X.back())))
            {
                Valid = false;
                break;
            }
            Loaded.Add(RuntimeX, RuntimeY);
        }
        if (Valid && Loaded.X.size() >= 2) *Entry.Target = std::move(Loaded);
    }
    if (const toml::table* Gearbox = FindSection(InTable, "gearbox"))
        if (const toml::array* Ratios = (*Gearbox)["ratios"].as_array())
        {
            std::vector<float> Forward;
            for (const toml::node& Ratio : *Ratios)
                if (auto Scalar = Ratio.value<double>()) Forward.push_back(static_cast<float>(*Scalar));
            if (!Forward.empty()) Vehicle::AssignForwardRatios(Configuration.Specification, Forward);
        }
    Configuration.AdvanceRevision();
}

void EncodeTyreConfiguration(const TyreConfiguration& Configuration, toml::table& OutTable)
{
    OutTable.insert_or_assign("corner", static_cast<int64_t>(Configuration.Corner));
    OutTable.insert_or_assign("enclosed", Configuration.Enclosed);
    OutTable.insert_or_assign("compound", Configuration.Compound);
    OutTable.insert_or_assign("cold_pressure_pa", Rounded(Configuration.ColdPressure));
    OutTable.insert_or_assign("set", Configuration.SetName);
}

void DecodeTyreConfiguration(const toml::table& InTable, TyreConfiguration& Configuration)
{
    Configuration.Corner       = static_cast<uint32_t>(InTable["corner"].value_or(static_cast<int64_t>(Configuration.Corner)) & 3);
    Configuration.Enclosed     = InTable["enclosed"].value_or(Configuration.Enclosed);
    Configuration.Compound     = InTable["compound"].value_or(Configuration.Compound);
    Configuration.ColdPressure = static_cast<float>(InTable["cold_pressure_pa"].value_or(static_cast<double>(Configuration.ColdPressure)));
    Configuration.SetName      = InTable["set"].value_or(Configuration.SetName);
    Configuration.AdvanceRevision();
}


bool EncodeVehicleCurvePack(const VehicleConfiguration& Configuration, std::string& OutText, std::string& OutError)
{
    OutText.clear();
    OutError.clear();
    Vehicle::VehicleSpecification Specification = Configuration.Specification;
    toml::table CurvesTable;
    for (const Vehicle::VehicleCurve& Entry : Vehicle::EnumerateVehicleCurves(Specification))
    {
        if (Entry.Target == nullptr || Entry.Target->X.size() < 2u || Entry.Target->X.size() > 4096u ||
            Entry.Target->X.size() != Entry.Target->Y.size())
        {
            OutError = "Curve " + Entry.Section + "." + Entry.Key + " must contain 2 to 4096 matching knots.";
            return false;
        }
        toml::array Points;
        for (size_t Index = 0; Index < Entry.Target->X.size(); ++Index)
        {
            const float X = Entry.Target->X[Index];
            const float Y = Entry.Target->Y[Index];
            if (!std::isfinite(X) || !std::isfinite(Y) || (Index > 0u && !(X > Entry.Target->X[Index - 1u])))
            {
                OutError = "Curve " + Entry.Section + "." + Entry.Key + " contains a non-finite or unordered knot.";
                return false;
            }
            if (X < Entry.MinimumX || X > Entry.MaximumX || Y < Entry.MinimumY || Y > Entry.MaximumY)
            {
                OutError = "Curve " + Entry.Section + "." + Entry.Key + " contains a knot outside its authored limits.";
                return false;
            }
            Points.push_back(toml::array{static_cast<double>(X), static_cast<double>(Y)});
        }
        ResolveSection(CurvesTable, Entry.Section).insert_or_assign(Entry.Key, std::move(Points));
    }

    toml::table Root;
    Root.insert_or_assign("format", "frontier.vehicle-curves");
    Root.insert_or_assign("version", static_cast<int64_t>(1));
    Root.insert_or_assign("vehicle", Configuration.Specification.Name);
    Root.insert_or_assign("curves", std::move(CurvesTable));
    std::ostringstream Output;
    Output << Root << '\n';
    OutText = std::move(Output).str();
    return true;
}

bool DecodeVehicleCurvePack(std::string_view Text, VehicleConfiguration& Configuration, std::string& OutError)
{
    OutError.clear();
    toml::table Root;
    try
    {
        Root = toml::parse(std::string(Text));
    }
    catch (const toml::parse_error& Error)
    {
        OutError = std::string("Invalid .vcurves TOML: ") + std::string(Error.description());
        return false;
    }

    const auto Format = Root["format"].value<std::string>();
    if (!Format || *Format != "frontier.vehicle-curves")
    {
        OutError = "The file is not a Frontier vehicle-curves pack.";
        return false;
    }
    const auto Version = Root["version"].value<int64_t>();
    if (!Version)
    {
        OutError = "The vehicle-curves pack is missing its integer version.";
        return false;
    }
    if (*Version != 1)
    {
        OutError = "Unsupported .vcurves version " + std::to_string(*Version) + ".";
        return false;
    }
    const toml::table* CurvesTable = Root["curves"].as_table();
    if (CurvesTable == nullptr)
    {
        OutError = "The .vcurves pack has no [curves] table.";
        return false;
    }

    struct PendingCurve
    {
        Vehicle::Curve* Target = nullptr;
        Vehicle::Curve  Value{};
        std::string     Name{};
    };
    std::vector<PendingCurve> Pending;
    for (const Vehicle::VehicleCurve& Entry : Vehicle::EnumerateVehicleCurves(Configuration.Specification))
    {
        if (Entry.Target == nullptr)
        {
            OutError = "Curve registry entry " + Entry.Section + "." + Entry.Key + " has no target.";
            return false;
        }
        const toml::table* Section = FindSection(*CurvesTable, Entry.Section);
        const toml::node*  Node = Section != nullptr ? Section->get(Entry.Key) : nullptr;
        if (Node == nullptr) continue; // 📝 Known but omitted curves keep their current values; future curve names are ignored.
        const toml::array* Points = Node->as_array();
        if (Points == nullptr || Points->size() < 2u || Points->size() > 4096u)
        {
            OutError = "Curve " + Entry.Section + "." + Entry.Key + " must be an array of 2 to 4096 knots.";
            return false;
        }

        PendingCurve Candidate;
        Candidate.Target = Entry.Target;
        Candidate.Name = Entry.Section + "." + Entry.Key;
        Candidate.Value.X.reserve(Points->size());
        Candidate.Value.Y.reserve(Points->size());
        for (size_t Index = 0; Index < Points->size(); ++Index)
        {
            const toml::array* Pair = (*Points)[Index].as_array();
            if (Pair == nullptr || Pair->size() != 2u)
            {
                OutError = "Curve " + Candidate.Name + " knot " + std::to_string(Index + 1u) + " must contain exactly [x, y].";
                return false;
            }
            const auto X = (*Pair)[0].value<double>();
            const auto Y = (*Pair)[1].value<double>();
            if (!X || !Y || !std::isfinite(*X) || !std::isfinite(*Y))
            {
                OutError = "Curve " + Candidate.Name + " knot " + std::to_string(Index + 1u) + " must contain finite numbers.";
                return false;
            }
            if (*X < Entry.MinimumX || *X > Entry.MaximumX || *Y < Entry.MinimumY || *Y > Entry.MaximumY)
            {
                OutError = "Curve " + Candidate.Name + " knot " + std::to_string(Index + 1u) + " is outside its authored limits.";
                return false;
            }
            const float RuntimeX = static_cast<float>(*X);
            const float RuntimeY = static_cast<float>(*Y);
            if (!std::isfinite(RuntimeX) || !std::isfinite(RuntimeY) ||
                (Index > 0u && !(RuntimeX > Candidate.Value.X.back())))
            {
                OutError = "Curve " + Candidate.Name + " X knots must remain strictly increasing after conversion to runtime floats.";
                return false;
            }
            Candidate.Value.Add(RuntimeX, RuntimeY);
        }
        Pending.push_back(std::move(Candidate));
    }

    if (Pending.empty())
    {
        OutError = "The .vcurves pack contains no curves recognized by this vehicle.";
        return false;
    }

    // 📝 Validation is transactional: no authored curve or revision changes until every recognized entry has passed.
    for (PendingCurve& Candidate : Pending) *Candidate.Target = std::move(Candidate.Value);
    Configuration.AdvanceRevision();
    return true;
}

} // namespace Frontier
