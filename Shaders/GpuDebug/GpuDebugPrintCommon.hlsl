#ifndef DEBUG_PRINT_COMMON_HLSL
#define DEBUG_PRINT_COMMON_HLSL

static const uint kDebugPrintHeaderSize = 4;
static const uint kDebugPrintEntryStride = 16;
static const uint kDebugPrintMaxEntries = 4096;
static const uint kDebugPrintDefaultAdvance = 8;
static const uint kDebugPrintStatsCount = 32;
static const uint kDebugPrintStatsOccludedIndex = 1;
static const uint kDebugPrintStatsLateVisibleIndex = 4;
static const uint kDebugPrintStatsClusterDagVisibleIndex = 5;
static const uint kDebugPrintStatsClusterDagHwRasterIndex = 7;
static const uint kDebugPrintStatsClusterDagSwRasterIndex = 8;
static const uint kDebugPrintStatsClusterDagCulledIndex = 10;
static const uint kDebugPrintStatsClusterDagStackOverflowIndex = 23;
static const uint kDebugPrintStatsClusterDagExpandedOverflowIndex = 24;
static const uint kDebugPrintStatsClusterDagIterationOverflowIndex = 25;
static const uint kDebugPrintStatsClusterDagPersistentOverflowIndex = 29;
static const uint kDebugPrintStatsSparseSdfGITriangleIndex = 19;
static const uint kDebugPrintStatsSparseSdfGIReferenceIndex = 20;
static const uint kDebugPrintStatsSparseSdfGIOccupiedBrickIndex = 21;
static const uint kDebugPrintStatsSparseSdfGITriangleOverflowIndex = 22;
static const uint kDebugPrintStatsSparseSdfGIReferenceOverflowIndex = 30;

#endif // DEBUG_PRINT_COMMON_HLSL
