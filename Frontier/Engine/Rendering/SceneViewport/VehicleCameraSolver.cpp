/*==========================================================================================================================================
                                                        VEHICLECAMERASOLVER.CPP
==========================================================================================================================================*/
// 🧩 1:1 C++20 port of VehicleHMI camera modules: LobbyCameraMotion, VehicleOrbitCamera, ChaseCameraSolver, DrivingCameraMotion & 8 rigs.

#include "Frontier/Engine/Rendering/SceneViewport/VehicleCameraSolver.h"

#include <algorithm>
#include <cmath>

namespace Frontier
{

namespace
{

constexpr double   Pi                  = 3.14159265358979323846;
constexpr double   DegreesToRadians    = Pi / 180.0;
constexpr double   StudioRadiusCeiling = 18.4;
constexpr double   StudioFloorMinimum  = 0.35;
constexpr double   StudioSoftboxCeiling = 5.25;
constexpr double   EyeLagTimeConstant  = 0.34;
constexpr double   TargetLagTimeConst  = 0.20;
constexpr double   AttractIdleThreshold = 10.0;
constexpr double   VisitorIdleThreshold = 60.0;
constexpr double   CinematicShotSeconds = 6.5;
constexpr double   CinematicFadeSeconds = 0.7;
constexpr uint32_t CinematicShotCount   = 6u;
constexpr double   ChaseFollowDistance  = 7.0;
constexpr double   ChaseFollowHeight    = 2.71;
constexpr double   ChaseLookAtHeight    = 0.91;
constexpr double   ChasePositionSpring  = 6.0;
constexpr double   ChaseYawDecayRate    = 0.0;

std::array<double, 3> Add3(const std::array<double, 3>& A, const std::array<double, 3>& B) noexcept
{
    return {A[0] + B[0], A[1] + B[1], A[2] + B[2]};
}

std::array<double, 3> Subtract3(const std::array<double, 3>& A, const std::array<double, 3>& B) noexcept
{
    return {A[0] - B[0], A[1] - B[1], A[2] - B[2]};
}

std::array<double, 3> Scale3(const std::array<double, 3>& Vector, double Scalar) noexcept
{
    return {Vector[0] * Scalar, Vector[1] * Scalar, Vector[2] * Scalar};
}

std::array<double, 3> Lerp3(const std::array<double, 3>& From, const std::array<double, 3>& To, double Weight) noexcept
{
    return {From[0] + (To[0] - From[0]) * Weight,
            From[1] + (To[1] - From[1]) * Weight,
            From[2] + (To[2] - From[2]) * Weight};
}

std::array<double, 3> Normalize3(const std::array<double, 3>& Vector) noexcept
{
    const double Length = std::sqrt(Vector[0] * Vector[0] + Vector[1] * Vector[1] + Vector[2] * Vector[2]);
    if (Length <= 1.0e-9)
    {
        return {0.0, 0.0, 1.0};
    }
    return {Vector[0] / Length, Vector[1] / Length, Vector[2] / Length};
}

std::array<double, 3> TransformLocalPoint(const VehicleSubjectKinematics& Subject, double ForwardX, double LeftY, double UpZ) noexcept
{
    return {
        Subject.PositionMetres[0] + Subject.ForwardUnit[0] * ForwardX + Subject.LeftUnit[0] * LeftY + Subject.UpUnit[0] * UpZ,
        Subject.PositionMetres[1] + Subject.ForwardUnit[1] * ForwardX + Subject.LeftUnit[1] * LeftY + Subject.UpUnit[1] * UpZ,
        Subject.PositionMetres[2] + Subject.ForwardUnit[2] * ForwardX + Subject.LeftUnit[2] * LeftY + Subject.UpUnit[2] * UpZ,
    };
}

std::array<double, 3> TransformLocalDirection(const VehicleSubjectKinematics& Subject, const std::array<double, 3>& Local) noexcept
{
    return Normalize3({
        Subject.ForwardUnit[0] * Local[0] + Subject.LeftUnit[0] * Local[1] + Subject.UpUnit[0] * Local[2],
        Subject.ForwardUnit[1] * Local[0] + Subject.LeftUnit[1] * Local[1] + Subject.UpUnit[1] * Local[2],
        Subject.ForwardUnit[2] * Local[0] + Subject.LeftUnit[2] * Local[1] + Subject.UpUnit[2] * Local[2],
    });
}

void ResolveHmiTabletView(const VehicleSubjectKinematics& Subject,
                          double                          Zoom,
                          std::array<double, 3>&          OutEye,
                          std::array<double, 3>&          OutTarget,
                          std::array<double, 3>&          OutUp,
                          double&                         OutFieldOfViewRadians) noexcept
{
    constexpr std::array<double, 3> ScreenCentre = {0.9672, 0.0, 0.5129};
    constexpr double                TiltRadians  = 0.42;
    const std::array<double, 3>     NormalLocal  = {-std::cos(TiltRadians), 0.0, std::sin(TiltRadians)};
    const std::array<double, 3>     UpLocal      = {std::sin(TiltRadians), 0.0, std::cos(TiltRadians)};

    const double     Scale          = std::clamp(Subject.HmiScale, 0.5, 2.0);
    const double     TabletWidth    = 0.30 * Scale;
    const double     TabletHeight   = 0.1875 * Scale;
    const double     FieldOfViewRad = 40.0 * DegreesToRadians;
    const double     HalfTan        = std::tan(0.5 * FieldOfViewRad);
    const double     Aspect         = std::max(0.25, Subject.AspectRatio);
    constexpr double Margin         = 0.86;
    const double     DistanceV      = (0.5 * TabletHeight / Margin) / HalfTan;
    const double     DistanceH      = (0.5 * TabletWidth / Margin) / (HalfTan * Aspect);
    const double     Distance       = std::max(DistanceV, DistanceH) * std::clamp(Zoom, 0.65, 2.0);

    const std::array<double, 3> EyeLocal = {
        ScreenCentre[0] + NormalLocal[0] * Distance,
        ScreenCentre[1] + NormalLocal[1] * Distance,
        ScreenCentre[2] + NormalLocal[2] * Distance,
    };
    OutEye                = TransformLocalPoint(Subject, EyeLocal[0], EyeLocal[1], EyeLocal[2]);
    OutTarget             = TransformLocalPoint(Subject, ScreenCentre[0], ScreenCentre[1], ScreenCentre[2]);
    OutUp                 = TransformLocalDirection(Subject, UpLocal);
    OutFieldOfViewRadians = FieldOfViewRad;
}

} // namespace

std::string_view QueryVehicleCameraRigLabel(VehicleCameraRig Rig) noexcept
{
    switch (Rig)
    {
        case VehicleCameraRig::Chase:         return "Chase";
        case VehicleCameraRig::Orbit:         return "Orbit";
        case VehicleCameraRig::Cockpit:       return "Cockpit";
        case VehicleCameraRig::Hmi:           return "HMI Tablet";
        case VehicleCameraRig::Passenger:     return "Passenger";
        case VehicleCameraRig::Hood:          return "Hood";
        case VehicleCameraRig::TyreFrontLeft: return "Tyre FL";
        case VehicleCameraRig::TyreRearLeft:  return "Tyre RL";
    }
    return "Orbit";
}

double EvaluateCinematicEase(double Progress) noexcept
{
    const double Clamped = std::clamp(Progress, 0.0, 1.0);
    return Clamped * Clamped * Clamped * (Clamped * (Clamped * 6.0 - 15.0) + 10.0);
}

double EvaluateExponentialLagBlend(double DeltaSeconds, double TimeConstantSeconds) noexcept
{
    if (!(DeltaSeconds > 0.0) || !(TimeConstantSeconds > 0.0))
    {
        return 1.0;
    }
    return std::min(1.0, 1.0 - std::exp(-DeltaSeconds / TimeConstantSeconds));
}

std::array<double, 3> ClampLobbyStudioEye(const std::array<double, 3>& EyeMetres) noexcept
{
    double       X      = EyeMetres[0];
    double       Y      = EyeMetres[1];
    const double Z      = std::clamp(EyeMetres[2], StudioFloorMinimum, StudioSoftboxCeiling);
    const double Radius = std::hypot(X, Y);
    if (Radius > StudioRadiusCeiling)
    {
        const double Scale = StudioRadiusCeiling / Radius;
        X *= Scale;
        Y *= Scale;
    }
    return {X, Y, Z};
}

VehicleCameraSolver::VehicleCameraSolver()
{
    ConfigureEnvironment(true, VehicleSubjectKinematics{});
}

void VehicleCameraSolver::ConfigureEnvironment(bool InLobbyStudio, const VehicleSubjectKinematics& InitialSubject)
{
    LobbyStudio           = InLobbyStudio;
    VisitorTouchedCamera  = false;
    IdleElapsedSeconds    = 0.0;
    TourPhase             = CinematicPhase::Inactive;
    TourShotIndex         = 0u;
    TourPhaseSeconds      = 0.0;
    RigBlend              = {};
    CameraShake.Reset();
    HeadYawRadians        = 0.0;
    HeadPitchRadians      = 0.0;
    InteriorFovDegrees    = 70.0;
    HmiZoom               = 1.0;
    LagInitialized        = false;
    ChaseInitialized      = false;

    if (LobbyStudio)
    {
        Rig                        = VehicleCameraRig::Orbit;
        EntranceDollyActive         = true;
        EntranceDollyElapsedSeconds = 0.0;
        Orbit                       = EntranceFrom;
        OrbitHoldRemainingSeconds   = 5.0;
    }
    else
    {
        Rig                 = VehicleCameraRig::Chase;
        EntranceDollyActive  = false;
        Orbit.YawRadians     = Pi + 0.4;
        Orbit.PitchRadians   = 0.30;
        Orbit.DistanceMetres = 8.0;
    }

    (void)Advance(InitialSubject, VehicleCameraInput{}, 0.0);
}

void VehicleCameraSolver::ConfigureRigImmediate(VehicleCameraRig NextRig, const VehicleSubjectKinematics& Subject)
{
    Rig                = NextRig;
    EntranceDollyActive = false;
    TourPhase           = CinematicPhase::Inactive;
    RigBlend            = {};
    CameraShake.Reset();
    HeadYawRadians      = 0.0;
    HeadPitchRadians    = 0.0;
    ChaseOrbitYawRad    = 0.0;
    ChaseOrbitPitchRad  = 0.22;
    ChaseDistanceScale  = 1.0;
    HmiZoom             = 1.0;
    LagInitialized      = false;
    ChaseInitialized    = false;
    if (LobbyStudio && Rig == VehicleCameraRig::Orbit)
    {
        Orbit = EntranceTo;
    }
    (void)Advance(Subject, VehicleCameraInput{}, 0.0);
}

void VehicleCameraSolver::TransitionToRig(VehicleCameraRig NextRig)
{
    if (NextRig == Rig && TourPhase == CinematicPhase::Inactive)
    {
        return;
    }
    EntranceDollyActive         = false;
    TourPhase                   = CinematicPhase::Inactive;
    IdleElapsedSeconds          = 0.0;
    CameraShake.Reset();
    HeadYawRadians              = 0.0;
    HeadPitchRadians            = 0.0;
    if (NextRig == VehicleCameraRig::Chase)
    {
        ChaseOrbitYawRad   = 0.0;
        ChaseOrbitPitchRad = 0.22;
        ChaseDistanceScale = 1.0;
    }
    RigBlend.Active             = true;
    RigBlend.FromPositionMetres = LastSolution.PositionMetres;
    RigBlend.FromTargetMetres   = LastSolution.TargetMetres;
    RigBlend.FromUpUnit         = LastSolution.UpUnit;
    RigBlend.FromFieldOfViewRad = LastSolution.VerticalFieldOfViewRadians;
    RigBlend.ElapsedSeconds     = 0.0;
    if (LobbyStudio)
    {
        RigBlend.DurationSeconds = (NextRig == VehicleCameraRig::Hmi) ? 2.1 : 1.7;
        if (NextRig == VehicleCameraRig::Orbit && Orbit.DistanceMetres > 6.9)
        {
            Orbit = EntranceTo;
        }
    }
    else
    {
        RigBlend.DurationSeconds = 0.85;
    }
    Rig           = NextRig;
    LagInitialized = false;
}

void VehicleCameraSolver::CycleRig(bool Reverse)
{
    if (LobbyStudio)
    {
        static constexpr std::array<VehicleCameraRig, 3> StudioRigs = {
            VehicleCameraRig::Orbit,
            VehicleCameraRig::Cockpit,
            VehicleCameraRig::Hmi,
        };
        size_t CurrentIndex = 0u;
        for (size_t Index = 0u; Index < StudioRigs.size(); ++Index)
        {
            if (StudioRigs[Index] == Rig)
            {
                CurrentIndex = Index;
                break;
            }
        }
        const size_t NextIndex = Reverse ? (CurrentIndex + StudioRigs.size() - 1u) % StudioRigs.size()
                                         : (CurrentIndex + 1u) % StudioRigs.size();
        TransitionToRig(StudioRigs[NextIndex]);
        return;
    }

    constexpr uint32_t TotalRigs = 8u;
    const uint32_t     Index     = static_cast<uint32_t>(Rig);
    const uint32_t     NextIndex = Reverse ? (Index + TotalRigs - 1u) % TotalRigs : (Index + 1u) % TotalRigs;
    TransitionToRig(static_cast<VehicleCameraRig>(NextIndex));
}

void VehicleCameraSolver::BeginRouteTransition(double CoverDurationSeconds, double RevealDurationSeconds)
{
    RoutePhase         = RouteFadePhase::Cover;
    RoutePhaseSeconds  = 0.0;
    RouteCoverSeconds  = std::max(0.1, CoverDurationSeconds);
    RouteRevealSeconds = std::max(0.1, RevealDurationSeconds);
    RouteSwitchReady   = false;
}

bool VehicleCameraSolver::ConsumeRouteSwitchPoint() noexcept
{
    if (!RouteSwitchReady)
    {
        return false;
    }
    RouteSwitchReady = false;
    return true;
}

void VehicleCameraSolver::RegisterVisitorInteraction() noexcept
{
    VisitorTouchedCamera = true;
    IdleElapsedSeconds   = 0.0;
    if (TourPhase != CinematicPhase::Inactive)
    {
        TourPhase        = CinematicPhase::Inactive;
        TourPhaseSeconds = 0.0;
        LagInitialized   = false;
    }
}

VehicleCameraSolution VehicleCameraSolver::EvaluateCinematicShotPose(const VehicleSubjectKinematics& Subject,
                                                                     uint32_t                        ShotIndex,
                                                                     double                          ShotSeconds) const noexcept
{
    const double Ease = EvaluateCinematicEase(ShotSeconds / CinematicShotSeconds);
    auto OffsetHub = [&](const std::array<double, 3>& Hub, double ForwardX, double LeftY, double UpZ)
    {
        return std::array<double, 3>{
            Hub[0] + Subject.ForwardUnit[0] * ForwardX + Subject.LeftUnit[0] * LeftY + Subject.UpUnit[0] * UpZ,
            Hub[1] + Subject.ForwardUnit[1] * ForwardX + Subject.LeftUnit[1] * LeftY + Subject.UpUnit[1] * UpZ,
            Hub[2] + Subject.ForwardUnit[2] * ForwardX + Subject.LeftUnit[2] * LeftY + Subject.UpUnit[2] * UpZ,
        };
    };

    VehicleCameraSolution Shot{};
    Shot.UpUnit             = {0.0, 0.0, 1.0};
    Shot.NearClipMetres     = 0.03;
    Shot.ActiveRig         = VehicleCameraRig::Orbit;
    Shot.CinematicActive    = true;
    Shot.CinematicShotIndex = ShotIndex % CinematicShotCount;

    switch (Shot.CinematicShotIndex)
    {
        case 0u:
            Shot.PositionMetres             = Lerp3(TransformLocalPoint(Subject, 8.0, 5.5, 1.4), TransformLocalPoint(Subject, 5.5, 7.0, 0.9), Ease);
            Shot.TargetMetres               = TransformLocalPoint(Subject, 0.0, 0.0, 0.1);
            Shot.VerticalFieldOfViewRadians = 55.0 * DegreesToRadians;
            break;
        case 1u:
            Shot.PositionMetres             = Lerp3(OffsetHub(Subject.FrontLeftHubMetres, 1.8, 2.0, 0.30),
                                                    OffsetHub(Subject.FrontLeftHubMetres, 2.4, 1.4, 0.15), Ease);
            Shot.TargetMetres               = Subject.FrontLeftHubMetres;
            Shot.VerticalFieldOfViewRadians = 42.0 * DegreesToRadians;
            break;
        case 2u:
            Shot.PositionMetres             = Lerp3(TransformLocalPoint(Subject, -4.8, 7.8, 0.2), TransformLocalPoint(Subject, 4.8, 7.8, 0.2), Ease);
            Shot.TargetMetres               = TransformLocalPoint(Subject, 0.0, 0.0, 0.0);
            Shot.VerticalFieldOfViewRadians = 50.0 * DegreesToRadians;
            break;
        case 3u:
            Shot.PositionMetres             = Lerp3(TransformLocalPoint(Subject, -8.0, -5.0, 1.5), TransformLocalPoint(Subject, -6.5, -6.5, 0.9), Ease);
            Shot.TargetMetres               = TransformLocalPoint(Subject, -0.4, 0.0, 0.1);
            Shot.VerticalFieldOfViewRadians = 52.0 * DegreesToRadians;
            break;
        case 4u:
            Shot.PositionMetres             = Lerp3(OffsetHub(Subject.RearRightHubMetres, -2.0, -1.8, 0.35),
                                                    OffsetHub(Subject.RearRightHubMetres, -1.5, -2.4, 0.15), Ease);
            Shot.TargetMetres               = Subject.RearRightHubMetres;
            Shot.VerticalFieldOfViewRadians = 42.0 * DegreesToRadians;
            break;
        default:
            Shot.PositionMetres             = Lerp3(TransformLocalPoint(Subject, 0.0, 13.5, 3.6), TransformLocalPoint(Subject, 3.0, 12.5, 3.0), Ease);
            Shot.TargetMetres               = TransformLocalPoint(Subject, 0.0, 0.0, 0.0);
            Shot.VerticalFieldOfViewRadians = 58.0 * DegreesToRadians;
            break;
    }

    Shot.PositionMetres = ClampLobbyStudioEye(Shot.PositionMetres);
    return Shot;
}

void VehicleCameraSolver::AdvanceLobbyCinematic(const VehicleSubjectKinematics& Subject, double DeltaSeconds) noexcept
{
    TourPhaseSeconds += DeltaSeconds;
    if (TourPhase == CinematicPhase::Shot)
    {
        TourPose             = EvaluateCinematicShotPose(Subject, TourShotIndex, TourPhaseSeconds);
        TourPose.VeilOpacity = 0.0;
        if (TourPhaseSeconds >= CinematicShotSeconds)
        {
            TourPhase        = CinematicPhase::Cover;
            TourPhaseSeconds = 0.0;
        }
    }
    else if (TourPhase == CinematicPhase::Cover)
    {
        TourPose             = EvaluateCinematicShotPose(Subject, TourShotIndex, CinematicShotSeconds);
        TourPose.VeilOpacity = EvaluateCinematicEase(TourPhaseSeconds / CinematicFadeSeconds);
        if (TourPhaseSeconds >= CinematicFadeSeconds)
        {
            TourShotIndex        = (TourShotIndex + 1u) % CinematicShotCount;
            TourPhase            = CinematicPhase::Reveal;
            TourPhaseSeconds     = 0.0;
            TourPose             = EvaluateCinematicShotPose(Subject, TourShotIndex, 0.0);
            TourPose.VeilOpacity = 1.0;
        }
    }
    else if (TourPhase == CinematicPhase::Reveal)
    {
        TourPose             = EvaluateCinematicShotPose(Subject, TourShotIndex, 0.0);
        TourPose.VeilOpacity = 1.0 - EvaluateCinematicEase(TourPhaseSeconds / CinematicFadeSeconds);
        if (TourPhaseSeconds >= CinematicFadeSeconds)
        {
            TourPhase            = CinematicPhase::Shot;
            TourPhaseSeconds     = 0.0;
            TourPose.VeilOpacity = 0.0;
        }
    }
}

VehicleCameraSolution VehicleCameraSolver::Advance(const VehicleSubjectKinematics& Subject,
                                                   const VehicleCameraInput&       Input,
                                                   double                          DeltaSeconds)
{
    const double Δτ = std::clamp(DeltaSeconds, 0.0, 0.25);

    const bool DragOrWheel = (Input.PrimaryDrag && (std::abs(Input.PointerDeltaPixels[0]) > 0.0 || std::abs(Input.PointerDeltaPixels[1]) > 0.0)) ||
                             std::abs(Input.WheelNotches) > 0.0;
    if (Input.AnyActivity || DragOrWheel || Input.CycleNextRig || Input.CyclePreviousRig)
    {
        if (LobbyStudio)
        {
            RegisterVisitorInteraction();
            if (DragOrWheel)
            {
                EntranceDollyActive       = false;
                OrbitHoldRemainingSeconds = 60.0;
            }
        }
    }
    if (Input.CycleNextRig)
    {
        CycleRig(false);
    }
    else if (Input.CyclePreviousRig)
    {
        CycleRig(true);
    }

    const bool InteriorRig = (Rig == VehicleCameraRig::Cockpit || Rig == VehicleCameraRig::Passenger);
    if (Rig == VehicleCameraRig::Orbit)
    {
        if (Input.PrimaryDrag)
        {
            Orbit.YawRadians  -= Input.PointerDeltaPixels[0] * 0.006;
            Orbit.PitchRadians = std::clamp(Orbit.PitchRadians + Input.PointerDeltaPixels[1] * 0.005, -0.15, 1.50);
        }
        if (std::abs(Input.WheelNotches) > 0.0)
        {
            const double MaximumDistance = LobbyStudio ? 6.9 : 40.0;
            Orbit.DistanceMetres         = std::clamp(Orbit.DistanceMetres * std::exp(-Input.WheelNotches * 0.12), 4.5, MaximumDistance);
        }
    }
    else if (Rig == VehicleCameraRig::Chase)
    {
        if (Input.PrimaryDrag)
        {
            ChaseOrbitYawRad  -= Input.PointerDeltaPixels[0] * 0.005;
            ChaseOrbitPitchRad = std::clamp(ChaseOrbitPitchRad + Input.PointerDeltaPixels[1] * 0.004, -0.30, 0.90);
        }
        else if (Δτ > 0.0)
        {
            ChaseOrbitYawRad *= std::exp(-ChaseYawDecayRate * Δτ);
            if (std::abs(ChaseOrbitYawRad) < 1.0e-4)
            {
                ChaseOrbitYawRad = 0.0;
            }
        }
        if (std::abs(Input.WheelNotches) > 0.0)
        {
            ChaseDistanceScale = std::clamp(ChaseDistanceScale * std::exp(-Input.WheelNotches * 0.10), 0.5, 3.0);
        }
    }
    else if (InteriorRig)
    {
        if (Input.PrimaryDrag)
        {
            HeadYawRadians   = std::clamp(HeadYawRadians - Input.PointerDeltaPixels[0] * 0.004, -1.9, 1.9);
            HeadPitchRadians = std::clamp(HeadPitchRadians + Input.PointerDeltaPixels[1] * 0.004, -0.8, 0.8);
        }
        if (std::abs(Input.WheelNotches) > 0.0)
        {
            InteriorFovDegrees = std::clamp(InteriorFovDegrees - Input.WheelNotches * 3.0, 30.0, 90.0);
        }
    }
    else if (Rig == VehicleCameraRig::Hmi)
    {
        if (std::abs(Input.WheelNotches) > 0.0)
        {
            HmiZoom = std::clamp(HmiZoom * std::exp(-Input.WheelNotches * 0.10), 0.65, 2.0);
        }
    }

    double Veil = 0.0;
    if (LobbyStudio && Rig == VehicleCameraRig::Orbit)
    {
        if (EntranceDollyActive)
        {
            EntranceDollyElapsedSeconds += Δτ;
            const double Ease      = EvaluateCinematicEase(EntranceDollyElapsedSeconds / EntranceDollyDurationSeconds);
            Orbit.YawRadians       = EntranceFrom.YawRadians + (EntranceTo.YawRadians - EntranceFrom.YawRadians) * Ease;
            Orbit.PitchRadians     = EntranceFrom.PitchRadians + (EntranceTo.PitchRadians - EntranceFrom.PitchRadians) * Ease;
            Orbit.DistanceMetres   = EntranceFrom.DistanceMetres + (EntranceTo.DistanceMetres - EntranceFrom.DistanceMetres) * Ease;
            if (EntranceDollyElapsedSeconds < 1.1)
            {
                Veil = std::max(Veil, 1.0 - EvaluateCinematicEase(EntranceDollyElapsedSeconds / 1.1));
            }
            if (EntranceDollyElapsedSeconds >= EntranceDollyDurationSeconds)
            {
                EntranceDollyActive = false;
                IdleElapsedSeconds  = 0.0;
            }
        }
        else
        {
            Orbit.DistanceMetres = std::min(Orbit.DistanceMetres, 6.9);
            if (!RigBlend.Active)
            {
                if (TourPhase != CinematicPhase::Inactive)
                {
                    AdvanceLobbyCinematic(Subject, Δτ);
                }
                else
                {
                    IdleElapsedSeconds += Δτ;
                    const double Threshold = VisitorTouchedCamera ? VisitorIdleThreshold : AttractIdleThreshold;
                    if (IdleElapsedSeconds >= Threshold)
                    {
                        TourPhase        = CinematicPhase::Shot;
                        TourShotIndex    = 0u;
                        TourPhaseSeconds = 0.0;
                        TourPose         = EvaluateCinematicShotPose(Subject, 0u, 0.0);
                    }
                    else
                    {
                        OrbitHoldRemainingSeconds = std::max(0.0, OrbitHoldRemainingSeconds - Δτ);
                        if (OrbitHoldRemainingSeconds <= 0.0)
                        {
                            Orbit.YawRadians = std::fmod(Orbit.YawRadians + Δτ * 0.06, 2.0 * Pi);
                        }
                    }
                }
            }
        }
    }
    else
    {
        TourPhase = CinematicPhase::Inactive;
    }

    double SpeedFovRadians = 0.0;
    if (!LobbyStudio)
    {
        const double SpeedFraction = std::clamp(Subject.SpeedMetresPerSecond / 55.0, 0.0, 1.0);
        const double SmoothSpeed   = SpeedFraction * SpeedFraction * (3.0 - 2.0 * SpeedFraction);
        SpeedFovRadians            = (14.0 * DegreesToRadians) * SmoothSpeed;
    }

    std::array<double, 3> DesiredEye    = {0.0, 0.0, 1.8};
    std::array<double, 3> DesiredTarget = Subject.PositionMetres;
    std::array<double, 3> DesiredUp     = {0.0, 0.0, 1.0};
    double                DesiredFovRad = 60.0 * DegreesToRadians;
    double                DesiredNear   = LobbyStudio ? 0.03 : 0.05;

    if (Rig == VehicleCameraRig::Orbit)
    {
        DesiredTarget         = {Subject.PositionMetres[0], Subject.PositionMetres[1], Subject.PositionMetres[2] + 0.46};
        const double CosPitch = std::cos(Orbit.PitchRadians);
        DesiredEye            = {
            DesiredTarget[0] + Orbit.DistanceMetres * CosPitch * std::cos(Orbit.YawRadians),
            DesiredTarget[1] + Orbit.DistanceMetres * CosPitch * std::sin(Orbit.YawRadians),
            std::max(Subject.GroundFloorMetres + 0.20, DesiredTarget[2] + Orbit.DistanceMetres * std::sin(Orbit.PitchRadians)),
        };
    }
    else if (Rig == VehicleCameraRig::Chase)
    {
        const double Heading  = std::atan2(Subject.ForwardUnit[1], Subject.ForwardUnit[0]) + ChaseOrbitYawRad;
        const double Distance = ChaseFollowDistance * ChaseDistanceScale;
        const double Height   = ChaseFollowHeight * ChaseDistanceScale + std::sin(ChaseOrbitPitchRad - 0.22) * Distance * 0.45;
        const std::array<double, 3> GoalEye = {
            Subject.PositionMetres[0] - std::cos(Heading) * Distance,
            Subject.PositionMetres[1] - std::sin(Heading) * Distance,
            std::max(Subject.GroundFloorMetres + 0.50, Subject.PositionMetres[2] + Height),
        };
        const std::array<double, 3> GoalTarget = {
            Subject.PositionMetres[0] + Subject.ForwardUnit[0] * 2.2,
            Subject.PositionMetres[1] + Subject.ForwardUnit[1] * 2.2,
            Subject.PositionMetres[2] + ChaseLookAtHeight,
        };
        if (!ChaseInitialized || Δτ <= 0.0)
        {
            ChaseEyeMetres    = GoalEye;
            ChaseTargetMetres = GoalTarget;
            ChaseInitialized  = true;
        }
        else
        {
            const double Alpha = 1.0 - std::exp(-ChasePositionSpring * Δτ);
            ChaseEyeMetres     = Lerp3(ChaseEyeMetres, GoalEye, Alpha);
            ChaseTargetMetres  = Lerp3(ChaseTargetMetres, GoalTarget, std::min(1.0, Alpha * 1.45));
        }
        DesiredEye    = ChaseEyeMetres;
        DesiredTarget = ChaseTargetMetres;
        DesiredFovRad = 60.0 * DegreesToRadians + SpeedFovRadians;
    }
    else if (Rig == VehicleCameraRig::Hmi)
    {
        ResolveHmiTabletView(Subject, HmiZoom, DesiredEye, DesiredTarget, DesiredUp, DesiredFovRad);
        DesiredNear = 0.01;
    }
    else if (Rig == VehicleCameraRig::Cockpit || Rig == VehicleCameraRig::Passenger)
    {
        const bool Driver  = (Rig == VehicleCameraRig::Cockpit);
        DesiredEye         = TransformLocalPoint(Subject, Driver ? -0.20 : -0.22, Driver ? 0.42 : -0.42, Driver ? 1.00715 : 0.98715);
        const double Pitch = (Driver ? 0.08 : 0.07) + HeadPitchRadians;
        const double Yaw   = HeadYawRadians;
        const std::array<double, 3> LookLocal = {std::cos(Pitch) * std::cos(Yaw), std::cos(Pitch) * std::sin(Yaw), -std::sin(Pitch)};
        DesiredTarget      = Add3(DesiredEye, Scale3(TransformLocalDirection(Subject, LookLocal), 10.0));
        DesiredUp          = Subject.UpUnit;
        DesiredFovRad      = InteriorFovDegrees * DegreesToRadians + 0.35 * SpeedFovRadians;
        DesiredNear        = 0.02;
    }
    else if (Rig == VehicleCameraRig::Hood)
    {
        DesiredEye                            = TransformLocalPoint(Subject, 1.45, 0.00, 0.72);
        const std::array<double, 3> LookLocal = {std::cos(0.05), 0.0, -std::sin(0.05)};
        DesiredTarget                         = Add3(DesiredEye, Scale3(TransformLocalDirection(Subject, LookLocal), 10.0));
        DesiredUp                             = Subject.UpUnit;
        DesiredFovRad                         = 62.0 * DegreesToRadians + 0.5 * SpeedFovRadians;
    }
    else if (Rig == VehicleCameraRig::TyreFrontLeft)
    {
        DesiredEye    = TransformLocalPoint(Subject, 2.85, 1.95, 0.04);
        DesiredTarget = TransformLocalPoint(Subject, 1.7274, 1.0475, -0.08);
        DesiredUp     = Subject.UpUnit;
        DesiredFovRad = 38.0 * DegreesToRadians;
    }
    else if (Rig == VehicleCameraRig::TyreRearLeft)
    {
        DesiredEye    = TransformLocalPoint(Subject, -3.05, 1.95, 0.04);
        DesiredTarget = TransformLocalPoint(Subject, -1.6686, 1.0475, -0.08);
        DesiredUp     = Subject.UpUnit;
        DesiredFovRad = 38.0 * DegreesToRadians;
    }

    VehicleCameraSolution Resolved{};
    Resolved.ActiveRig          = Rig;
    Resolved.EntranceDollyActive = LobbyStudio && EntranceDollyActive && Rig == VehicleCameraRig::Orbit;
    Resolved.NearClipMetres      = DesiredNear;

    if (LobbyStudio && Rig == VehicleCameraRig::Orbit)
    {
        DesiredEye = ClampLobbyStudioEye(DesiredEye);
    }

    if (RigBlend.Active)
    {
        RigBlend.ElapsedSeconds += Δτ;
        const double Ease                   = EvaluateCinematicEase(RigBlend.ElapsedSeconds / RigBlend.DurationSeconds);
        Resolved.PositionMetres             = Lerp3(RigBlend.FromPositionMetres, DesiredEye, Ease);
        Resolved.TargetMetres               = Lerp3(RigBlend.FromTargetMetres, DesiredTarget, Ease);
        Resolved.UpUnit                     = Normalize3(Lerp3(RigBlend.FromUpUnit, DesiredUp, Ease));
        Resolved.VerticalFieldOfViewRadians = RigBlend.FromFieldOfViewRad + (DesiredFovRad - RigBlend.FromFieldOfViewRad) * Ease;
        Resolved.TransitionActive           = true;
        if (LobbyStudio && Rig == VehicleCameraRig::Orbit)
        {
            Resolved.PositionMetres = ClampLobbyStudioEye(Resolved.PositionMetres);
        }
        if (RigBlend.ElapsedSeconds >= RigBlend.DurationSeconds)
        {
            RigBlend.Active    = false;
            LaggedEyeMetres    = DesiredEye;
            LaggedTargetMetres = DesiredTarget;
            LagInitialized     = true;
        }
    }
    else if (LobbyStudio && TourPhase != CinematicPhase::Inactive && Rig == VehicleCameraRig::Orbit)
    {
        Resolved           = TourPose;
        Veil               = std::max(Veil, TourPose.VeilOpacity);
        LaggedEyeMetres    = Resolved.PositionMetres;
        LaggedTargetMetres = Resolved.TargetMetres;
        LagInitialized     = true;
    }
    else if (LobbyStudio && Rig == VehicleCameraRig::Orbit)
    {
        if (!LagInitialized)
        {
            LaggedEyeMetres    = DesiredEye;
            LaggedTargetMetres = DesiredTarget;
            LagInitialized     = true;
        }
        else
        {
            LaggedEyeMetres    = Lerp3(LaggedEyeMetres, DesiredEye, EvaluateExponentialLagBlend(Δτ, EyeLagTimeConstant));
            LaggedTargetMetres = Lerp3(LaggedTargetMetres, DesiredTarget, EvaluateExponentialLagBlend(Δτ, TargetLagTimeConst));
        }
        Resolved.PositionMetres             = ClampLobbyStudioEye(LaggedEyeMetres);
        Resolved.TargetMetres               = LaggedTargetMetres;
        Resolved.UpUnit                     = DesiredUp;
        Resolved.VerticalFieldOfViewRadians = DesiredFovRad;
    }
    else
    {
        Resolved.PositionMetres             = DesiredEye;
        Resolved.TargetMetres               = DesiredTarget;
        Resolved.UpUnit                     = DesiredUp;
        Resolved.VerticalFieldOfViewRadians = DesiredFovRad;
        LagInitialized                      = false;
    }

    if (RoutePhase == RouteFadePhase::Cover)
    {
        RoutePhaseSeconds += Δτ;
        Veil = std::max(Veil, EvaluateCinematicEase(RoutePhaseSeconds / RouteCoverSeconds));
        if (RoutePhaseSeconds >= RouteCoverSeconds)
        {
            RouteSwitchReady  = true;
            RoutePhase        = RouteFadePhase::Reveal;
            RoutePhaseSeconds = 0.0;
            Veil              = 1.0;
        }
        Resolved.TransitionActive = true;
    }
    else if (RoutePhase == RouteFadePhase::Reveal)
    {
        RoutePhaseSeconds += Δτ;
        Veil = std::max(Veil, 1.0 - EvaluateCinematicEase(RoutePhaseSeconds / RouteRevealSeconds));
        if (RoutePhaseSeconds >= RouteRevealSeconds)
        {
            RoutePhase        = RouteFadePhase::Idle;
            RoutePhaseSeconds = 0.0;
        }
        else
        {
            Resolved.TransitionActive = true;
        }
    }

    const bool ShakeRig = !LobbyStudio && Subject.Available &&
                          (Rig == VehicleCameraRig::Chase || Rig == VehicleCameraRig::Cockpit ||
                           Rig == VehicleCameraRig::Passenger || Rig == VehicleCameraRig::Hood);
    if (ShakeRig)
    {
        const CameraShakeOffset Shake = CameraShake.Advance(Subject.VelocityMetresPerSec,
                                                             Subtract3(Resolved.TargetMetres, Resolved.PositionMetres),
                                                             Resolved.UpUnit, Δτ);
        Resolved.PositionMetres = Add3(Resolved.PositionMetres, Shake.EyeMetres);
        Resolved.TargetMetres   = Add3(Resolved.TargetMetres, Shake.TargetMetres);
    }
    else
    {
        CameraShake.Reset();
    }

    Resolved.VeilOpacity = std::clamp(Veil, 0.0, 1.0);
    LastSolution         = Resolved;
    return Resolved;
}

} // namespace Frontier
