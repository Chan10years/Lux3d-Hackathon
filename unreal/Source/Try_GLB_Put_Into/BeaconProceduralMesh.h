#pragma once

#include "CoreMinimal.h"

class UProceduralMeshComponent;

namespace BeaconProceduralMesh
{
	/** Populates section 0 of Target with a double-terminated faceted Origin Energy Crystal (~110 cm tall). */
	void BuildCoreCrystal(UProceduralMeshComponent* Target);

	/** Populates section 0 of Target with 4 floating satellite crystal shards at 90-degree intervals (radius ~48 cm). */
	void BuildOrbitShards(UProceduralMeshComponent* Target);

	/** Populates section 0 of Target with a crisp beveled octagonal calibration ring of the given Radius and Thickness. */
	void BuildCalibrationRing(UProceduralMeshComponent* Target, float Radius, float Thickness, const FLinearColor& Tint);

	/** Populates section 0 of Target with a ground pickup halo ring (outer radius ~PickupRadius cm) and inner rune hexagon on the ground plane. */
	void BuildGroundHalo(UProceduralMeshComponent* Target, float PickupRadius = 130.f);

	/** Populates section 0 of Target with a multi-layered vertical Skyward Light Pillar (inner core + outer aura up to PillarHeight cm). */
	void BuildLightPillar(UProceduralMeshComponent* Target, float PillarHeight = 2800.f);
}
