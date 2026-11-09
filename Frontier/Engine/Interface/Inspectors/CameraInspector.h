/*==========================================================================================================================================
                                                           CAMERAINSPECTOR.H
==========================================================================================================================================*/
// 🧩 CameraConfiguration inspector: perspective optics, aperture & focus study, subtle camera shake, 3-mode motion blur, and lens flare link.

#pragma once

#include "Engine/Diagnostics/BuildConfiguration.h"
#include "Engine/Interface/InspectorRuntime.h"
#include "Engine/Scene/EngineConfigurations.h"

#include <algorithm>
#include <string>
#include <vector>

namespace Frontier
{

#if FRONTIER_EDITOR

//------------------------------------------------------------------------------------------------------------------------
//                                                    CAMERA INSPECTOR
//------------------------------------------------------------------------------------------------------------------------

class CameraInspector final : public RecordInspector
{
  public:
    explicit CameraInspector(CameraConfiguration& InParameters) noexcept : Parameters(InParameters) {}

    [[nodiscard]] const char* QueryEyebrowText() const noexcept override
    {
        return Parameters.Role == CameraRole::Vehicle ? "CAMERA · VEHICLE RIG" : "CAMERA · EDITOR PROJECTION";
    }
    [[nodiscard]] const char* QuerySectionNote() const noexcept override
    {
        return Parameters.Active ? "Live viewport camera" : "Perspective optics & motion";
    }
    [[nodiscard]] bool QueryToggleSupported() const noexcept override
    {
        return true;
    }
    [[nodiscard]] bool QueryToggleEnabled() const noexcept override
    {
        return Parameters.Enabled;
    }
    void ConfigureToggle(bool Enabled) override
    {
        Parameters.Enabled = Enabled;
        Parameters.AdvanceRevision();
    }
    [[nodiscard]] uint64_t QueryRevision() const noexcept override
    {
        return Parameters.Revision;
    }
    [[nodiscard]] std::string QueryDirectoryReadout() const override;
    void                      EnumerateSwitches(std::vector<InspectorSwitchEntry>& OutSwitches) override;
    void                      ConstructInspector() override;

    [[nodiscard]] bool QueryOccupiesColumn() const noexcept override
    {
        return true;
    }
    [[nodiscard]] int QuerySectionCount() const noexcept override
    {
        return 4;
    }
    void ConfigureSection(int Section) noexcept override
    {
        ActivePane = std::clamp(Section, 0, 3);
    }

  private:
    CameraConfiguration& Parameters;
    int                  ActivePane = 0;
};

#endif // FRONTIER_EDITOR

} // namespace Frontier
