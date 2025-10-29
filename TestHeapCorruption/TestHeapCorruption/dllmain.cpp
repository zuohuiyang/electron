// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include "HeapCorruptionExports.h"

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}


extern "C" {

// 导出接口：触发堆溢出（使用 new[]/delete[]）
__declspec(dllexport) void TriggerHeapOverflow()
{
    const size_t size = 16;
    char* p = new char[size];

    // 故意越界写入 size+32 字节，触发堆溢出
    for (size_t i = 0; i < size + 32; ++i) {
        p[i] = 'A';
    }

    delete[] p;
}


// 导出接口：常见的 UAF（类对象场景）
__declspec(dllexport) void TriggerUAF()
{
    // 一个简单类，含缓冲区与长度字段
    struct Dummy {
        char buf[64];
        int  len;
        void fill(char c) {
            for (int i = 0; i < 64; ++i) buf[i] = c;
            len = 64;
        }
    };

    Dummy* obj = new Dummy();
    obj->fill('B');

    // 释放对象
    delete obj;

    // 释放后继续使用对象成员（写入），触发 UAF
    obj->len = 1;
    obj->buf[0] = 'C';
}

// 导出接口：双重释放（double free）
__declspec(dllexport) void TriggerDoubleFree()
{
    char* p = new char[128];
    delete[] p;
    // 再次释放同一块内存，触发 double free
    delete[] p;
}

} // extern "C"

