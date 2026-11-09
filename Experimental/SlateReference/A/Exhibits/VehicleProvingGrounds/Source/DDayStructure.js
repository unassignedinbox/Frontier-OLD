//============================================================================================================================================
//                                                           DDAYSTRUCTURE.JS
//============================================================================================================================================
// 🧩 Dedicated "D-Day · Militarized Wall Run" ("dday") level geometry, fortifications, ordnance & dynamic combat shape library:
//    • 480 m militarized coastal assault gauntlet from Omaha Beachhead Landing Craft Ramps (X = -156 m) to the towering
//      14 m Reinforced-Concrete Atlantic Fortress Wall & Central Breach Gate (X = +318 m)
//    • Pillbox Bunkers, Two-Story Heavy Casemate Bunkers, Czech Hedgehogs, Concrete Dragon's Teeth, Sandbag & Blast Cover Barricades
//    • Rotating Twin-Barrel Machine-Gun Turrets, Long-Barrel Coastal Artillery Cannons, Moving Allied Assault Tanks (Hull + Turret + Wreck),
//      Fighter-Bomber / Dive-Bomber Aircraft (Dropping Finned Bombs + Kamikaze/Stricken Crash Dives), and Anti-Tank Landmines.

import { CourseMaterial } from "./MaterialSpecification.js";
import { EmitQuad, EmitBoxFacets, EmitPrismFacets } from "./CourseStructure.js";
import { ConstructAxisQuaternion, MultiplyQuaternion, RotateVector } from "./TransformProjection.js";

//------------------------------------------------------------------------------------------------------------------------
//                                  WATERTIGHT 2-MANIFOLD PRIMITIVE HELPERS
//------------------------------------------------------------------------------------------------------------------------

// 📝 Closed outward-wound axis-aligned box with bottom face at BaseZ and top face at BaseZ + SZ.
function EmitBoxAt(CX, CY, BaseZ, SX, SY, SZ, Family, FaceFamilies = null)
{
    const Raw = EmitBoxFacets(SX, SY, SZ, Family, FaceFamilies);
    return Raw.map((F) => ({
        Family:  F.Family,
        Corners: F.Corners.map((P) => [P[0] + CX, P[1] + CY, P[2] + BaseZ])
    }));
}

// 📝 Closed outward-wound 3D-rotated box centered at [CX, CY, CZ] with full extents [SX, SY, SZ].
function EmitRotatedBox(CX, CY, CZ, SX, SY, SZ, Roll, Pitch, Yaw, Family, FaceFamilies = null)
{
    const QZ  = ConstructAxisQuaternion([0, 0, 1], Yaw);
    const QY  = ConstructAxisQuaternion([0, 1, 0], Pitch);
    const QX  = ConstructAxisQuaternion([1, 0, 0], Roll);
    const Q   = MultiplyQuaternion(QZ, MultiplyQuaternion(QY, QX));
    const Raw = EmitBoxFacets(SX, SY, SZ, Family, FaceFamilies);
    return Raw.map((F) => ({
        Family:  F.Family,
        Corners: F.Corners.map((P) =>
        {
            const Local = [P[0], P[1], P[2] - 0.5 * SZ];
            const Rot   = RotateVector(Q, Local);
            return [Rot[0] + CX, Rot[1] + CY, Rot[2] + CZ];
        })
    }));
}

// 📝 Closed outward-wound tapered rectangular frustum (battered bunkers, pyramidal dragon's teeth, sloped tank armor).
function EmitFrustumAt(CX, CY, BaseZ, BottomSX, BottomSY, TopSX, TopSY, HeightZ, Family, TopFamily = Family)
{
    const BX = 0.5 * BottomSX, BY = 0.5 * BottomSY;
    const TX = 0.5 * TopSX,    TY = 0.5 * TopSY;
    const Z0 = BaseZ,          Z1 = BaseZ + HeightZ;
    const B00 = [CX - BX, CY - BY, Z0], B10 = [CX + BX, CY - BY, Z0], B11 = [CX + BX, CY + BY, Z0], B01 = [CX - BX, CY + BY, Z0];
    const T00 = [CX - TX, CY - TY, Z1], T10 = [CX + TX, CY - TY, Z1], T11 = [CX + TX, CY + TY, Z1], T01 = [CX - TX, CY + TY, Z1];
    const Facets = [];
    EmitQuad(Facets, T00, T10, T11, T01, TopFamily); // +Z top
    EmitQuad(Facets, B01, B11, B10, B00, Family);    // -Z bottom
    EmitQuad(Facets, B10, B11, T11, T10, Family);    // +X front
    EmitQuad(Facets, B01, B00, T00, T01, Family);    // -X back
    EmitQuad(Facets, B11, B01, T01, T11, Family);    // +Y left
    EmitQuad(Facets, B00, B10, T10, T00, Family);    // -Y right
    return Facets;
}

// 📝 Closed horizontal cylinder / barrel oriented along +X from X0 to X1 around [X, CY, CZ].
function EmitHorizontalCylinderX(X0, X1, CY, CZ, Radius, Sides, Family, MuzzleFamily = Family)
{
    const Facets = [];
    for (let I = 0; I < Sides; ++I)
    {
        const A0 = (2.0 * Math.PI * I) / Sides;
        const A1 = (2.0 * Math.PI * (I + 1)) / Sides;
        const Y0 = CY + Radius * Math.cos(A0), Z0 = CZ + Radius * Math.sin(A0);
        const Y1 = CY + Radius * Math.cos(A1), Z1 = CZ + Radius * Math.sin(A1);
        // Outward side quad around +X axis: Cross((X1-X0,0,0), (0,Y1-Y0,Z1-Z0)) = (0, -(Z1-Z0)*DX, (Y1-Y0)*DX)
        // At A=0 (Y=CY+R, Z=CZ) -> A1>0 (Z1>CZ): outward normal is +Y, so Cross((DX,0,0),(0,dY,dZ)) = (0,-DX*dZ, DX*dY) -> wait:
        // Let's verify: at A=0..dA, Y≈CY+R, Z goes 0->+dZ, dY≈0. Cross([1,0,0], [0,0,1]) = [0,-1,0] (-Y, inward!).
        // Therefore outward winding is ([X0,Y0,Z0], [X0,Y1,Z1], [X1,Y1,Z1], [X1,Y0,Z0])? Check:
        // Cross([0,0,1], [1,0,0]) = [0, +1, 0] (+Y, outward!!).
        EmitQuad(Facets, [X0, Y0, Z0], [X0, Y1, Z1], [X1, Y1, Z1], [X1, Y0, Z0], Family);
        // +X cap (normal +X): Cross([0, Y1-CY, Z1-CZ], [0, Y0-CY, Z0-CZ])? Wait:
        // [0, R, 0] x [0, R*cos(dA), R*sin(dA)] = [+R^2*sin(dA), 0, 0] (+X outward!!).
        Facets.push({ Corners: [[X1, CY, CZ], [X1, Y0, Z0], [X1, Y1, Z1]], Family: MuzzleFamily });
        // -X cap (normal -X):
        Facets.push({ Corners: [[X0, CY, CZ], [X0, Y1, Z1], [X0, Y0, Z0]], Family });
    }
    return Facets;
}

//------------------------------------------------------------------------------------------------------------------------
//                      FORTIFICATIONS, BARRICADES & THE ATLANTIC FORTRESS WALL
//------------------------------------------------------------------------------------------------------------------------

// 📝 Higgins LCVP / Tank Landing Craft at the Beachhead with lowered bow ramp facing +X.
export function EmitLandingCraftFacets()
{
    const Out = [];
    // Hull floor & stern engine house
    Out.push(...EmitFrustumAt(-2.0, 0.0, 0.0, 14.5, 5.4, 15.2, 5.8, 0.65, CourseMaterial.SteelDark, CourseMaterial.ApronConcrete));
    // Port & Starboard armored gunwales (disjoint by 16 mm)
    Out.push(...EmitBoxAt(-2.2,  2.65, 0.666, 14.2, 0.42, 1.85, CourseMaterial.SteelDark));
    Out.push(...EmitBoxAt(-2.2, -2.65, 0.666, 14.2, 0.42, 1.85, CourseMaterial.SteelDark));
    // Stern armored coxswain bulkhead & twin shields
    Out.push(...EmitBoxAt(-9.1,  0.0,  0.666, 2.4,  4.80, 2.45, CourseMaterial.SteelDark));
    Out.push(...EmitBoxAt(-8.6,  0.0,  3.132, 1.4,  2.20, 0.85, CourseMaterial.WallViridian));
    // Lowered bow assault ramp sloping gently onto the beach
    Out.push(...EmitRotatedBox(6.8, 0.0, 0.28, 3.8, 4.7, 0.24, 0.0, 0.11, 0.0, CourseMaterial.SteelDark));
    return Out;
}

// 📝 Reinforced Normandy Concrete Pillbox Bunker with overhanging blast roof & armored firing slit.
export function EmitPillboxBunkerFacets()
{
    const Out = [];
    // Lower battered concrete base up to firing-slit sill
    Out.push(...EmitFrustumAt(0.0, 0.0, 0.0, 7.6, 7.6, 6.9, 6.9, 1.65, CourseMaterial.TunnelConcrete));
    // Recessed dark armored firing-slit core (X,Y in [-2.25..+2.25])
    Out.push(...EmitBoxAt(0.0, 0.0, 1.668, 4.5, 4.5, 0.56, CourseMaterial.SteelDark));
    // 4 disjoint corner blast pillars flanking the firing embrasures (X,Y in [±2.32..±3.22])
    for (const [SX, SY] of [[-2.77, -2.77], [2.77, -2.77], [-2.77, 2.77], [2.77, 2.77]])
        Out.push(...EmitBoxAt(SX, SY, 1.668, 0.90, 0.90, 0.56, CourseMaterial.BarrierConcrete));
    // Heavy overhanging concrete blast cap roof
    Out.push(...EmitFrustumAt(0.0, 0.0, 2.246, 7.8, 7.8, 6.8, 6.8, 1.15, CourseMaterial.BarrierConcrete));
    // Rooftop armored turret ring plinth
    Out.push(...EmitFrustumAt(0.0, 0.0, 3.414, 2.6, 2.6, 2.2, 2.2, 0.36, CourseMaterial.SteelDark));
    return Out;
}

// 📝 Two-story Heavy Coastal Casemate Bunker housing coastal cannons, MG nests & blast deflectors.
export function EmitCasemateBunkerFacets()
{
    const Out = [];
    // Massive sloped concrete foundation casemate
    Out.push(...EmitFrustumAt(0.0, 0.0, 0.0, 13.6, 10.6, 12.2, 9.4, 2.60, CourseMaterial.TunnelConcrete));
    // Recessed dark artillery embrasure core (X in [-2.8..+2.8], Y in [-2.5..+2.5])
    Out.push(...EmitBoxAt(0.0, 0.0, 2.618, 5.6, 5.0, 1.34, CourseMaterial.SteelDark));
    // 4 disjoint armored side cheeks framing the recessed cannon embrasure
    Out.push(...EmitFrustumAt(-4.45, -3.55, 2.618, 2.9, 1.8, 2.6, 1.6, 1.34, CourseMaterial.BarrierConcrete));
    Out.push(...EmitFrustumAt(-4.45,  3.55, 2.618, 2.9, 1.8, 2.6, 1.6, 1.34, CourseMaterial.BarrierConcrete));
    Out.push(...EmitFrustumAt( 4.45, -3.55, 2.618, 2.9, 1.8, 2.6, 1.6, 1.34, CourseMaterial.BarrierConcrete));
    Out.push(...EmitFrustumAt( 4.45,  3.55, 2.618, 2.9, 1.8, 2.6, 1.6, 1.34, CourseMaterial.BarrierConcrete));
    // Massive 2.05 m-thick reinforced bomb-proof concrete brow slab (Regelbau style!)
    Out.push(...EmitFrustumAt(0.3, 0.0, 3.976, 14.4, 11.2, 12.6, 9.8, 2.05, CourseMaterial.BarrierConcrete));
    // Upper rangefinder observation cupola & AA / MG deck
    Out.push(...EmitFrustumAt(-1.2, 0.0, 6.044, 5.2, 4.8, 4.4, 4.2, 1.45, CourseMaterial.TunnelConcrete));
    Out.push(...EmitBoxAt(-1.2, 0.0, 7.512, 4.7, 4.5, 0.35, CourseMaterial.SteelDark));
    return Out;
}

// 📝 Reinforced Concrete Tactical Cover Wall (players can duck behind these between tank advances!).
export function EmitCoverWallFacets()
{
    const Out = [];
    Out.push(...EmitFrustumAt(0.0, 0.0, 0.0, 2.1, 8.2, 1.35, 7.8, 2.55, CourseMaterial.BarrierConcrete));
    // Disjoint steel armor face plate on West (-X) glacis & yellow tactical hazard stripe on crown
    Out.push(...EmitBoxAt(-1.16, 0.0, 0.35, 0.14, 7.2, 1.25, CourseMaterial.SteelDark));
    Out.push(...EmitBoxAt( 0.0,  0.0, 2.568, 1.42, 7.6, 0.22, CourseMaterial.PaintYellow));
    return Out;
}

// 📝 Stacked Sandbag & Steel-Beam Field Barricade.
export function EmitSandbagBarricadeFacets()
{
    const Out = [];
    // 3 stepped tiers of earth/khaki sandbag courses + disjoint flanking steel bracing posts
    Out.push(...EmitFrustumAt(0.0, 0.0, 0.000, 1.55, 4.4, 1.38, 4.2, 0.52, CourseMaterial.Gravel));
    Out.push(...EmitFrustumAt(0.0, 0.0, 0.538, 1.34, 4.0, 1.16, 3.8, 0.48, CourseMaterial.Stone));
    Out.push(...EmitFrustumAt(0.0, 0.0, 1.034, 1.12, 3.6, 0.92, 3.4, 0.44, CourseMaterial.Gravel));
    Out.push(...EmitBoxAt(0.0, -2.38, 0.0, 0.22, 0.22, 1.62, CourseMaterial.SteelDark));
    Out.push(...EmitBoxAt(0.0,  2.38, 0.0, 0.22, 0.22, 1.62, CourseMaterial.SteelDark));
    return Out;
}

// 📝 Iconic D-Day Czech Hedgehog (central riveted gusset cube + 6 radiating steel girder legs, 100% disjoint 2-manifold).
export function EmitCzechHedgehogFacets()
{
    const Out = [];
    // Central riveted steel gusset hub at Z = 0.92 m
    Out.push(...EmitBoxAt(0.0, 0.0, 0.72, 0.40, 0.40, 0.40, CourseMaterial.VehicleTrim));
    // 3 tripod ground legs + 3 upper anti-tank horn spikes radiating cleanly outside the gusset hub
    const Axes = [
        [ 0.58,  0.58,  0.57,  0.62, -0.52, 0.78],
        [-0.58,  0.58,  0.57, -0.62, -0.52, 2.35],
        [ 0.00, -0.81,  0.58,  0.72,  0.00, 0.00]
    ];
    for (const [DX, DY, DZ, RX, PY, YZ] of Axes)
    {
        Out.push(...EmitRotatedBox( DX * 0.78,  DY * 0.78, 0.92 + DZ * 0.78, 0.18, 0.18, 0.98, RX, PY, YZ, CourseMaterial.SteelDark));
        Out.push(...EmitRotatedBox(-DX * 0.78, -DY * 0.78, 0.92 - DZ * 0.78, 0.18, 0.18, 0.98, RX, PY, YZ, CourseMaterial.SteelDark));
    }
    return Out;
}

// 📝 Concrete Anti-Tank Dragon's Teeth cluster (4 staggered concrete pyramids on a shared sunken plinth).
export function EmitDragonsTeethFacets()
{
    const Out = [];
    Out.push(...EmitBoxAt(0.0, 0.0, 0.0, 2.8, 5.4, 0.22, CourseMaterial.ApronConcrete));
    const Offsets = [[-0.55, -1.85, 1.38], [0.55, -0.62, 1.56], [-0.55, 0.62, 1.52], [0.55, 1.85, 1.36]];
    for (const [OX, OY, H] of Offsets)
        Out.push(...EmitFrustumAt(OX, OY, 0.236, 1.25, 1.25, 0.26, 0.26, H, CourseMaterial.BarrierConcrete));
    return Out;
}

// 📝 14 m-tall Reinforced-Concrete Atlantic Fortress Wall Segment (12.8 m span along Y, sloped West glacis).
export function EmitFortressWallSegmentFacets()
{
    const Out = [];
    // Lower battered seawall / anti-tank skirt (0 .. 6.2 m)
    Out.push(...EmitFrustumAt(0.0, 0.0, 0.0, 6.8, 12.76, 5.2, 12.76, 6.20, CourseMaterial.TunnelConcrete));
    // Upper vertical fortress curtain wall (6.22 .. 12.1 m)
    Out.push(...EmitFrustumAt(0.35, 0.0, 6.218, 5.1, 12.76, 4.6, 12.76, 5.85, CourseMaterial.BarrierConcrete));
    // Overhanging anti-scaling concrete parapet & firing gallery brow (12.08 .. 13.8 m)
    Out.push(...EmitFrustumAt(0.10, 0.0, 12.084, 5.9, 12.76, 5.5, 12.76, 1.45, CourseMaterial.TunnelConcrete));
    // Crenellated merlons & steel armor belt along the West face
    Out.push(...EmitBoxAt(-2.62, 0.0, 5.65, 0.28, 12.60, 0.52, CourseMaterial.SteelDark));
    for (const MY of [-4.4, 0.0, 4.4])
        Out.push(...EmitBoxAt(-2.05, MY, 13.55, 1.35, 2.65, 0.95, CourseMaterial.BarrierConcrete));
    return Out;
}

// 📝 Central Atlantic Wall Breach Gatehouse Archway spanning Y = [-12.5 .. +12.5] above Z = 6.5 m with Victory Beacons!
export function EmitWallBreachArchFacets()
{
    const Out = [];
    // Left & Right armored gate jamb piers (outside the 22 m clear roadway!)
    Out.push(...EmitFrustumAt(0.0, -12.6, 0.0, 6.6, 3.2, 5.6, 2.8, 14.2, CourseMaterial.BarrierConcrete));
    Out.push(...EmitFrustumAt(0.0,  12.6, 0.0, 6.6, 3.2, 5.6, 2.8, 14.2, CourseMaterial.BarrierConcrete));
    // Overhead reinforced gatehouse lintel spanning clear Z = 6.6 m .. 13.8 m across Y = -10.8 .. +10.8 m
    Out.push(...EmitBoxAt(0.0, 0.0, 6.60, 5.6, 21.8, 4.80, CourseMaterial.TunnelConcrete));
    Out.push(...EmitBoxAt(0.0, 0.0, 11.42, 6.2, 22.0, 2.20, CourseMaterial.SteelDark));
    // Glowing emerald & amber breach guidance beacons on West face so the goal shines through battle smoke!
    Out.push(...EmitBoxAt(-2.92,  0.0,  7.40, 0.22, 14.0, 0.42, CourseMaterial.GateLamp));
    Out.push(...EmitBoxAt(-2.88, -9.8,  2.20, 0.24,  0.65, 3.60, CourseMaterial.GateLamp));
    Out.push(...EmitBoxAt(-2.88,  9.8,  2.20, 0.24,  0.65, 3.60, CourseMaterial.GateLamp));
    Out.push(...EmitBoxAt(-3.16,  0.0, 12.10, 0.22,  8.50, 0.55, CourseMaterial.BannerAmber));
    return Out;
}

// 📝 19 m-tall Fortress Watchtower & Searchlight Bastion flanking the Atlantic Wall Breach.
export function EmitWallWatchtowerFacets()
{
    const Out = [];
    Out.push(...EmitFrustumAt(0.0, 0.0, 0.0, 7.8, 7.8, 6.4, 6.4, 13.8, CourseMaterial.TunnelConcrete));
    // Observation gallery slit + overhanging command crown
    Out.push(...EmitBoxAt(0.0, 0.0, 13.818, 5.4, 5.4, 1.35, CourseMaterial.SteelDark));
    Out.push(...EmitFrustumAt(0.0, 0.0, 15.184, 7.2, 7.2, 6.2, 6.2, 2.40, CourseMaterial.BarrierConcrete));
    // Searchlight & radar cupola on roof
    Out.push(...EmitBoxAt(0.0, 0.0, 17.600, 2.8, 2.8, 1.15, CourseMaterial.SteelDark));
    Out.push(...EmitBoxAt(-1.48, 0.0, 17.85, 0.24, 1.35, 0.68, CourseMaterial.Headlight));
    return Out;
}

//------------------------------------------------------------------------------------------------------------------------
//                ALLIED ASSAULT TANKS, ENEMY MG TURRETS, FORTRESS CANNONS, AIRCRAFT, BOMBS & MINES
//------------------------------------------------------------------------------------------------------------------------

// 📝 Allied Medium Assault Tank Hull (6.2 m × 3.2 m × 1.55 m): tracks, sponsons, sloped glacis, white recognition stripe.
export function EmitTankHullFacets()
{
    const Out = [];
    // Left & Right armored caterpillar track assemblies (disjoint from center hull by 20 mm)
    for (const Side of [-1.22, 1.22])
    {
        Out.push(...EmitFrustumAt(0.0, Side, 0.0, 5.85, 0.68, 6.18, 0.68, 0.96, CourseMaterial.SteelDark));
        // Armored olive side sand-shield skirt above track
        Out.push(...EmitBoxAt(0.0, Side, 0.976, 6.05, 0.72, 0.34, CourseMaterial.WallViridian));
    }
    // Lower armored hull tub + sloped front glacis + engine deck
    Out.push(...EmitFrustumAt(-0.05, 0.0, 0.28, 5.60, 1.72, 6.10, 1.72, 1.08, CourseMaterial.WallViridian));
    // White Allied recognition invasion stripe on front glacis & hull sides + twin armored headlights
    Out.push(...EmitBoxAt( 1.55, 0.0, 1.376, 0.95, 1.58, 0.08, CourseMaterial.PaintWhite));
    Out.push(...EmitBoxAt(-1.65, 0.0, 1.376, 1.85, 1.48, 0.16, CourseMaterial.SteelDark));
    Out.push(...EmitBoxAt( 2.92,  0.64, 1.12, 0.24, 0.24, 0.22, CourseMaterial.Headlight));
    Out.push(...EmitBoxAt( 2.92, -0.64, 1.12, 0.24, 0.24, 0.22, CourseMaterial.Headlight));
    return Out;
}

// 📝 Rotating Tank Turret + 75mm Long Main Gun Barrel + Commander Cupola (local origin at turret ring Z = 0).
export function EmitTankTurretFacets()
{
    const Out = [];
    // Cast-armor sloped turret bustle & cheeks
    Out.push(...EmitFrustumAt(-0.15, 0.0, 0.0, 2.55, 2.05, 2.05, 1.68, 0.86, CourseMaterial.WallViridian));
    // Commander hatch cupola & white recognition band
    Out.push(...EmitBoxAt(-0.45, 0.36, 0.876, 0.68, 0.68, 0.24, CourseMaterial.SteelDark));
    Out.push(...EmitBoxAt(-0.95, 0.0,  0.28,  0.55, 2.08, 0.32, CourseMaterial.PaintWhite));
    // Heavy gun mantlet + 3.4 m long 75mm main barrel along +X
    Out.push(...EmitBoxAt( 1.22, 0.0,  0.24,  0.46, 0.92, 0.48, CourseMaterial.SteelDark));
    Out.push(...EmitHorizontalCylinderX(1.48, 4.35, 0.0, 0.48, 0.11, 10, CourseMaterial.SteelDark, CourseMaterial.VehicleTrim));
    return Out;
}

// 📝 Destroyed / Exploded Tank Wreck Hull (charred steel, popped ember turret ring, collapsed track skirt).
export function EmitTankWreckFacets()
{
    const Out = [];
    for (const Side of [-1.22, 1.22])
        Out.push(...EmitFrustumAt(0.0, Side, 0.0, 5.75, 0.68, 5.95, 0.68, 0.82, CourseMaterial.SteelDark));
    Out.push(...EmitFrustumAt(-0.08, 0.0, 0.18, 5.55, 1.72, 5.85, 1.72, 0.98, CourseMaterial.SteelDark));
    // Glowing orange/crimson ammo-rack ember core inside open turret ring + tilted blown turret hulk
    Out.push(...EmitBoxAt(0.10, 0.0, 1.18, 1.45, 1.35, 0.22, CourseMaterial.TrafficOrange));
    Out.push(...EmitRotatedBox(-0.55, 0.42, 1.58, 2.05, 1.65, 0.68, 0.34, -0.26, 0.48, CourseMaterial.SteelDark));
    return Out;
}

// 📝 Machine-Gun Turret Armored Cupola Base + Rotating Twin-Barrel Heavy MG Head.
export function EmitMgTurretBaseFacets()
{
    const Out = [];
    Out.push(...EmitFrustumAt(0.0, 0.0, 0.0, 2.20, 2.20, 1.65, 1.65, 0.72, CourseMaterial.SteelDark));
    return Out;
}

export function EmitMgTurretHeadFacets()
{
    const Out = [];
    // Armored gunhouse + angled frontal ballistic gun-shield
    Out.push(...EmitFrustumAt(-0.12, 0.0, 0.0, 1.45, 1.38, 1.18, 1.12, 0.78, CourseMaterial.SteelDark));
    Out.push(...EmitRotatedBox(0.68, 0.0, 0.44, 0.16, 1.46, 0.86, 0.0, -0.28, 0.0, CourseMaterial.BarrierConcrete));
    // Twin heavy machine-gun barrels + glowing hot muzzle flash suppressors
    Out.push(...EmitHorizontalCylinderX(0.78, 2.38, -0.26, 0.42, 0.065, 8, CourseMaterial.SteelDark, CourseMaterial.TrafficOrange));
    Out.push(...EmitHorizontalCylinderX(0.78, 2.38,  0.26, 0.42, 0.065, 8, CourseMaterial.SteelDark, CourseMaterial.TrafficOrange));
    // Twin side ammunition drum magazines
    Out.push(...EmitBoxAt(0.18, -0.78, 0.24, 0.52, 0.28, 0.42, CourseMaterial.WallViridian));
    Out.push(...EmitBoxAt(0.18,  0.78, 0.24, 0.52, 0.28, 0.42, CourseMaterial.WallViridian));
    return Out;
}

// 📝 Heavy Fortress Coastal Artillery Cannon Pedestal + Rotating 155mm Armored Turret & Long Barrel.
export function EmitCannonBaseFacets()
{
    const Out = [];
    Out.push(...EmitFrustumAt(0.0, 0.0, 0.0, 3.80, 3.80, 2.95, 2.95, 1.05, CourseMaterial.TunnelConcrete));
    Out.push(...EmitBoxAt(0.0, 0.0, 1.066, 2.75, 2.75, 0.26, CourseMaterial.SteelDark));
    return Out;
}

export function EmitCannonBarrelFacets()
{
    const Out = [];
    // Armored coastal turret casemate hood
    Out.push(...EmitFrustumAt(-0.35, 0.0, 0.0, 3.30, 2.85, 2.55, 2.25, 1.42, CourseMaterial.SteelDark));
    // Hydraulic recoil cradle + massive 5.8 m 155mm artillery barrel + heavy T-muzzle brake
    Out.push(...EmitBoxAt(1.55, 0.0, 0.48, 1.40, 0.92, 0.64, CourseMaterial.VehicleTrim));
    Out.push(...EmitHorizontalCylinderX(2.28, 6.85, 0.0, 0.80, 0.19, 12, CourseMaterial.SteelDark, CourseMaterial.TrafficOrange));
    Out.push(...EmitBoxAt(7.12, 0.0, 0.54, 0.48, 0.78, 0.52, CourseMaterial.SteelDark));
    return Out;
}

// 📝 WWII Dive-Bomber / Fighter-Bomber Aircraft (9.8 m wingspan, gull-wing pylons, glazed canopy, crimson/amber markings).
export function EmitAircraftBodyFacets()
{
    const Out = [];
    // Streamlined fuselage nose, main body & tapering tailcone
    Out.push(...EmitFrustumAt( 0.35, 0.0, -0.42, 5.20, 1.18, 4.80, 1.04, 1.02, CourseMaterial.SteelDark));
    Out.push(...EmitBoxAt( 3.18, 0.0, -0.28, 0.82, 0.86, 0.78, CourseMaterial.TrafficRed)); // Crimson spinner/cowling ring
    // Glazed greenhouse pilot canopy
    Out.push(...EmitFrustumAt( 0.55, 0.0, 0.616, 2.15, 0.84, 1.55, 0.62, 0.54, CourseMaterial.VehicleGlass));
    // Port & Starboard main attack wings + wingtip navigation beacons + underwing bomb racks
    Out.push(...EmitBoxAt( 0.42, -2.75, -0.14, 2.15, 4.25, 0.22, CourseMaterial.SteelDark));
    Out.push(...EmitBoxAt( 0.42,  2.75, -0.14, 2.15, 4.25, 0.22, CourseMaterial.SteelDark));
    Out.push(...EmitBoxAt( 0.42, -4.96, -0.12, 0.65, 0.14, 0.18, CourseMaterial.TrafficRed));
    Out.push(...EmitBoxAt( 0.42,  4.96, -0.12, 0.65, 0.14, 0.18, CourseMaterial.GateLamp));
    Out.push(...EmitBoxAt( 0.50, -1.85, -0.52, 0.95, 0.24, 0.34, CourseMaterial.PaintYellow));
    Out.push(...EmitBoxAt( 0.50,  1.85, -0.52, 0.95, 0.24, 0.34, CourseMaterial.PaintYellow));
    // Rear tail boom, horizontal stabilizer & tall vertical rudder fin
    Out.push(...EmitBoxAt(-3.45,  0.0,  -0.16, 2.35, 0.58, 0.62, CourseMaterial.SteelDark));
    Out.push(...EmitBoxAt(-4.35,  0.0,   0.04, 0.98, 3.20, 0.14, CourseMaterial.SteelDark));
    Out.push(...EmitFrustumAt(-4.28, 0.0, 0.48, 1.18, 0.16, 0.64, 0.12, 1.48, CourseMaterial.WallCrimson));
    return Out;
}

export function EmitAircraftPropellerFacets()
{
    const Out = [];
    // Spinner hub + 4 disjoint propeller blades centered at origin (rotates around local X axis!)
    Out.push(...EmitBoxAt(0.0,  0.0,  -0.14, 0.24, 0.28, 0.28, CourseMaterial.PaintYellow));
    Out.push(...EmitBoxAt(0.18, 0.0,   0.18, 0.05, 0.22, 1.18, CourseMaterial.SteelDark));
    Out.push(...EmitBoxAt(0.18, 0.0,  -1.36, 0.05, 0.22, 1.18, CourseMaterial.SteelDark));
    Out.push(...EmitBoxAt(0.25, 0.77, -0.11, 0.05, 1.18, 0.22, CourseMaterial.PaintYellow));
    Out.push(...EmitBoxAt(0.25,-0.77, -0.11, 0.05, 1.18, 0.22, CourseMaterial.PaintYellow));
    return Out;
}

// 📝 Finned 250 kg Aerial Bomb (oriented with nose along +X so velocity quaternion pitches nose straight down in flight).
export function EmitAerialBombFacets()
{
    const Out = [];
    Out.push(...EmitHorizontalCylinderX(-0.65, 0.55, 0.0, 0.0, 0.24, 10, CourseMaterial.SteelDark, CourseMaterial.PaintYellow));
    // Nose fuse tip
    Out.push(...EmitBoxAt(0.66, 0.0, -0.10, 0.18, 0.20, 0.20, CourseMaterial.TrafficRed));
    // Disjoint 4-fin tail stabilizer cruciform
    Out.push(...EmitBoxAt(-0.82, 0.0,  0.05, 0.28, 0.06, 0.34, CourseMaterial.SteelDark));
    Out.push(...EmitBoxAt(-0.82, 0.0, -0.39, 0.28, 0.06, 0.34, CourseMaterial.SteelDark));
    Out.push(...EmitBoxAt(-0.82, 0.22,-0.03, 0.28, 0.34, 0.06, CourseMaterial.SteelDark));
    Out.push(...EmitBoxAt(-0.82,-0.22,-0.03, 0.28, 0.34, 0.06, CourseMaterial.SteelDark));
    return Out;
}

// 📝 Glowing 155mm Artillery Shell & Rapid Machine-Gun Tracer Streak.
export function EmitArtilleryShellFacets()
{
    const Out = [];
    Out.push(...EmitHorizontalCylinderX(-0.55, 0.55, 0.0, 0.0, 0.16, 8, CourseMaterial.TrafficOrange, CourseMaterial.Headlight));
    return Out;
}

export function EmitTracerBoltFacets()
{
    return EmitBoxAt(0.0, 0.0, -0.045, 1.85, 0.09, 0.09, CourseMaterial.Headlight);
}

// 📝 Anti-Tank Teller-Mine: armored steel disc + yellow stencil band + glowing crimson armed-fuse cap.
export function EmitLandmineFacets()
{
    const Out = [];
    Out.push(...EmitPrismFacets(0.58, 0.14, 14, CourseMaterial.SteelDark));
    for (const F of EmitPrismFacets(0.42, 0.06, 12, CourseMaterial.PaintYellow))
        Out.push({ Family: F.Family, Corners: F.Corners.map((P) => [P[0], P[1], P[2] + 0.154]) });
    for (const F of EmitPrismFacets(0.18, 0.07, 10, CourseMaterial.TrafficRed))
        Out.push({ Family: F.Family, Corners: F.Corners.map((P) => [P[0], P[1], P[2] + 0.228]) });
    return Out;
}

// 📝 Minefield Hazard Warning Stake (red/white post flanking mine belts).
export function EmitMinePostFacets()
{
    const Out = [];
    Out.push(...EmitBoxAt(0.0, 0.0, 0.0,  0.14, 0.14, 1.25, CourseMaterial.SteelDark));
    Out.push(...EmitBoxAt(0.0, 0.0, 1.27, 0.12, 0.68, 0.46, CourseMaterial.TrafficRed));
    Out.push(...EmitBoxAt(-0.08, 0.0, 1.38, 0.03, 0.48, 0.24, CourseMaterial.PaintWhite));
    return Out;
}

//------------------------------------------------------------------------------------------------------------------------
//                                        DDAY SHAPE REGISTRY
//------------------------------------------------------------------------------------------------------------------------

export const DDayShapes = Object.freeze({
    // Static SDF + GI + Collider Fortifications (compact voxel counts for fast <0.4s startup)
    DDayLandingCraft:        { Emit: EmitLandingCraftFacets,        Voxel: 0.28, Cards: 5.0 },
    DDayPillboxBunker:       { Emit: EmitPillboxBunkerFacets,       Voxel: 0.22, Cards: 6.0 },
    DDayCasemateBunker:      { Emit: EmitCasemateBunkerFacets,      Voxel: 0.28, Cards: 5.0 },
    DDayCoverWall:           { Emit: EmitCoverWallFacets,           Voxel: 0.16, Cards: 7.0 },
    DDaySandbagBarricade:    { Emit: EmitSandbagBarricadeFacets,    Voxel: 0.14, Cards: 8.0 },
    DDayCzechHedgehog:       { Emit: EmitCzechHedgehogFacets,       Voxel: 0.12, Cards: 8.0 },
    DDayDragonsTeeth:        { Emit: EmitDragonsTeethFacets,        Voxel: 0.16, Cards: 7.0 },
    DDayFortressWallSegment: { Emit: EmitFortressWallSegmentFacets, Voxel: 0.34, Cards: 4.0 },
    DDayWallBreachArch:      { Emit: EmitWallBreachArchFacets,      Voxel: 0.36, Cards: 4.0 },
    DDayWallWatchtower:      { Emit: EmitWallWatchtowerFacets,      Voxel: 0.34, Cards: 4.0 },
    DDayTankHull:            { Emit: EmitTankHullFacets,            Voxel: 0.16, Cards: 8.0 },

    // Fast Dynamic / Raster Combat Actors & Ordnance
    DDayTankTurret:          { Emit: EmitTankTurretFacets,          Voxel: 0.12, Cards: 0.0, Distance: false },
    DDayTankWreckHull:       { Emit: EmitTankWreckFacets,           Voxel: 0.16, Cards: 0.0, Distance: false },
    DDayMgTurretBase:        { Emit: EmitMgTurretBaseFacets,        Voxel: 0.12, Cards: 0.0, Distance: false },
    DDayMgTurretHead:        { Emit: EmitMgTurretHeadFacets,        Voxel: 0.10, Cards: 0.0, Distance: false },
    DDayCannonBase:          { Emit: EmitCannonBaseFacets,          Voxel: 0.14, Cards: 0.0, Distance: false },
    DDayCannonBarrel:        { Emit: EmitCannonBarrelFacets,        Voxel: 0.14, Cards: 0.0, Distance: false },
    DDayAircraftBody:        { Emit: EmitAircraftBodyFacets,        Voxel: 0.16, Cards: 0.0, Distance: false },
    DDayAircraftPropeller:   { Emit: EmitAircraftPropellerFacets,   Voxel: 0.10, Cards: 0.0, Distance: false },
    DDayAerialBomb:          { Emit: EmitAerialBombFacets,          Voxel: 0.08, Cards: 0.0, Distance: false },
    DDayArtilleryShell:      { Emit: EmitArtilleryShellFacets,      Voxel: 0.08, Cards: 0.0, Distance: false },
    DDayTracerBolt:          { Emit: EmitTracerBoltFacets,          Voxel: 0.06, Cards: 0.0, Distance: false },
    DDayLandmine:            { Emit: EmitLandmineFacets,            Voxel: 0.06, Cards: 0.0, Distance: false },
    DDayMinePost:            { Emit: EmitMinePostFacets,            Voxel: 0.08, Cards: 0.0, Distance: false }
});

//------------------------------------------------------------------------------------------------------------------------
//                      BATTLEFIELD COORDINATES, TURRET / CANNON / MINE / TANK TABLES
//------------------------------------------------------------------------------------------------------------------------

export const DDayWallX       = 318.0;
export const DDayBreachGoalX = 310.0;
export const DDayBreachHalfY = 12.0;

export const DDayBounds = Object.freeze({
    Minimum: Object.freeze([-205.0, -136.0]),
    Maximum: Object.freeze([ 362.0,  136.0])
});

export const DDaySpawn = Object.freeze({
    Position: Object.freeze([-154.0, 0.0, 0.63]),
    Yaw:      0.0
});

export const DDayZones = Object.freeze([
    { Name: "1 · Omaha Beachhead Ramp",    Position: [-154.0,   0.0], Yaw:  0.0, Note: "Start of the Militarized Wall Run" },
    { Name: "2 · Pillbox & Minefield",     Position: [ -84.0,  -5.0], Yaw:  0.0, Note: "Barricade slalom & Teller-mines" },
    { Name: "3 · Tank Escort & MG Alley",  Position: [  16.0, -11.0], Yaw:  0.0, Note: "Use advancing Allied tanks as cover" },
    { Name: "4 · Artillery & Bombing Run", Position: [ 128.0,   8.0], Yaw:  0.0, Note: "155mm coastal cannons & dive-bombers" },
    { Name: "5 · Kamikaze Crash Gauntlet", Position: [ 224.0,   0.0], Yaw:  0.0, Note: "Stricken aircraft & final mine belt" },
    { Name: "6 · Atlantic Wall Breach",    Position: [ 288.0,   0.0], Yaw:  0.0, Note: "Final sprint through the Wall gate" }
]);

export const DDayClearZones = Object.freeze([
    [-190.0, -18.0, 355.0,  18.0], // Central Assault Causeway
    [-180.0,  38.0, 310.0,  58.0], // North Sapper Flanking Track
    [-180.0, -58.0, 310.0, -38.0]  // South Sapper Flanking Track
]);

// 📝 8 Rotating Twin-Barrel Machine-Gun Turret Mounts (atop Pillboxes, Casemates & Wall Bastions).
export const DDayMgTurretSpecs = Object.freeze([
    { Id: "mg_01", Name: "North Pillbox MG-42",      Position: [ -48.0,  27.0, 3.78], BaseYaw: Math.PI, Range: 105.0, Phase: 0.0 },
    { Id: "mg_02", Name: "South Pillbox MG-42",      Position: [ -36.0, -27.0, 3.78], BaseYaw: Math.PI, Range: 105.0, Phase: 0.7 },
    { Id: "mg_03", Name: "North Casemate Cupola MG", Position: [  46.0,  31.0, 7.86], BaseYaw: Math.PI, Range: 118.0, Phase: 1.4 },
    { Id: "mg_04", Name: "South Casemate Cupola MG", Position: [  58.0, -31.0, 7.86], BaseYaw: Math.PI, Range: 118.0, Phase: 2.1 },
    { Id: "mg_05", Name: "Midfield Ridge MG Nest",   Position: [ 132.0,  29.0, 3.78], BaseYaw: Math.PI, Range: 115.0, Phase: 0.4 },
    { Id: "mg_06", Name: "South Flank MG Nest",      Position: [ 148.0, -29.0, 3.78], BaseYaw: Math.PI, Range: 115.0, Phase: 1.2 },
    { Id: "mg_07", Name: "Wall Approach Bastion MG", Position: [ 246.0,  25.0, 3.78], BaseYaw: Math.PI, Range: 115.0, Phase: 1.9 },
    { Id: "mg_08", Name: "Atlantic Wall Gate MG",    Position: [ 313.5, -16.5, 13.55],BaseYaw: Math.PI, Range: 130.0, Phase: 2.6 }
]);

// 📝 6 Heavy Rotating 155mm Fortress & Wall Artillery Cannons (target both advancing Tanks & exposed Cars!).
export const DDayCannonSpecs = Object.freeze([
    { Id: "can_01", Name: "Casemate Alpha 155mm",     Position: [  46.0, -31.0, 6.05], BaseYaw: Math.PI, Range: 195.0, Cooldown: 3.8, Phase: 0.5 },
    { Id: "can_02", Name: "Casemate Bravo 155mm",     Position: [ 118.0,  31.0, 7.86], BaseYaw: Math.PI, Range: 205.0, Cooldown: 4.1, Phase: 1.8 },
    { Id: "can_03", Name: "Artillery Ridge North",    Position: [ 198.0,  33.0, 6.05], BaseYaw: Math.PI, Range: 215.0, Cooldown: 3.6, Phase: 2.9 },
    { Id: "can_04", Name: "Artillery Ridge South",    Position: [ 204.0, -33.0, 6.05], BaseYaw: Math.PI, Range: 215.0, Cooldown: 3.9, Phase: 0.9 },
    { Id: "can_05", Name: "Atlantic Wall North Gun",  Position: [ 316.0,  34.0, 13.55],BaseYaw: Math.PI, Range: 235.0, Cooldown: 3.5, Phase: 2.2 },
    { Id: "can_06", Name: "Atlantic Wall South Gun",  Position: [ 316.0, -34.0, 13.55],BaseYaw: Math.PI, Range: 235.0, Cooldown: 3.7, Phase: 3.1 }
]);

// 📝 6 Advancing Allied Assault Tank Slots (steadily rolling eastward toward the Atlantic Wall; provide real cover & explode under fire!).
export const DDayTankSpecs = Object.freeze([
    { Id: "tank_01", Name: "Iron Spear 1 (Center-Right)", LaneY:  -7.2, StartX: -124.0, Speed: 8.2, MaxArmor: 100 },
    { Id: "tank_02", Name: "Iron Spear 2 (Center-Left)",  LaneY:   7.4, StartX:  -46.0, Speed: 7.8, MaxArmor: 100 },
    { Id: "tank_03", Name: "Vanguard 3 (Center Lead)",    LaneY:  -6.8, StartX:   48.0, Speed: 7.4, MaxArmor: 100 },
    { Id: "tank_04", Name: "Vanguard 4 (Wall Push)",      LaneY:   7.2, StartX:  138.0, Speed: 7.2, MaxArmor: 100 },
    { Id: "tank_05", Name: "North Sapper Tank 5",         LaneY:  43.5, StartX:  -64.0, Speed: 8.5, MaxArmor: 100 },
    { Id: "tank_06", Name: "South Sapper Tank 6",         LaneY: -43.5, StartX:   18.0, Speed: 8.4, MaxArmor: 100 }
]);

// 📝 30 Armed Anti-Tank Teller-Mines placed across 3 Minefield Belts (with navigable slalom gaps for fast racing!).
export const DDayMineSpecs = Object.freeze([
    // Belt Alpha: Beachhead Exit & Pillbox Approach (X = -102 .. -24 m)
    [-102.0,  -4.2], [ -92.0,   4.8], [ -80.0,  -9.5], [ -72.0,   1.6], [ -62.0,   8.8],
    [ -52.0,  -4.6], [ -42.0,   3.9], [ -30.0,  -8.4], [ -88.0,  46.5], [ -64.0, -49.5],
    // Belt Bravo: Casemate & Tank Kill-Zone (X = +6 .. +158 m)
    [   8.0,   3.6], [  24.0,  -3.4], [  40.0,  11.2], [  66.0,  -2.8], [  84.0,   4.2],
    [ 104.0, -10.4], [ 122.0,   2.2], [ 142.0,  -4.8], [  74.0,  49.5], [ 112.0, -46.0],
    // Belt Charlie: Final Artillery & Wall Approach Gauntlet (X = +176 .. +284 m)
    [ 178.0,   4.6], [ 194.0,  -3.8], [ 212.0,   9.4], [ 226.0,  -1.8], [ 242.0,   5.2],
    [ 256.0,  -5.6], [ 270.0,   3.4], [ 282.0,  -4.2], [ 218.0,  47.5], [ 236.0, -48.5]
].map(([X, Y], I) => Object.freeze({
    Id:       `mine_${String(I + 1).padStart(2, "0")}`,
    Name:     `Teller-Mine #${I + 1}`,
    Position: Object.freeze([X, Y, 0.0])
})));

//------------------------------------------------------------------------------------------------------------------------
//                          STATIC COVER BOXES FOR BALLISTIC LOS & AI NAVIGATION
//------------------------------------------------------------------------------------------------------------------------

// 📝 Analytical static ballistic cover boxes [MinX, MinY, MinZ, MaxX, MaxY, MaxZ] matching Bunkers & Tactical Cover Walls.
export const DDayStaticCoverBoxes = [];

export function ResolveDDayPlacements()
{
    const Placements = [];
    DDayStaticCoverBoxes.length = 0;

    const AddCoverBox = (CX, CY, SX, SY, SZ, Yaw = 0.0) =>
    {
        const Cos = Math.abs(Math.cos(Yaw)), Sin = Math.abs(Math.sin(Yaw));
        const HX = 0.5 * (SX * Cos + SY * Sin);
        const HY = 0.5 * (SX * Sin + SY * Cos);
        DDayStaticCoverBoxes.push([CX - HX, CY - HY, 0.0, CX + HX, CY + HY, SZ]);
    };

    const Place = (Shape, Name, X, Y, Z = 0.0, Yaw = 0.0, Collider = true) =>
    {
        Placements.push({ Shape, Name, Position: [X, Y, Z], Yaw, Collider });
    };

    // ① Beachhead Landing Craft Flotilla (3 armored Higgins / Tank Landing Crafts at West Surf)
    Place("DDayLandingCraft", "LCVP-01 Center Landing Craft", -170.0,   0.0, 0.0, 0.0, true);
    Place("DDayLandingCraft", "LCVP-02 North Landing Craft",  -170.0,  48.0, 0.0, 0.0, true);
    Place("DDayLandingCraft", "LCVP-03 South Landing Craft",  -170.0, -48.0, 0.0, 0.0, true);

    // ② Pillbox Bunkers & Heavy Two-Story Casemate Bunkers along the Militarized Corridor
    const Pillboxes = [
        [-48.0,  27.0, Math.PI,       "North Forward Pillbox"],
        [-36.0, -27.0, Math.PI,       "South Forward Pillbox"],
        [-58.0,  68.0, -Math.PI * 0.8,"NW Ridge Pillbox"],
        [-52.0, -68.0,  Math.PI * 0.8,"SW Ridge Pillbox"],
        [132.0,  29.0, Math.PI,       "Midfield North Pillbox"],
        [148.0, -29.0, Math.PI,       "Midfield South Pillbox"],
        [246.0,  25.0, Math.PI,       "Wall Approach North Pillbox"],
        [254.0, -25.0, Math.PI,       "Wall Approach South Pillbox"]
    ];
    for (const [X, Y, Yaw, Name] of Pillboxes)
    {
        Place("DDayPillboxBunker", Name, X, Y, 0.0, Yaw, true);
        AddCoverBox(X, Y, 7.4, 7.4, 3.6, Yaw);
    }

    const Casemates = [
        [ 46.0,  31.0, Math.PI, "Heavy Casemate North-Alpha"],
        [ 46.0, -31.0, Math.PI, "Heavy Casemate South-Alpha"],
        [118.0,  31.0, Math.PI, "Heavy Casemate North-Bravo"],
        [198.0,  33.0, Math.PI, "Artillery Casemate North"],
        [204.0, -33.0, Math.PI, "Artillery Casemate South"]
    ];
    for (const [X, Y, Yaw, Name] of Casemates)
    {
        Place("DDayCasemateBunker", Name, X, Y, 0.0, Yaw, true);
        AddCoverBox(X, Y, 13.2, 10.2, 6.2, Yaw);
    }

    // ③ Tactical Reinforced Concrete Cover Walls (angled shields along the corridor so players can take cover!)
    const CoverWalls = [
        [-114.0,  -9.5,  0.14], [-82.0,  10.2, -0.14], [-44.0, -10.5,  0.12],
        [  -6.0,  10.4, -0.12], [ 34.0, -11.6,  0.10], [ 78.0,  11.4, -0.10],
        [ 118.0, -11.2,  0.12], [162.0,  11.0, -0.12], [206.0, -11.2,  0.10],
        [ 248.0,  10.6, -0.14], [282.0,  -9.8,  0.15],
        [  28.0,  55.5,  0.00], [156.0,  55.5,  0.00], [ 96.0, -55.5,  0.00]
    ];
    CoverWalls.forEach(([X, Y, Yaw], Idx) =>
    {
        Place("DDayCoverWall", `Blast Cover Wall ${Idx + 1}`, X, Y, 0.0, Yaw, true);
        AddCoverBox(X, Y, 2.1, 8.2, 2.75, Yaw);
    });

    // ④ Sandbag Barricades (weaving chicanes & roadside blast positions)
    const Sandbags = [
        [-134.0,  11.5,  0.25], [-134.0, -11.5, -0.25],
        [ -96.0, -12.0,  0.18], [ -66.0,  12.4, -0.22],
        [ -18.0, -12.2,  0.16], [  16.0,  12.8, -0.18],
        [  62.0, -12.6,  0.20], [  98.0,  12.6, -0.20],
        [ 144.0,  12.4,  0.18], [ 182.0, -12.4, -0.18],
        [ 230.0,  12.0,  0.22], [ 268.0,  11.6, -0.22],
        [ -28.0,  41.5,  0.15], [ 104.0,  41.5, -0.15],
        [ -24.0, -41.5, -0.15], [ 134.0, -41.5,  0.15]
    ];
    Sandbags.forEach(([X, Y, Yaw], Idx) =>
    {
        Place("DDaySandbagBarricade", `Sandbag Barricade ${Idx + 1}`, X, Y, 0.0, Yaw, true);
        AddCoverBox(X, Y, 1.5, 4.8, 1.5, Yaw);
    });

    // ⑤ Iconic D-Day Czech Hedgehogs & Concrete Dragon's Teeth Funnels
    const Hedgehogs = [
        [-144.0,  15.5], [-142.0, -15.5], [-126.0,  22.0], [-124.0, -22.0],
        [-108.0,  16.5], [-106.0, -16.5], [ -74.0,  17.5], [ -72.0, -17.5],
        [ -26.0,  17.0], [ -24.0, -17.0], [  14.0, -17.5], [  54.0,  17.5],
        [  90.0, -17.5], [ 128.0,  17.5], [ 168.0, -17.5], [ 214.0,  17.5],
        [ 260.0, -16.5], [ 292.0,  15.5], [ 292.0, -15.5],
        [-138.0,  34.0], [-138.0, -34.0], [  64.0,  36.0], [ 172.0, -36.0]
    ];
    Hedgehogs.forEach(([X, Y], Idx) =>
    {
        Place("DDayCzechHedgehog", `Czech Hedgehog ${Idx + 1}`, X, Y, 0.0, Idx * 0.65, true);
    });

    const TeethLines = [
        [-90.0,  21.0,  0.15], [-90.0, -21.0, -0.15],
        [  4.0,  21.5, -0.12], [  4.0, -21.5,  0.12],
        [ 96.0,  22.0,  0.10], [ 96.0, -22.0, -0.10],
        [182.0,  22.0, -0.12], [182.0, -22.0,  0.12],
        [274.0,  20.5,  0.22], [274.0, -20.5, -0.22],
        [298.0,  18.5,  0.30], [298.0, -18.5, -0.30],
        [ 20.0,  64.0,  0.00], [ 20.0, -64.0,  0.00],
        [220.0,  64.0,  0.00], [220.0, -64.0,  0.00]
    ];
    TeethLines.forEach(([X, Y, Yaw], Idx) =>
    {
        Place("DDayDragonsTeeth", `Dragon's Teeth Line ${Idx + 1}`, X, Y, 0.0, Yaw, true);
    });

    // ⑥ THE ATLANTIC FORTRESS WALL (X = +318 m, continuous from Y = -132 m to +132 m except Central 22 m Breach Gate!)
    for (let Seg = 0; Seg < 9; ++Seg)
    {
        const YNorth =  20.4 + Seg * 12.6;
        const YSouth = -20.4 - Seg * 12.6;
        Place("DDayFortressWallSegment", `Atlantic Wall North Rampart ${Seg + 1}`, DDayWallX, YNorth, 0.0, 0.0, true);
        Place("DDayFortressWallSegment", `Atlantic Wall South Rampart ${Seg + 1}`, DDayWallX, YSouth, 0.0, 0.0, true);
    }
    // Central Armored Breach Gate Archway (clear Z = 0..6.6 m, Y = -10.9..+10.9 m; collider false so car races under the arch!)
    Place("DDayWallBreachArch", "Atlantic Wall Breach Gatehouse", DDayWallX, 0.0, 0.0, 0.0, false);
    // Gatehouse jambs & 4 Towering Fortress Watchtowers (with true colliders flanking the gate!)
    Place("DDayWallWatchtower", "Breach Gate North Watchtower", 315.5,  15.6, 0.0, 0.0, true);
    Place("DDayWallWatchtower", "Breach Gate South Watchtower", 315.5, -15.6, 0.0, 0.0, true);
    Place("DDayWallWatchtower", "North Bastion Watchtower",     315.5,  66.0, 0.0, 0.0, true);
    Place("DDayWallWatchtower", "South Bastion Watchtower",     315.5, -66.0, 0.0, 0.0, true);

    // ⑦ Minefield Warning Posts (non-colliding visual markers beside mine belts)
    const MinePosts = [
        [-106.0, -13.5], [-106.0, 13.5], [-28.0, -13.5], [-28.0, 13.5],
        [   4.0, -13.5], [   4.0, 13.5], [146.0, -13.5], [146.0, 13.5],
        [ 174.0, -13.5], [ 174.0, 13.5], [286.0, -13.5], [286.0, 13.5]
    ];
    MinePosts.forEach(([X, Y], Idx) =>
    {
        Place("DDayMinePost", `Minefield Marker ${Idx + 1}`, X, Y, 0.0, 0.0, false);
    });

    // ⑧ Battlefield Perimeter Jersey Guard along outer North/South/East courtyard edges
    for (let X = -188.0; X <= 304.0; X += 24.0)
    {
        Place("Jersey", `Perimeter North ${X.toFixed(0)}`, X,  128.0, 0.0, 0.0, true);
        Place("Jersey", `Perimeter South ${X.toFixed(0)}`, X, -128.0, 0.0, 0.0, true);
    }
    for (let Y = -28.0; Y <= 28.0; Y += 8.0)
        Place("Jersey", `Citadel East Stop ${Y.toFixed(0)}`, 354.0, Y, 0.0, Math.PI * 0.5, true);

    return Placements;
}

export function ResolveDDayCones()
{
    const Spots = [
        [-142.0,  -5.5], [-142.0,   5.5], [-118.0,   4.8], [ -56.0,  -6.2],
        [   2.0,   6.0], [  70.0,  -5.8], [ 152.0,   5.8], [ 216.0,  -5.8],
        [ 296.0,  -8.2], [ 296.0,   8.2], [ 306.0,  -8.2], [ 306.0,   8.2]
    ];
    return Spots.map(([X, Y], I) => ({
        Name:     `Assault Marker Cone ${I + 1}`,
        Group:    "lone",
        Shape:    "Cone",
        Position: [X, Y, 0.0],
        Yaw:      I * 0.5
    }));
}

// 📝 Smashable Wooden Military Munition Crates & Brittle Concrete Sapper Blocks across tactical shortcuts!
export function ResolveDDayBreakables()
{
    const Specs = [
        { Name: "Beachhead Ammo Crate 1",    Mat: "wood",     Pos: [-122.0,  -2.2], Ext: [1.35, 1.35, 1.15] },
        { Name: "Pillbox Supply Crate 2",    Mat: "wood",     Pos: [ -58.0,   2.4], Ext: [1.35, 1.35, 1.15] },
        { Name: "Trench Barricade Crate 3",  Mat: "wood",     Pos: [  -8.0,  -2.8], Ext: [1.40, 1.40, 1.20] },
        { Name: "Casemate Munition Crate 4", Mat: "wood",     Pos: [  52.0,   2.6], Ext: [1.35, 1.35, 1.15] },
        { Name: "Sapper Roadblock Slab 5",   Mat: "concrete", Pos: [ 110.0,  -2.6], Ext: [0.85, 2.30, 1.05] },
        { Name: "Artillery Supply Crate 6",  Mat: "wood",     Pos: [ 168.0,   2.4], Ext: [1.40, 1.40, 1.20] },
        { Name: "Forward Sapper Slab 7",     Mat: "concrete", Pos: [ 234.0,  -2.4], Ext: [0.85, 2.30, 1.05] },
        { Name: "North Sapper Barricade 8",  Mat: "wood",     Pos: [  42.0,  47.5], Ext: [0.95, 2.50, 1.15] },
        { Name: "South Sapper Barricade 9",  Mat: "wood",     Pos: [  42.0, -47.5], Ext: [0.95, 2.50, 1.15] },
        { Name: "Wall Funnel Left Crate 10", Mat: "wood",     Pos: [ 284.0,  -5.4], Ext: [1.35, 1.35, 1.15] },
        { Name: "Wall Funnel Right Crate 11",Mat: "wood",     Pos: [ 284.0,   5.4], Ext: [1.35, 1.35, 1.15] },
        { Name: "Breach Gate Wooden Barrier",Mat: "wood",     Pos: [ 302.0,   0.0], Ext: [0.95, 2.80, 1.15] }
    ];
    return Specs.map((S, I) => ({
        Name:     S.Name,
        Material: S.Mat,
        Mode:     "baked",
        Shape:    { Kind: "box", Extent: S.Ext },
        Position: [S.Pos[0], S.Pos[1], 0.0],
        Yaw:      (I % 2 === 0 ? 0.12 : -0.12),
        Approach: [1.0, 0.0, 0.0],
        Capacity: 10,
        Energy:   S.Mat === "concrete" ? 6800 : 3800,
        Seed:     4406 + I * 41
    }));
}

export function ResolveDDayTrafficLights()
{
    return [];
}

// 📝 Multi-zone D-Day Ground Mesh: Western Surf Shallows -> Beach Sand Apron -> Scorched No-Man's-Land -> Concrete Wall Citadel + Markings!
export function EmitDDayGroundFacets()
{
    const Extent = 920.0;
    const Facets = [];
    const Quad = (X0, Y0, X1, Y1, Z, Family) =>
    {
        Facets.push(
            { Family, Corners: [[X0, Y0, Z], [X1, Y0, Z], [X1, Y1, Z]] },
            { Family, Corners: [[X0, Y0, Z], [X1, Y1, Z], [X0, Y1, Z]] }
        );
    };

    // ① Western English Channel Landing Shallows (X = -920 .. -162 m)
    Quad(-Extent, -Extent, -162.0, Extent, 0.0, CourseMaterial.WallCobalt);
    // Wet shingle surf strip along landing ramps (X = -162 .. -142 m)
    Quad(-162.0, -145.0, -142.0, 145.0, 0.0, CourseMaterial.AsphaltPatch);
    // ② Sandy Omaha Beachhead & Shingle Zone (X = -142 .. -60 m)
    Quad(-142.0, -145.0, -60.0, 145.0, 0.0, CourseMaterial.Gravel);
    // ③ Scorched Militarized Combat Corridor (X = -60 .. +285 m)
    Quad(-60.0, -145.0, 285.0, 145.0, 0.0, CourseMaterial.AsphaltPatch);
    // ④ Reinforced Concrete Atlantic Wall Forecourt & Eastern Citadel Courtyard (X = +285 .. +370 m)
    Quad(285.0, -145.0, 370.0, 145.0, 0.0, CourseMaterial.ApronConcrete);
    // ⑤ Outer Coastal Hinterland Grass Flanks
    Quad(-162.0,  145.0, Extent,  Extent, 0.0, CourseMaterial.Grass);
    Quad(-162.0, -Extent, Extent, -145.0, 0.0, CourseMaterial.Grass);
    Quad( 370.0, -145.0, Extent,  145.0, 0.0, CourseMaterial.Grass);

    // ⑥ Tactical Assault Corridor & Breach Gate Ground Markings (+3 mm lift)
    const Lift = 0.003;
    for (let X = 286.0; X <= 316.0; X += 5.0)
    {
        Quad(X, -9.6, X + 3.0, -9.1, Lift, CourseMaterial.PaintGreen);
        Quad(X,  9.1, X + 3.0,  9.6, Lift, CourseMaterial.PaintGreen);
    }
    // Checkered Victory Threshold Line across the Atlantic Wall Breach at X = 310 m
    for (let S = -9; S < 9; ++S)
    {
        const Mat = (S & 1) === 0 ? CourseMaterial.PaintWhite : CourseMaterial.PaintYellow;
        Quad(309.5, S * 1.0, 311.2, (S + 1) * 1.0, Lift, Mat);
    }

    return Facets;
}
