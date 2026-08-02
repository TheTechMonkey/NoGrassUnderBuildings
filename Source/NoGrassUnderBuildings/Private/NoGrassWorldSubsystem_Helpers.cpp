// Static utility methods — location quantization, spatial cell mapping,
// and the segment-box intersection test.

#include "NoGrassInternal.h"

FIntVector UNoGrassWorldSubsystem::QuantizeLocation(const FVector& Location)
{
	return FIntVector(
		FMath::RoundToInt(Location.X),
		FMath::RoundToInt(Location.Y),
		FMath::RoundToInt(Location.Z));
}

FIntPoint UNoGrassWorldSubsystem::SpatialCellForLocation(const FVector& Location)
{
	return FIntPoint(
		FMath::FloorToInt(Location.X / NoGrass::CoverageTileSize),
		FMath::FloorToInt(Location.Y / NoGrass::CoverageTileSize));
}

bool UNoGrassWorldSubsystem::SegmentIntersectsBox(
	const FVector& Start,
	const FVector& End,
	const FBox& Box)
{
	if (!Box.IsValid)
	{
		return false;
	}

	const FVector Delta = End - Start;
	double MinTime = 0.0;
	double MaxTime = 1.0;

	for (int32 Axis = 0; Axis < 3; ++Axis)
	{
		const double AxisStart = Start[Axis];
		const double AxisDelta = Delta[Axis];
		const double AxisMin = Box.Min[Axis];
		const double AxisMax = Box.Max[Axis];

		if (FMath::Abs(AxisDelta) <= UE_SMALL_NUMBER)
		{
			if (AxisStart < AxisMin || AxisStart > AxisMax)
			{
				return false;
			}
			continue;
		}

		double TimeA = (AxisMin - AxisStart) / AxisDelta;
		double TimeB = (AxisMax - AxisStart) / AxisDelta;
		if (TimeA > TimeB)
		{
			Swap(TimeA, TimeB);
		}

		MinTime = FMath::Max(MinTime, TimeA);
		MaxTime = FMath::Min(MaxTime, TimeB);
		if (MinTime > MaxTime)
		{
			return false;
		}
	}

	return true;
}
