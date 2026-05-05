// ============================================================
// SystemStats/SystemStats.cpp
// CPU via PDH, RAM via GlobalMemoryStatusEx,
// GPU load via PDH GPU Engine (Intel iGPU compatible).
// No external tools required. Runs forever, auto-reconnects.
// ============================================================
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <cstdio>
#include <string>
#include "../Shared/SharedMemory.h"
#pragma comment(lib,"pdh.lib")

// ---- CPU ----
struct CpuMon {
    PDH_HQUERY q=nullptr; PDH_HCOUNTER c=nullptr; bool ok=false;
    bool Init(){
        if(::PdhOpenQueryW(nullptr,0,&q)!=ERROR_SUCCESS) return false;
        if(::PdhAddEnglishCounterW(q,
            L"\\Processor(_Total)\\% Processor Time",0,&c)!=ERROR_SUCCESS) return false;
        ::PdhCollectQueryData(q); ok=true; return true;
    }
    float Get(){
        if(!ok) return 0.f;
        ::PdhCollectQueryData(q);
        PDH_FMT_COUNTERVALUE v{};
        ::PdhGetFormattedCounterValue(c,PDH_FMT_DOUBLE,nullptr,&v);
        return (float)v.doubleValue;
    }
    ~CpuMon(){if(q)::PdhCloseQuery(q);}
};

// ---- Intel iGPU load via PDH GPU Engine (Win10 1803+) ----
struct GpuMon {
    PDH_HQUERY q=nullptr; PDH_HCOUNTER c=nullptr; bool ok=false;
    bool Init(){
        if(::PdhOpenQueryW(nullptr,0,&q)!=ERROR_SUCCESS) return false;
        if(::PdhAddEnglishCounterW(q,
            L"\\GPU Engine(*)\\Utilization Percentage",0,&c)!=ERROR_SUCCESS){
            ::PdhCloseQuery(q);q=nullptr;return false;
        }
        ::PdhCollectQueryData(q); ok=true; return true;
    }
    float Get(){
        if(!ok) return -1.f;
        ::PdhCollectQueryData(q);
        DWORD sz=0,n=0;
        ::PdhGetFormattedCounterArrayW(c,PDH_FMT_DOUBLE,&sz,&n,nullptr);
        if(!sz||!n) return -1.f;
        auto* arr=(PDH_FMT_COUNTERVALUE_ITEM_W*)new BYTE[sz];
        float sum=0.f; DWORD cnt=0;
        if(::PdhGetFormattedCounterArrayW(c,PDH_FMT_DOUBLE,&sz,&n,arr)==ERROR_SUCCESS){
            for(DWORD i=0;i<n;i++){
                std::wstring nm(arr[i].szName);
                if(nm.find(L"engtype_3D")!=std::wstring::npos||
                   nm.find(L"engtype_Graphics")!=std::wstring::npos){
                    sum+=(float)arr[i].FmtValue.doubleValue; cnt++;
                }
            }
        }
        delete[](BYTE*)arr;
        if(!cnt) return -1.f;
        float avg=sum/(float)cnt;
        return avg>100.f?100.f:avg;
    }
    ~GpuMon(){if(q)::PdhCloseQuery(q);}
};

// ---- RAM ----
static void GetRam(float& used, float& total){
    MEMORYSTATUSEX ms{sizeof(ms)};
    ::GlobalMemoryStatusEx(&ms);
    total=(float)ms.ullTotalPhys/1048576.f;
    used=total-(float)ms.ullAvailPhys/1048576.f;
}

// ================================================================
// main — never exits, auto-reconnects
// ================================================================
int wmain(){
    ::SetConsoleTitleW(L"RTSS Clone - SystemStats (keep open)");
    wprintf(L"\n  RTSS Clone SystemStats\n");
    wprintf(L"  Keep this window open while gaming.\n");
    wprintf(L"  Will auto-connect when you inject.\n\n");

    CpuMon cpu; cpu.Init();
    GpuMon gpu; bool gpuOk=gpu.Init();
    wprintf(L"  CPU PDH: OK\n");
    wprintf(L"  GPU PDH: %s\n\n", gpuOk?L"OK":L"FAILED (Win10 1803+ required)");
    wprintf(L"  Note: GPU temperature not available on Intel HD\n");
    wprintf(L"        without third-party tools.\n\n");

    SharedMemHandle shm;

    while(true)
    {
        // Wait for HookDLL to create shared memory
        if(!shm.Valid()){
            wprintf(L"\r  Waiting for injection...         ");
            while(!shm.OpenMapping()) ::Sleep(500);
            wprintf(L"\r  Connected! Monitoring...         \n");
        }

        RtssStats* s=shm.Data();
        if(!s||s->version!=RTSS_SHARED_VERSION){
            shm.Close();
            wprintf(L"\r  Game closed, waiting...          \n");
            ::Sleep(1000); continue;
        }

        // CPU
        s->cpuUsagePercent=cpu.Get();

        // RAM
        float ru=0,rt=0; GetRam(ru,rt);
        s->ramUsedMB=ru; s->ramTotalMB=rt;

        // GPU (Intel iGPU via PDH)
        if(gpuOk){
            s->gpuUsagePercent=gpu.Get();
            s->gpuMemUsedMB=ru;  // iGPU shares system RAM
        } else {
            s->gpuUsagePercent=-1.f;
            s->gpuMemUsedMB=-1.f;
        }
        s->gpuTempC=-1.f; // not available on Intel HD without external tools

        wprintf(L"\r  FPS %-5.0f  CPU %-4.0f%%  RAM %-5.0fMB  GPU %-4.0f%%   ",
            s->fps, s->cpuUsagePercent, s->ramUsedMB, s->gpuUsagePercent);

        ::Sleep(500);
    }
    return 0;
}
