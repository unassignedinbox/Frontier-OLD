/*==========================================================================================================================================
                                                         VEHICLECAMERASOLVER.H
==========================================================================================================================================*/
// 🧩 VehicleHMI camera solver: studio Lobby entrance dolly, lag, containment, 6-shot Lobby cinematic tour, rig/route transitions & 8 rigs.

#pragma once

#include "CameraEffects.h"

#include <array>
#include <cstdint>
#include <string_view>

namespace Frontier
{

enum class VehicleCameraRig : uint8_t
{
    Chase         = 0,   // [-] - Spring-damped chase camera behind the driven chassis
    Orbit         = 1,   // [-] - Spherical turntable / exterior inspection orbit
    Cockpit       = 2,   // [-] - Driver seated first-person view with head-look
    Hmi           = 3,   // [-] - Centre-console HMI tablet focus framing
    Passenger     = 4,   // [-] - Front passenger seated view with head-look
    Hood          = 5,   // [-] - Bonnet-mounted forward camera
    TyreFrontLeft = 6,   // [-] - Low wheel-arch rig aimed at front-left tyre contact patch
    TyreRearLeft  = 7    // [-] - Low trailing rig aimed at rear-left tyre contact patch
};

[[nodiscard]] std::string_view QueryVehicleCameraRigLabel(VehicleCameraRig Rig) noexcept;

struct VehicleSubjectKinematics
{
    bool                  Available            = true;
    std::array<double, 3> PositionMetres       = {0.0, 0.0, 0.525};
    std::array<double, 3> ForwardUnit          = {1.0, 0.0, 0.0};
    std::array<double, 3> LeftUnit             = {0.0, 1.0, 0.0};
    std::array<double, 3> UpUnit               = {0.0, 0.0, 1.0};
    double                SpeedMetresPerSecond = 0.0;
    std::array<double, 3> VelocityMetresPerSec = {0.0, 0.0, 0.0};
    std::array<double, 3> FrontLeftHubMetres   = {1.7274, 1.0475, 0.51};
    std::array<double, 3> RearRightHubMetres   = {-1.6686, -1.0475, 0.51};
    double                AspectRatio          = 16.0 / 9.0;
    double                HmiScale             = 1.0;
    double                GroundFloorMetres    = 0.0;
};

struct VehicleCameraInput
{
    std::array<double, 2> PointerDeltaPixels = {0.0, 0.0};
    double                WheelNotches       = 0.0;
    bool                  PrimaryDrag        = false;
    bool                  CycleNextRig       = false;
    bool                  CyclePreviousRig   = false;
    bool                  AnyActivity        = false;
};

struct VehicleCameraSolution
{
    std::array<double, 3> PositionMetres             = {-7.0, 0.0, 2.6};
    std::array<double, 3> TargetMetres               = {0.0, 0.0, 0.8};
    std::array<double, 3> UpUnit                     = {0.0, 0.0, 1.0};
    double                VerticalFieldOfViewRadians = 1.0471975511965976;
    double                NearClipMetres             = 0.05;
    double                VeilOpacity                = 0.0;
    VehicleCameraRig     ActiveRig                 = VehicleCameraRig::Orbit;
    bool                  EntranceDollyActive        = false;
    bool                  CinematicActive            = false;
    uint32_t              CinematicShotIndex         = 0u;
    bool                  TransitionActive           = false;
};

[[nodiscard]] double                EvaluateCinematicEase(double Progress) noexcept;
[[nodiscard]] double                EvaluateExponentialLagBlend(double DeltaSeconds, double TimeConstantSeconds) noexcept;
[[nodiscard]] std::array<double, 3> ClampLobbyStudioEye(const std::array<double, 3>& EyeMetres) noexcept;

class VehicleCameraSolver
{
public:
    VehicleCameraSolver();

    void ConfigureEnvironment(bool InLobbyStudio, const VehicleSubjectKinematics& InitialSubject = {});
    void ConfigureRigImmediate(VehicleCameraRig NextRig, const VehicleSubjectKinematics& Subject);
    void TransitionToRig(VehicleCameraRig NextRig);
    void CycleRig(bool Reverse = false);

    void               BeginRouteTransition(double CoverDurationSeconds = 0.55, double RevealDurationSeconds = 0.65);
    [[nodiscard]] bool ConsumeRouteSwitchPoint() noexcept;
    [[nodiscard]] bool QueryRouteTransitionActive() const noexcept { return RoutePhase != RouteFadePhase::Idle; }

    void                  RegisterVisitorInteraction() noexcept;
    VehicleCameraSolution Advance(const VehicleSubjectKinematics& Subject, const VehicleCameraInput& Input, double DeltaSeconds);

    [[nodiscard]] bool                         QueryLobbyStudio() const noexcept { return LobbyStudio; }
    [[nodiscard]] VehicleCameraRig            QueryRig() const noexcept { return Rig; }
    [[nodiscard]] const VehicleCameraSolution& QueryLastSolution() const noexcept { return LastSolution; }
    [[nodiscard]] CameraShakeComponent& AccessCameraShake() noexcept { return CameraShake; }
    [[nodiscard]] const CameraShakeComponent& QueryCameraShake() const noexcept { return CameraShake; }
    [[nodiscard]] CameraMotionBlurComponent& AccessMotionBlur() noexcept { return MotionBlur; }
    [[nodiscard]] const CameraMotionBlurComponent& QueryMotionBlur() const noexcept { return MotionBlur; }

private:
    enum class CinematicPhase : uint8_t
    {
        Inactive = 0,
        Shot     = 1,
        Cover    = 2,
        Reveal   = 3
    };

    enum class RouteFadePhase : uint8_t
    {
        Idle   = 0,
        Cover  = 1,
        Reveal = 2
    };

    struct OrbitPose
    {
        double YawRadians     = 1.15;
        double PitchRadians   = 0.20;
        double DistanceMetres = 6.6;
    };

    struct RigDollyBlend
    {
        bool                  Active             = false;
        std::array<double, 3> FromPositionMetres = {0.0, 0.0, 0.0};
        std::array<double, 3> FromTargetMetres   = {1.0, 0.0, 0.0};
        std::array<double, 3> FromUpUnit         = {0.0, 0.0, 1.0};
        double                FromFieldOfViewRad = 1.0471975511965976;
        double                ElapsedSeconds     = 0.0;
        double                DurationSeconds    = 1.7;
    };

    [[nodiscard]] VehicleCameraSolution EvaluateCinematicShotPose(const VehicleSubjectKinematics& Subject,
                                                                  uint32_t                        ShotIndex,
                                                                  double                          ShotSeconds) const noexcept;
    void                                AdvanceLobbyCinematic(const VehicleSubjectKinematics& Subject, double DeltaSeconds) noexcept;

    bool                  LobbyStudio                 = true;
    VehicleCameraRig     Rig                        = VehicleCameraRig::Orbit;
    OrbitPose             Orbit{};
    double                OrbitHoldRemainingSeconds   = 5.0;
    bool                  EntranceDollyActive         = true;
    double                EntranceDollyElapsedSeconds = 0.0;
    double                EntranceDollyDurationSeconds = 7.0;
    OrbitPose             EntranceFrom{2.45, 0.04, 17.5};
    OrbitPose             EntranceTo{1.15, 0.20, 6.6};
    bool                  LagInitialized              = false;
    std::array<double, 3> LaggedEyeMetres             = {0.0, 0.0, 0.0};
    std::array<double, 3> LaggedTargetMetres          = {0.0, 0.0, 0.0};
    bool                  VisitorTouchedCamera        = false;
    double                IdleElapsedSeconds          = 0.0;
    CinematicPhase        TourPhase                   = CinematicPhase::Inactive;
    uint32_t              TourShotIndex               = 0u;
    double                TourPhaseSeconds            = 0.0;
    VehicleCameraSolution TourPose{};
    RigDollyBlend         RigBlend{};
    RouteFadePhase        RoutePhase                  = RouteFadePhase::Idle;
    double                RoutePhaseSeconds           = 0.0;
    double                RouteCoverSeconds           = 0.55;
    double                RouteRevealSeconds          = 0.65;
    bool                  RouteSwitchReady            = false;
    bool                  ChaseInitialized            = false;
    std::array<double, 3> ChaseEyeMetres              = {-7.0, 0.0, 2.6};
    std::array<double, 3> ChaseTargetMetres           = {0.0, 0.0, 0.8};
    double                ChaseOrbitYawRad            = 0.0;
    double                ChaseOrbitPitchRad          = 0.22;
    double                ChaseDistanceScale          = 1.0;
    CameraShakeComponent  CameraShake{};
    CameraMotionBlurComponent MotionBlur{};
    double                HeadYawRadians              = 0.0;
    double                HeadPitchRadians            = 0.0;
    double                InteriorFovDegrees          = 70.0;
    double                HmiZoom                     = 1.0;
    VehicleCameraSolution LastSolution{};
};

} // namespace Frontier
