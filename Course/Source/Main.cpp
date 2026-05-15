#include "CourseRunner.h"
#include "Courses/Course01_Reduction.h"
#include "Courses/Course02_PrefixScan.h"
#include "Courses/Course03_Enqueue.h"
#include "Courses/Course04_BottomUpTraversal.h"
#include "Courses/Course05_WaterfallScheme.h"
#include "Courses/Course06_PersistentThreads.h"
#include "Courses/Course07_PoolAllocator.h"
#include "Courses/Course08_LinearProbing.h"
#include "Courses/Course09_RadixSort.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <Windows.h>

namespace
{
    void EnsureWorkingDirectory()
    {
        namespace fs = std::filesystem;
        wchar_t ExePath[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, ExePath, MAX_PATH);
        fs::path ExeDir = fs::path(ExePath).parent_path();

        // If running from bin/, move up to the repo root.
        if (_wcsicmp(ExeDir.filename().c_str(), L"bin") == 0)
        {
            std::error_code Ec;
            fs::current_path(ExeDir.parent_path(), Ec);
        }
    }

    void PrintUsage()
    {
        printf("Usage: CourseTests [course]\n");
        printf("  course: 01..09  (run specific course)\n");
        printf("          all     (run all courses, default)\n");
        printf("          01-05   (run Phase 1 + 2)\n");
        printf("  Courses:\n");
        printf("    01 - Reduction           (block / wave)\n");
        printf("    02 - Prefix Scan         (Hillis-Steele / Blelloch, intra-block)\n");
        printf("    03 - Enqueue             (stream compaction variants)\n");
        printf("    04 - Bottom-Up Traversal (atomic second-arrival gate)\n");
        printf("    05 - Waterfall Scheme    (parallel BST builder)\n");
        printf("    06 - Persistent Threads  (⚠ global barrier – may TDR!)\n");
        printf("    07 - Pool Allocator      (warp-level stack pool)\n");
        printf("    08 - Linear Probing      (concurrent hash table)\n");
        printf("    09 - Radix Sort          (3-pass GPU sort)\n");
    }
}

int main(int argc, char* argv[])
{
    EnsureWorkingDirectory();

    const char* Selection = (argc > 1) ? argv[1] : "all";

    if (strcmp(Selection, "--help") == 0 || strcmp(Selection, "-h") == 0)
    {
        PrintUsage();
        return 0;
    }

    CourseRunner Runner;
    try
    {
        if (!Runner.Initialize())
        {
            fprintf(stderr, "Failed to initialize D3D12.\n");
            return 1;
        }
        Runner.PrintAdapterInfo();
    }
    catch (const std::exception& E)
    {
        fprintf(stderr, "Init error: %s\n", E.what());
        return 1;
    }

    auto Run = [&](int Course)
    {
        wchar_t CaptureName[128];
        swprintf_s(CaptureName, L"captures/Course%02d.wpix", Course);

        Runner.BeginCapture(CaptureName);
#ifdef _DEBUG
        PIXBeginEvent(PIX_COLOR(200, 180, 255), L"Course%02d", Course);
#endif
        try
        {
            switch (Course)
            {
            case 1:  Course01_Run(Runner); break;
            case 2:  Course02_Run(Runner); break;
            case 3:  Course03_Run(Runner); break;
            case 4:  Course04_Run(Runner); break;
            case 5:  Course05_Run(Runner); break;
            case 6:  Course06_Run(Runner); break;
            case 7:  Course07_Run(Runner); break;
            case 8:  Course08_Run(Runner); break;
            case 9:  Course09_Run(Runner); break;
            default: fprintf(stderr, "Unknown course: %d\n", Course); break;
            }
        }
        catch (const std::exception& E)
        {
            fprintf(stderr, "Course %d threw: %s\n", Course, E.what());
        }
#ifdef _DEBUG
        PIXEndEvent();
#endif
        Runner.EndCapture();
    };

    if (strcmp(Selection, "all") == 0)
    {
        for (int i = 1; i <= 9; ++i) Run(i);
    }
    else if (strlen(Selection) == 2 && Selection[0] >= '0' && Selection[0] <= '9')
    {
        Run(atoi(Selection));
    }
    else
    {
        int Course = atoi(Selection);
        if (Course >= 1 && Course <= 9) Run(Course);
        else { PrintUsage(); return 1; }
    }

    printf("\nDone.\n");
    return 0;
}
