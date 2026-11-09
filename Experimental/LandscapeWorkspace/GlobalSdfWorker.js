/*==========================================================================================================================================
                                                           GLOBALSDFWORKER.JS
==========================================================================================================================================*/
// 🧩 Off-Main-Thread Per-Layer 3D Global SDF Builder Worker:
//    Builds full-resolution sparse 8³-brick (10³ trilinear) SDF layers concurrently and transfers TypedArray buffers zero-copy.

import { BuildLayerSparseSdf } from './GlobalSdfBrickAtlas.js';

self.onmessage = (Event) =>
{
    const Payload = Event.data;
    if (!Payload || Payload.Type !== 'build-layer') return;

    const { RequestId, Layer, LayerIndex } = Payload;
    try
    {
        const Volume = BuildLayerSparseSdf(Layer, LayerIndex ?? 0);
        const Transfer = [
            Volume.IsAllocated.buffer,
            Volume.IsSolidCore.buffer,
            Volume.ChebyshevDist.buffer,
            Volume.BrickIndexByCell.buffer,
            Volume.BrickCells.buffer,
            Volume.BricksF32.buffer,
            Volume.BricksF16.buffer,
            Volume.GridPlanar.buffer,
            Volume.GridSurfY.buffer,
            Volume.GridSlope.buffer
        ];
        self.postMessage({ Type: 'layer-built', RequestId, LayerIndex: LayerIndex ?? 0, Volume }, Transfer);
    }
    catch (Err)
    {
        self.postMessage({
            Type: 'layer-error',
            RequestId,
            LayerIndex: LayerIndex ?? 0,
            Message: Err instanceof Error ? Err.message : String(Err)
        });
    }
};
