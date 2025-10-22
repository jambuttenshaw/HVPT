#pragma once


#ifdef __cplusplus
#include "HLSLTypeAliases.h"

namespace UE::HLSL
{
#endif


struct FHVPT_Reservoir
{
	float RunningSum;	// The sum of all weights of all samples processed by this reservoir
	float M;			// The effective sample account of this reservoir - the number of candidate samples processed
	float P_y;			// The target function evaluation for the sample in this reservoir

	// PackedData.x:
	//		Top 28 bits: SampledPixel (linear index into reservoir buffer) (23 bits are required for 3840x2160 frame)
	//		Next 3 bits: NumExtraBounces (supports up to 8 bounces in total)
	//		Final bit:   bEmissionPath
	// PackedData.y:
	//		Top 16 bits: LightId (supporting up to 65536 lights)
	//		Bottom 16 bits: Depth as 16-bit float
	uint2 PackedData;
	float2 LightSample;

#ifndef __cplusplus
	void SetEmissionPath(bool bEmissionPath)
	{
		PackedData.x = (PackedData.x & 0xFFFFFFE) | (bEmissionPath & 0x00000001);
	}
	bool GetEmissionPath()
	{
		return (PackedData.x & 0x00000001);
	}

	void SetNumExtraBounces(uint NumExtraBounces)
	{
		PackedData.x = (PackedData.x & 0xFFFFFF1) | ((NumExtraBounces << 1) & 0x0000000E);
	}
	uint GetNumExtraBounces()
	{
		return (PackedData.x & 0x0000000E) >> 1;
	}

	void SetSampledPixel(uint SampledPixel)
	{
		PackedData.x = (PackedData.x & 0x0000000F) | (SampledPixel << 4);
	}
	uint GetSampledPixel()
	{
		return PackedData.x >> 4;
	}

	void SetDepth(float Depth)
	{
		PackedData.y = (PackedData.y & 0xFFFF0000) | f32tof16(Depth);
	}
	float GetDepth()
	{
		return f16tof32(PackedData.y);
	}

	void SetLightId(uint LightId)
	{
		PackedData.y = (PackedData.y & 0x0000FFFF) | (LightId << 16);
	}
	uint GetLightId()
	{
		return PackedData.y >> 16;
	}
#endif
};


// (omega, z) tuples describing subsequent bounces after the first scattering event
struct FHVPT_Bounce
{
	// X - Direction encoded with octahedron mapping with 16 bits per component (X component in high bits, Y component in low bits)
	// Y - Distance
	// Encoding functions defined in ReSTIRUtils.ush
	uint2 PackedData;
};


// Debug tools

// Flags and view modes are packed together into a single uint
// Flags occupy top 24 bits
// Debug view modes are placed into bottom 8 bits (giving 256 possible view modes)

// Debug flags
#define HVPT_DEBUG_FLAG_ENABLE					0x00000100

// Debug view modes
#define HVPT_DEBUG_VIEW_MODE_NUM_BOUNCES		0x00
#define HVPT_DEBUG_VIEW_MODE_PATH_TYPE			0x01		// Scattering vs Emission vs Surface
#define HVPT_DEBUG_VIEW_MODE_LIGHT_ID			0x02
#define HVPT_DEBUG_VIEW_MODE_TEMPORAL_REUSE		0x03		// Whether temporal sample was selected
#define HVPT_DEBUG_VIEW_MODE_FIREFLY_DETECTION	0x04		// Visualize when sum in reservoirs is very high to detect fireflies
#define HVPT_DEBUG_VIEW_MODE_REPROJECTION		0x05		// Visualizes difference between pixel position and reprojected pixel position

#define HVPT_DEBUG_VIEW_MODE_CUSTOM				0xFF		// Used for temporary debug visualization


#ifdef __cplusplus
}

using FHVPT_Reservoir = UE::HLSL::FHVPT_Reservoir;
using FHVPT_Bounce = UE::HLSL::FHVPT_Bounce;

#endif
