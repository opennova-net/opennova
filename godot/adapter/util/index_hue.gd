class_name IndexHue
extends RefCounted

## The golden-ratio hue walk every index-keyed debug/fallback color uses: the
## conjugate spreads consecutive indices maximally apart on the wheel. One
## precision for every consumer (three copies at three precisions predated it).
const PHI_CONJUGATE := 0.618033988749895


static func hue_for_index(index: int) -> float:
	return fposmod(float(index) * PHI_CONJUGATE, 1.0)
