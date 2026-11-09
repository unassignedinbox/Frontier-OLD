/*==========================================================================================================================================
                                                           FLUIDPLUMEINSPECTOR.H
==========================================================================================================================================*/
// 🧩 FluidPlumeConfiguration: the Eulerian gas & explosion plume page over the shared Inspector controls, with its 1:1 FluidPlumeEditor.

#pragma once

#include "Engine/Diagnostics/BuildConfiguration.h"
#include "Engine/Interface/InspectorRuntime.h"
#include "Engine/Scene/EngineConfigurations.h"

#include <string>
#include <vector>

namespace Frontier
{

#if FRONTIER_EDITOR

//------------------------------------------------------------------------------------------------------------------------
//                                                    FLUID PLUME INSPECTOR
//------------------------------------------------------------------------------------------------------------------------

class FluidPlumeInspector final : public RecordInspector
{
  public:
    explicit FluidPlumeInspector(FluidPlumeConfiguration& InParameters) noexcept : Parameters(InParameters) {}

    [[nodiscard]] const char* QueryEyebrowText() const noexcept override { return "SOLVER \xC2\xB7 FLUID PLUME"; }
    [[nodiscard]] const char* QuerySectionNote() const noexcept override { return "3D Eulerian Gas & Combustion"; }
    [[nodiscard]] const char* QueryEditorButtonLabel() const noexcept override { return "Fluid Editor"; }
    void                      ConstructEditor(RecordToken Token) override;
    [[nodiscard]] bool        QueryToggleSupported() const noexcept override { return true; }
    [[nodiscard]] bool        QueryToggleEnabled() const noexcept override { return Parameters.Enabled; }
    void                      ConfigureToggle(bool Enabled) override { Parameters.Enabled = Enabled; Parameters.AdvanceRevision(); }
    [[nodiscard]] uint64_t    QueryRevision() const noexcept override { return Parameters.Revision; }
    [[nodiscard]] bool        QueryOccupiesColumn() const noexcept override { return true; }
    [[nodiscard]] int         QuerySectionCount() const noexcept override { return 3; }
    void                      ConfigureSection(int Section) noexcept override { ActivePane = Section < 0 ? 0 : (Section > 2 ? 2 : Section); }
    void                      EnumerateSwitches(std::vector<InspectorSwitchEntry>& OutSwitches) override;
    void                      ConstructInspector() override;
    [[nodiscard]] std::string QueryDirectoryReadout() const override;
    [[nodiscard]] RecordOperationalCondition QueryDirectoryCondition() const noexcept override
    {
        return Parameters.Enabled ? (Parameters.CombustionEnabled ? RecordOperationalCondition::Live : RecordOperationalCondition::Warm)
                                  : RecordOperationalCondition::Standby;
    }

    [[nodiscard]] FluidPlumeConfiguration&       AccessParameters() noexcept { return Parameters; }
    [[nodiscard]] const FluidPlumeConfiguration& QueryParameters() const noexcept { return Parameters; }

  private:
    FluidPlumeConfiguration& Parameters;
    int                      ActivePane = 0;
};

#endif // FRONTIER_EDITOR

} // namespace Frontier
