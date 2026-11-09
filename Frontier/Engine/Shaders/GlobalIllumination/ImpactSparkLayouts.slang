//============================================================================================================================================
//                                                       IMPACTSPARKLAYOUTS.SLANG
//============================================================================================================================================
// 🧩 CPU/GPU layout for Jolt impact particles: a 16-byte header and up to 1024 std140-compatible 48-byte streak records.

#pragma once

static const uint ImpactSparkCapacity = 1024u;

struct ImpactSparkRecord
{
    float4 HeadWidth;        // xyz [m] current head, w [m] world-space streak width
    float4 TailPixelWidth;   // xyz [m] prior-frame head, w [-] bounded pixel-width factor
    float4 RadianceShutter;  // rgb [linear radiance], w [-] motion-shutter fraction
};

struct ImpactSparkHeader
{
    uint4 Header;            // x active particle count; kept in a small uniform range
};
