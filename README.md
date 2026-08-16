# No Grass Under Buildings

Performance-focused rewrite for Satisfactory 1.2.4 (CL 502094) and SML 3.12.

The mod hides landscape grass, cliff grass, flowers, and other small decorative
foliage beneath foundations and buildings. Vegetation returns when the covering
buildable is dismantled.

This implementation uses targeted, event-driven exclusion updates. It does not
continuously rescan the world, permanently remove foliage, or emit recurring
per-instance diagnostic logs.
