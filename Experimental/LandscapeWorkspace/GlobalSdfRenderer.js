/*==========================================================================================================================================
                                                          GLOBALSDFRENDERER.JS
==========================================================================================================================================*/
// 🧩 WebGL2 Fullscreen Ray-Space Global SDF + Spectral Ocean Renderer:
//    • Hardware GL_LINEAR 3D R16F sparse-brick atlas + RGBA8UI L∞ Chebyshev indirection grid (zero LOD).
//    • Sub-millisecond incremental gl.texSubImage3D brick updates for interactive 60 FPS 3D sculpting & Undo/Redo.
//    • Exact camera-relative ray math shared identically between GPU shader and CPU 3D sculpt raypicker.

import { GlobalSdfVertexShader, GlobalSdfFragmentShader, MAX_SHADER_LAYERS } from './GlobalSdfShader.js';
import { GlobalSlotToCoords } from './GlobalSdfBrickAtlas.js';
import { ColorToRgb01 } from './GlobalSdfField.js';
import { EvaluateOcean, OceanPhaseParameters, PrepareOceanSpectrum } from './OceanSurface.js';

export const DEFAULT_FOCAL_LENGTH = 1.65; // ~62.4° vertical field of view

//------------------------------------------------------------------------------------------------------------------------
//                                  EXACT SHARED CAMERA BASIS & RAY UNPROJECTION
//------------------------------------------------------------------------------------------------------------------------

export function ComputeCameraBasis(Camera)
{
    const Yaw      = Number(Camera?.Yaw) || 0;
    const Pitch    = Math.max(-1.50, Math.min(1.50, Number(Camera?.Pitch) || 0.42));
    const Distance = Math.max(2.0, Number(Camera?.Distance) || 1200);
    const Target   = Camera?.Target || [0, 40, 0];

    const SinY = Math.sin(Yaw),   CosY = Math.cos(Yaw);
    const SinP = Math.sin(Pitch), CosP = Math.cos(Pitch);

    // Matches CameraNavigation.js horizontal frame: Right = (+cosY, 0, -sinY), Forward_horiz = (-sinY, 0, -cosY)
    const Forward = [-SinY * CosP, -SinP, -CosY * CosP];
    const Right   = [ CosY,         0.0,  -SinY];
    // Up = Right × Forward (orthonormal right-handed basis)
    const Up = [
        Right[1] * Forward[2] - Right[2] * Forward[1],
        Right[2] * Forward[0] - Right[0] * Forward[2],
        Right[0] * Forward[1] - Right[1] * Forward[0]
    ];

    const Eye = [
        Target[0] - Forward[0] * Distance,
        Target[1] - Forward[1] * Distance,
        Target[2] - Forward[2] * Distance
    ];

    return { Eye, Forward, Right, Up, Target, Distance, Yaw, Pitch };
}

export function UnprojectScreenRay(Camera, ClientX, ClientY, Rect, FocalLength = DEFAULT_FOCAL_LENGTH)
{
    const Width  = Math.max(1, Rect.width);
    const Height = Math.max(1, Rect.height);
    const NdcX   = ((ClientX - Rect.left) / Width) * 2.0 - 1.0;
    const NdcY   = 1.0 - ((ClientY - Rect.top) / Height) * 2.0;
    const Aspect = Width / Height;

    const Basis = ComputeCameraBasis(Camera);
    const RX = Basis.Forward[0] * FocalLength + Basis.Right[0] * (NdcX * Aspect) + Basis.Up[0] * NdcY;
    const RY = Basis.Forward[1] * FocalLength + Basis.Right[1] * (NdcX * Aspect) + Basis.Up[1] * NdcY;
    const RZ = Basis.Forward[2] * FocalLength + Basis.Right[2] * (NdcX * Aspect) + Basis.Up[2] * NdcY;
    const InvLen = 1.0 / (Math.hypot(RX, RY, RZ) || 1.0);

    return {
        Origin: Basis.Eye,
        Dir: [RX * InvLen, RY * InvLen, RZ * InvLen],
        Basis
    };
}

//------------------------------------------------------------------------------------------------------------------------
//                          CPU ANALYTIC RAY-OCEAN INTERSECTION (EXACT SHADER EQUIVALENT)
//------------------------------------------------------------------------------------------------------------------------

export function TraceOceanRayCpu(Origin, Dir, MaxRayT, Ocean, Seconds = 0, PixelAngle = 0.001)
{
    if (!Ocean || Ocean.Enabled === false) return null;
    const WaveAmp = Math.max(0, Number(Ocean.WaveAmplitude) ?? 0.65);
    const MaxAmp  = Math.max(0.06, WaveAmp * 2.45);

    const OY = Origin[1], DY = Dir[1];
    let T0 = 0, T1 = 0;

    if (OY > MaxAmp)
    {
        if (DY >= -1e-5) return null;
        T0 = (MaxAmp - OY) / DY;
        T1 = (-MaxAmp - OY) / DY;
    }
    else if (OY >= -MaxAmp)
    {
        T0 = 0.02;
        if (DY < -1e-4)      T1 = (-MaxAmp - OY) / DY;
        else if (DY > 1e-4)  T1 = ( MaxAmp - OY) / DY;
        else                 T1 = 6000.0;
    }
    else
    {
        if (DY <= 1e-5) return null;
        T0 = (-MaxAmp - OY) / DY;
        T1 = ( MaxAmp - OY) / DY;
    }

    if (T0 >= MaxRayT) return null;
    T0 = Math.max(0.02, T0);
    T1 = Math.min(MaxRayT, Math.min(T1, 160000.0));
    if (T1 <= T0) return null;

    const Sign0 = OY < -MaxAmp ? -1.0 : 1.0;
    const EvalG = (T) =>
    {
        const X  = Origin[0] + Dir[0] * T;
        const Y  = Origin[1] + Dir[1] * T;
        const Z  = Origin[2] + Dir[2] * T;
        const FP = T * PixelAngle / Math.max(Math.abs(DY), 0.06);
        return Y - EvaluateOcean(X, Z, Seconds, WaveAmp, FP).Height;
    };

    let PrevT = T0;
    let PrevG = EvalG(PrevT);
    if (PrevG * Sign0 <= 0.0)
    {
        return { Distance: PrevT, Position: [Origin[0] + Dir[0] * PrevT, Origin[1] + Dir[1] * PrevT, Origin[2] + Dir[2] * PrevT] };
    }

    const Steps = 12;
    for (let I = 1; I <= Steps; I++)
    {
        const U    = I / Steps;
        const CurT = T0 + (T1 - T0) * (U * (0.45 + 0.55 * U));
        const CurG = EvalG(CurT);
        if (CurG * Sign0 <= 0.0)
        {
            let Lo = PrevT, Hi = CurT;
            for (let B = 0; B < 6; B++)
            {
                const Mid = 0.5 * (Lo + Hi);
                if (EvalG(Mid) * Sign0 > 0.0) Lo = Mid;
                else                          Hi = Mid;
            }
            const HitT = 0.5 * (Lo + Hi);
            return {
                Distance: HitT,
                Position: [Origin[0] + Dir[0] * HitT, Origin[1] + Dir[1] * HitT, Origin[2] + Dir[2] * HitT]
            };
        }
        PrevT = CurT;
        PrevG = CurG;
    }
    return null;
}

//------------------------------------------------------------------------------------------------------------------------
//                                      WEBGL2 GLOBAL SDF RENDERER CLASS
//------------------------------------------------------------------------------------------------------------------------

function CompileShader(GL, Type, Source)
{
    const Shader = GL.createShader(Type);
    GL.shaderSource(Shader, Source);
    GL.compileShader(Shader);
    if (!GL.getShaderParameter(Shader, GL.COMPILE_STATUS))
    {
        const Log = GL.getShaderInfoLog(Shader) || 'Unknown shader compile error';
        GL.deleteShader(Shader);
        throw new Error(`GlobalSdfShader compile failure: ${Log}`);
    }
    return Shader;
}

export class GlobalSdfRenderer
{
    constructor(Canvas)
    {
        this.Canvas = Canvas;
        const GL = Canvas.getContext('webgl2', {
            alpha: false,
            depth: false,
            stencil: false,
            antialias: false,
            preserveDrawingBuffer: true,
            powerPreference: 'high-performance'
        });
        if (!GL)
        {
            throw new Error('WebGL2 is required for the Global 3D SDF Landscape Editor.');
        }
        this.GL = GL;
        GL.pixelStorei(GL.UNPACK_ALIGNMENT, 1);

        const VS = CompileShader(GL, GL.VERTEX_SHADER, GlobalSdfVertexShader);
        const FS = CompileShader(GL, GL.FRAGMENT_SHADER, GlobalSdfFragmentShader);
        const Program = GL.createProgram();
        GL.attachShader(Program, VS);
        GL.attachShader(Program, FS);
        GL.linkProgram(Program);
        GL.deleteShader(VS);
        GL.deleteShader(FS);

        if (!GL.getProgramParameter(Program, GL.LINK_STATUS))
        {
            const Log = GL.getProgramInfoLog(Program) || 'Unknown program link error';
            GL.deleteProgram(Program);
            throw new Error(`GlobalSdfShader link failure: ${Log}`);
        }

        this.Program = Program;
        this.VAO     = GL.createVertexArray();
        this.IndirectionTex = null;
        this.BrickAtlasTex  = null;
        this.PackedState    = null;
        this.LayersMeta     = [];
        this.RenderScale    = 1.0;

        this._CacheUniformLocations();
        this._CreatePlaceholderTextures();
    }

    _CacheUniformLocations()
    {
        const GL = this.GL;
        const P  = this.Program;
        const U  = (Name) => GL.getUniformLocation(P, Name);

        this.Uniforms = {
            uCamEye:             U('uCamEye'),
            uCamForward:         U('uCamForward'),
            uCamRight:           U('uCamRight'),
            uCamUp:              U('uCamUp'),
            uResolution:         U('uResolution'),
            uFocalLength:        U('uFocalLength'),
            uPixelAngle:         U('uPixelAngle'),
            uIndirection:        U('uIndirection'),
            uBrickAtlas:         U('uBrickAtlas'),
            uInvAtlasTexelDims:  U('uInvAtlasTexelDims'),
            uLayerCount:         U('uLayerCount'),
            uLayerBoundsMin:     U('uLayerBoundsMin[0]'),
            uLayerBoundsMax:     U('uLayerBoundsMax[0]'),
            uLayerPitch:         U('uLayerPitch[0]'),
            uLayerBrickWorld:    U('uLayerBrickWorld[0]'),
            uLayerGridDims:      U('uLayerGridDims[0]'),
            uLayerIndYOffset:    U('uLayerIndYOffset[0]'),
            uLayerBand:          U('uLayerBand[0]'),
            uLayerMinVoxel:      U('uLayerMinVoxel[0]'),
            uLayerColor:         U('uLayerColor[0]'),
            uLayerCliffColor:    U('uLayerCliffColor[0]'),
            uLayerSurfaceParams: U('uLayerSurfaceParams[0]'),
            uOceanEnabled:       U('uOceanEnabled'),
            uOceanTime:          U('uOceanTime'),
            uOceanWaveAmp:       U('uOceanWaveAmp'),
            uOceanVectors:       U('uOceanVectors[0]'),
            uOceanPhases:        U('uOceanPhases[0]'),
            uSunDir:             U('uSunDir'),
            uBrushActive:        U('uBrushActive'),
            uBrushMode:          U('uBrushMode'),
            uBrushCenter:        U('uBrushCenter'),
            uBrushRadius:        U('uBrushRadius'),
            uBrushFalloff:       U('uBrushFalloff')
        };

        GL.useProgram(P);
        GL.uniform1i(this.Uniforms.uIndirection, 0);
        GL.uniform1i(this.Uniforms.uBrickAtlas,  1);
        GL.uniform2fv(this.Uniforms.uOceanPhases, OceanPhaseParameters);
    }

    _CreatePlaceholderTextures()
    {
        const GL = this.GL;

        this.IndirectionTex = GL.createTexture();
        GL.activeTexture(GL.TEXTURE0);
        GL.bindTexture(GL.TEXTURE_3D, this.IndirectionTex);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_MIN_FILTER, GL.NEAREST);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_MAG_FILTER, GL.NEAREST);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_WRAP_S, GL.CLAMP_TO_EDGE);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_WRAP_T, GL.CLAMP_TO_EDGE);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_WRAP_R, GL.CLAMP_TO_EDGE);
        GL.texStorage3D(GL.TEXTURE_3D, 1, GL.RGBA8UI, 1, 1, 1);
        GL.texSubImage3D(GL.TEXTURE_3D, 0, 0, 0, 0, 1, 1, 1, GL.RGBA_INTEGER, GL.UNSIGNED_BYTE, new Uint8Array([255, 255, 255, 192]));

        this.BrickAtlasTex = GL.createTexture();
        GL.activeTexture(GL.TEXTURE1);
        GL.bindTexture(GL.TEXTURE_3D, this.BrickAtlasTex);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_MIN_FILTER, GL.LINEAR);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_MAG_FILTER, GL.LINEAR);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_WRAP_S, GL.CLAMP_TO_EDGE);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_WRAP_T, GL.CLAMP_TO_EDGE);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_WRAP_R, GL.CLAMP_TO_EDGE);
        GL.texStorage3D(GL.TEXTURE_3D, 1, GL.R16F, 10, 10, 10);
    }

    UploadPackedState(PackedState, WorldLayers = [])
    {
        const GL = this.GL;
        this.PackedState = PackedState;

        if (this.IndirectionTex) GL.deleteTexture(this.IndirectionTex);
        if (this.BrickAtlasTex)  GL.deleteTexture(this.BrickAtlasTex);

        const [IX, IY, IZ] = PackedState.IndirectionDims;
        this.IndirectionTex = GL.createTexture();
        GL.activeTexture(GL.TEXTURE0);
        GL.bindTexture(GL.TEXTURE_3D, this.IndirectionTex);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_MIN_FILTER, GL.NEAREST);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_MAG_FILTER, GL.NEAREST);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_WRAP_S, GL.CLAMP_TO_EDGE);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_WRAP_T, GL.CLAMP_TO_EDGE);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_WRAP_R, GL.CLAMP_TO_EDGE);
        GL.texStorage3D(GL.TEXTURE_3D, 1, GL.RGBA8UI, IX, IY, IZ);
        GL.texSubImage3D(GL.TEXTURE_3D, 0, 0, 0, 0, IX, IY, IZ, GL.RGBA_INTEGER, GL.UNSIGNED_BYTE, PackedState.IndirectionBytes);

        const [AX, AY, AZ] = PackedState.AtlasTexelDims;
        this.BrickAtlasTex = GL.createTexture();
        GL.activeTexture(GL.TEXTURE1);
        GL.bindTexture(GL.TEXTURE_3D, this.BrickAtlasTex);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_MIN_FILTER, GL.LINEAR);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_MAG_FILTER, GL.LINEAR);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_WRAP_S, GL.CLAMP_TO_EDGE);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_WRAP_T, GL.CLAMP_TO_EDGE);
        GL.texParameteri(GL.TEXTURE_3D, GL.TEXTURE_WRAP_R, GL.CLAMP_TO_EDGE);
        GL.texStorage3D(GL.TEXTURE_3D, 1, GL.R16F, AX, AY, AZ);
        GL.texSubImage3D(GL.TEXTURE_3D, 0, 0, 0, 0, AX, AY, AZ, GL.RED, GL.HALF_FLOAT, PackedState.AtlasHalfData);

        this.SyncLayerUniforms(WorldLayers);
    }

    // Incremental sub-millisecond GPU upload during interactive 3D sculpting & Undo/Redo
    UploadDirtyBricks(Volume, DirtyLocalBricks, DirtyIndirectionBox = null)
    {
        const GL = this.GL;
        if (!this.PackedState || !Volume) return;

        if (DirtyLocalBricks && DirtyLocalBricks.length > 0)
        {
            GL.activeTexture(GL.TEXTURE1);
            GL.bindTexture(GL.TEXTURE_3D, this.BrickAtlasTex);
            for (let I = 0; I < DirtyLocalBricks.length; I++)
            {
                const LocalSlot  = DirtyLocalBricks[I];
                const GlobalSlot = Volume.GlobalSlotByBrick?.[LocalSlot];
                if (GlobalSlot === undefined || GlobalSlot < 0) continue;
                const [SX, SY, SZ] = GlobalSlotToCoords(GlobalSlot);
                GL.texSubImage3D(
                    GL.TEXTURE_3D, 0,
                    SX * 10, SY * 10, SZ * 10,
                    10, 10, 10,
                    GL.RED, GL.HALF_FLOAT,
                    Volume.BricksF16,
                    LocalSlot * 1000
                );
            }
        }

        if (DirtyIndirectionBox && this.IndirectionTex)
        {
            const [X0, Y0, Z0, X1, Y1, Z1] = DirtyIndirectionBox;
            const W = X1 - X0 + 1;
            const H = Y1 - Y0 + 1;
            const D = Z1 - Z0 + 1;
            const [IX, IY] = this.PackedState.IndirectionDims;
            const Src = this.PackedState.IndirectionBytes;
            const Sub = new Uint8Array(W * H * D * 4);
            let DstOff = 0;
            for (let Z = Z0; Z <= Z1; Z++)
            {
                for (let Y = Y0; Y <= Y1; Y++)
                {
                    const RowStart = ((Z * IY + Y) * IX + X0) * 4;
                    Sub.set(Src.subarray(RowStart, RowStart + W * 4), DstOff);
                    DstOff += W * 4;
                }
            }
            GL.activeTexture(GL.TEXTURE0);
            GL.bindTexture(GL.TEXTURE_3D, this.IndirectionTex);
            GL.texSubImage3D(GL.TEXTURE_3D, 0, X0, Y0, Z0, W, H, D, GL.RGBA_INTEGER, GL.UNSIGNED_BYTE, Sub);
        }
    }

    SyncLayerUniforms(WorldLayers = [])
    {
        if (!this.PackedState) return;
        const GL = this.GL;
        const U  = this.Uniforms;

        const ActiveRecords = [];

        for (const Rec of this.PackedState.ActiveLayers)
        {
            const Live    = WorldLayers[Rec.LayerIndex];
            const Visible = Live ? Live.Visible !== false : true;
            if (!Visible) continue;
            ActiveRecords.push({
                ...Rec,
                Color:      ColorToRgb01(Live?.Color      ?? Rec.Color,      [0.76, 0.65, 0.48]),
                CliffColor: ColorToRgb01(Live?.CliffColor ?? Rec.CliffColor, [0.52, 0.41, 0.30]),
                Roughness:  Live?.Roughness  ?? Rec.Roughness,
                Strata:     Live?.Strata     ?? Rec.Strata
            });
            if (ActiveRecords.length >= MAX_SHADER_LAYERS) break;
        }

        const Count = ActiveRecords.length;
        const BoundsMin     = new Float32Array(MAX_SHADER_LAYERS * 3);
        const BoundsMax     = new Float32Array(MAX_SHADER_LAYERS * 3);
        const Pitch         = new Float32Array(MAX_SHADER_LAYERS * 3);
        const BrickWorld    = new Float32Array(MAX_SHADER_LAYERS * 3);
        const GridDims      = new Int32Array(MAX_SHADER_LAYERS * 3);
        const IndYOffset    = new Int32Array(MAX_SHADER_LAYERS);
        const Band          = new Float32Array(MAX_SHADER_LAYERS);
        const MinVoxel      = new Float32Array(MAX_SHADER_LAYERS);
        const Color         = new Float32Array(MAX_SHADER_LAYERS * 3);
        const CliffColor    = new Float32Array(MAX_SHADER_LAYERS * 3);
        const SurfaceParams = new Float32Array(MAX_SHADER_LAYERS * 2);

        for (let K = 0; K < Count; K++)
        {
            const R = ActiveRecords[K];
            BoundsMin.set(R.BoundsMin, K * 3);
            BoundsMax.set(R.BoundsMax, K * 3);
            Pitch.set(R.Pitch, K * 3);
            BrickWorld.set(R.BrickWorld, K * 3);
            GridDims.set(R.GridDims, K * 3);
            IndYOffset[K] = R.IndirectionYOffset;
            Band[K]       = R.Band;
            MinVoxel[K]   = R.MinVoxel;
            Color.set(R.Color, K * 3);
            CliffColor.set(R.CliffColor, K * 3);
            SurfaceParams[K * 2 + 0] = R.Strata ?? 0.45;
            SurfaceParams[K * 2 + 1] = R.Roughness ?? 0.78;
        }

        const [AX, AY, AZ] = this.PackedState.AtlasTexelDims;

        GL.useProgram(this.Program);
        GL.uniform3f(U.uInvAtlasTexelDims, 1.0 / AX, 1.0 / AY, 1.0 / AZ);
        GL.uniform1i(U.uLayerCount, Count);
        if (Count > 0)
        {
            GL.uniform3fv(U.uLayerBoundsMin,     BoundsMin);
            GL.uniform3fv(U.uLayerBoundsMax,     BoundsMax);
            GL.uniform3fv(U.uLayerPitch,         Pitch);
            GL.uniform3fv(U.uLayerBrickWorld,    BrickWorld);
            GL.uniform3iv(U.uLayerGridDims,      GridDims);
            GL.uniform1iv(U.uLayerIndYOffset,    IndYOffset);
            GL.uniform1fv(U.uLayerBand,          Band);
            GL.uniform1fv(U.uLayerMinVoxel,      MinVoxel);
            GL.uniform3fv(U.uLayerColor,         Color);
            GL.uniform3fv(U.uLayerCliffColor,    CliffColor);
            GL.uniform2fv(U.uLayerSurfaceParams, SurfaceParams);
        }
    }

    ResizeIfNeeded()
    {
        const DPR    = Math.min(window.devicePixelRatio || 1, 2.0) * this.RenderScale;
        const Width  = Math.max(1, Math.round(this.Canvas.clientWidth * DPR));
        const Height = Math.max(1, Math.round(this.Canvas.clientHeight * DPR));
        if (this.Canvas.width !== Width || this.Canvas.height !== Height)
        {
            this.Canvas.width  = Width;
            this.Canvas.height = Height;
        }
        return { Width, Height };
    }

    RenderFrame({ Camera, Ocean, TimeSeconds = 0, BrushState = null, SunDirection = null })
    {
        const GL = this.GL;
        const U  = this.Uniforms;
        const { Width, Height } = this.ResizeIfNeeded();

        GL.viewport(0, 0, Width, Height);
        GL.useProgram(this.Program);

        const Basis       = ComputeCameraBasis(Camera);
        const FocalLength = DEFAULT_FOCAL_LENGTH;
        const PixelAngle  = 2.0 / (Math.max(1, Height) * FocalLength);

        GL.uniform3fv(U.uCamEye,     Basis.Eye);
        GL.uniform3fv(U.uCamForward, Basis.Forward);
        GL.uniform3fv(U.uCamRight,   Basis.Right);
        GL.uniform3fv(U.uCamUp,      Basis.Up);
        GL.uniform2f(U.uResolution,  Width, Height);
        GL.uniform1f(U.uFocalLength, FocalLength);
        GL.uniform1f(U.uPixelAngle,  PixelAngle);

        const OceanEnabled = Ocean && Ocean.Enabled !== false ? 1 : 0;
        const WaveAmp      = Math.max(0.0, Number(Ocean?.WaveAmplitude ?? 0.65));
        GL.uniform1i(U.uOceanEnabled, OceanEnabled);
        GL.uniform1f(U.uOceanTime,    TimeSeconds);
        GL.uniform1f(U.uOceanWaveAmp, WaveAmp);
        if (OceanEnabled)
        {
            GL.uniform4fv(U.uOceanVectors, PrepareOceanSpectrum(WaveAmp, 0.0));
        }

        const Sun = SunDirection || [0.565, 0.595, 0.571];
        const SunInv = 1.0 / (Math.hypot(Sun[0], Sun[1], Sun[2]) || 1.0);
        GL.uniform3f(U.uSunDir, Sun[0] * SunInv, Sun[1] * SunInv, Sun[2] * SunInv);

        if (BrushState && BrushState.Active && BrushState.Center)
        {
            const ModeIdx = BrushState.Mode === 'carve' || BrushState.Mode === 'remove' ? 1
                          : BrushState.Mode === 'smooth' ? 2
                          : BrushState.Mode === 'flatten' ? 3 : 0;
            GL.uniform1i(U.uBrushActive,  1);
            GL.uniform1i(U.uBrushMode,    ModeIdx);
            GL.uniform3fv(U.uBrushCenter, BrushState.Center);
            GL.uniform1f(U.uBrushRadius,  Math.max(0.5, Number(BrushState.Radius) || 35));
            GL.uniform1f(U.uBrushFalloff, Math.max(0.0, Math.min(1.0, Number(BrushState.Falloff) ?? 0.6)));
        }
        else
        {
            GL.uniform1i(U.uBrushActive, 0);
        }

        GL.activeTexture(GL.TEXTURE0);
        GL.bindTexture(GL.TEXTURE_3D, this.IndirectionTex);
        GL.activeTexture(GL.TEXTURE1);
        GL.bindTexture(GL.TEXTURE_3D, this.BrickAtlasTex);

        GL.bindVertexArray(this.VAO);
        GL.drawArrays(GL.TRIANGLES, 0, 3);
        GL.bindVertexArray(null);
    }

    Dispose()
    {
        const GL = this.GL;
        if (this.IndirectionTex) GL.deleteTexture(this.IndirectionTex);
        if (this.BrickAtlasTex)  GL.deleteTexture(this.BrickAtlasTex);
        if (this.VAO)            GL.deleteVertexArray(this.VAO);
        if (this.Program)        GL.deleteProgram(this.Program);
    }
}
