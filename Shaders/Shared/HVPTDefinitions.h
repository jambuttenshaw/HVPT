#pragma once


#ifdef __cplusplus
#include "HLSLTypeAliases.h"

namespace UE::HLSL
{
#endif


struct FHVPT_Reservoir
{
	// TODO: Data packing
	// TODO: Pack pixel index and num extra bounces together (into one int? would give max resolution of 16k x 16k and 15 max bounces)
	// TODO: Also infer bEmissionPath from LightId. LightId doens't need 32 bits of precision, could it be packed with another field?

	float RunningSum;	// The sum of all weights of all samples processed by this reservoir
	float M;			// The effective sample account of this reservoir - the number of candidate samples processed
	float P_y;			// The target function evaluation for the sample in this reservoir

	// Data to recover the path stored in the reservoir
	bool bEmissionPath;   // Whether this path is a scattering or emission path
	uint NumExtraBounces; // The number of bounces stored in the extra bounces buffer
	uint2 SampledPixel;	  // The pixel that this reservoir samples in (useful for retrieving where to read/write extra bounces from/to)
	float Depth;		  // Distance along ray that first scattering event occurs
	int LightId;		  // If scattering path, the light source that the path should sample
	float2 LightSample;   // RNG for the light sample to re-eval the same point on the light
};


// (omega, z) tuples describing subsequent bounces after the first scattering event
struct FHVPT_Bounce
{
	// TODO: Pack to float 3
	float3 Direction;
	float Distance;
};


// Debug tools

// Flags and view modes are packed together into a single uint
// Flags occupy top 24 bits
// Debug view modes are placed into bottom 8 bits (giving 256 possible view modes)

// Debug flags
#define HVPT_DEBUG_FLAG_ENABLE					0x00000100

// Debug view modes
#define HVPT_DEBUG_VIEW_MODE_NUM_BOUNCES		0x00
#define HVPT_DEBUG_VIEW_MODE_PATH_TYPE			0x01		// Scattering vs Emission
#define HVPT_DEBUG_VIEW_MODE_LIGHT_ID			0x02
#define HVPT_DEBUG_VIEW_MODE_TEMPORAL_REUSE		0x03		// Whether temporal sample was selected
#define HVPT_DEBUG_VIEW_MODE_FIREFLY_DETECTION	0x04		// Visualize when sum in reservoirs is very high to detect fireflies

#define HVPT_DEBUG_VIEW_MODE_CUSTOM				0xFF		// Used for temporary debug visualization


#ifdef __cplusplus
}

using FHVPT_Reservoir = UE::HLSL::FHVPT_Reservoir;
using FHVPT_Bounce = UE::HLSL::FHVPT_Bounce;

#endif
