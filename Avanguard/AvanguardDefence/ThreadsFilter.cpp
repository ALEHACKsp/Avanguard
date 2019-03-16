#include "AvnDefinitions.h"
#ifdef FEATURE_THREADS_FILTER

#define WIN32_LEAN_AND_MEAN

#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS

#include <winternl.h>
#include <ntstatus.h>

#include "NativeAPI.h"
#include "AvnGlobals.h"

#include "Locks.h"
#include <unordered_map>
#include <unordered_set>

#include <string>
#include "Logger.h"

#include <HookLib.h>

#ifdef FEATURE_DLL_FILTER
    #include "DllFilter.h"
#endif

#ifdef FEATURE_MEMORY_FILTER
    #include "MemoryFilter.h"
#endif

#include "ThreadsFilter.h"

namespace ThreadsFilter {

    class ThreadsStorage final {
    private:
        mutable RWLock Lock;
        std::unordered_map<LPCVOID, SIZE_T> Threads; // Thread -> Refcount
    public:
        ThreadsStorage(const ThreadsStorage&) = delete;
        ThreadsStorage(ThreadsStorage&&) = delete;
        ThreadsStorage& operator = (const ThreadsStorage&) = delete;
        ThreadsStorage& operator = (ThreadsStorage&&) = delete;

        ThreadsStorage() : Lock(), Threads() {}
        ~ThreadsStorage() = default;

        VOID Ref(LPCVOID Thread) noexcept {
            Lock.LockExclusive();
            auto Entry = Threads.find(Thread);
            if (Entry == Threads.end())
                Threads.emplace(Thread, 1);
            else
                ++Entry->second;
            Lock.UnlockExclusive();
        }

        BOOL Unref(LPCVOID Thread) noexcept {
            Lock.LockExclusive();
            auto Entry = Threads.find(Thread);
            BOOL Exists = Entry != Threads.end();
            if (Exists) {
                --Entry->second;
                if (!Entry->second) Threads.erase(Entry); // Refcount == 0
            }
            Lock.UnlockExclusive();
            return Exists;
        }

        VOID Clear() noexcept {
            Lock.LockExclusive();
            Threads.clear();
            Lock.UnlockExclusive();
        }
    };

    class ThreadPoolsStorage final {
    private:
        mutable RWLock Lock;
        std::unordered_set<LPCVOID> TppWorkerThreads;
    public:
        ThreadPoolsStorage(const ThreadPoolsStorage&) = delete;
        ThreadPoolsStorage(ThreadPoolsStorage&&) = delete;
        ThreadPoolsStorage& operator = (const ThreadPoolsStorage&) = delete;
        ThreadPoolsStorage& operator = (ThreadPoolsStorage&&) = delete;

        ThreadPoolsStorage() : Lock(), TppWorkerThreads() {}
        ~ThreadPoolsStorage() = default;

        VOID Add(LPCVOID ThreadPoolEntry) {
            Lock.LockExclusive();
            TppWorkerThreads.emplace(ThreadPoolEntry);
            Lock.UnlockExclusive();
        }

        BOOL Exists(LPCVOID ThreadPoolEntry) const {
            Lock.LockShared();
            BOOL IsExists = TppWorkerThreads.find(ThreadPoolEntry) != TppWorkerThreads.end();
            Lock.UnlockShared();
            return IsExists;
        }
    };

    static struct {
        ThreadsStorage Threads;
        ThreadPoolsStorage ThreadPools;
        ULONG Pid;
        volatile ULONG TppInitTid;
        PVOID pLdrInitializeThunk;
        PVOID pNtCreateThread;
        PVOID pNtCreateThreadEx;
        PVOID pNtCreateWorkerFactory;
        NTSTATUS(NTAPI*_TpAllocPool)(OUT PTP_POOL* PoolReturn, _Reserved_ PVOID Reserved);
        enum DENIED_EPs {
            epLdrLoadDll,
            epKBaseLoadLibraryA,
            epKBaseLoadLibraryW,
            epKBaseLoadLibraryExA,
            epKBaseLoadLibraryExW,
            epK32LoadLibraryA,
            epK32LoadLibraryW,
            epK32LoadLibraryExA,
            epK32LoadLibraryExW,
            epMax
        };
        LPCVOID DeniedEntryPoints[epMax];
        BOOL Enabled;
    } FilterData = {};

    static VOID InitializeDeniedEntryPoints()
    {
        FilterData.DeniedEntryPoints[FilterData.epLdrLoadDll] = _GetProcAddress(AvnGlobals.hModules.hNtdll, "LdrLoadDll");
        FilterData.DeniedEntryPoints[FilterData.epKBaseLoadLibraryA] = _GetProcAddress(AvnGlobals.hModules.hKernelBase, "LoadLibraryA");
        FilterData.DeniedEntryPoints[FilterData.epKBaseLoadLibraryW] = _GetProcAddress(AvnGlobals.hModules.hKernelBase, "LoadLibraryW");
        FilterData.DeniedEntryPoints[FilterData.epKBaseLoadLibraryExA] = _GetProcAddress(AvnGlobals.hModules.hKernelBase, "LoadLibraryExA");
        FilterData.DeniedEntryPoints[FilterData.epKBaseLoadLibraryExW] = _GetProcAddress(AvnGlobals.hModules.hKernelBase, "LoadLibraryExW");
        FilterData.DeniedEntryPoints[FilterData.epK32LoadLibraryA] = _GetProcAddress(AvnGlobals.hModules.hKernel32, "LoadLibraryA");
        FilterData.DeniedEntryPoints[FilterData.epK32LoadLibraryW] = _GetProcAddress(AvnGlobals.hModules.hKernel32, "LoadLibraryW");
        FilterData.DeniedEntryPoints[FilterData.epK32LoadLibraryExA] = _GetProcAddress(AvnGlobals.hModules.hKernel32, "LoadLibraryExA");
        FilterData.DeniedEntryPoints[FilterData.epK32LoadLibraryExW] = _GetProcAddress(AvnGlobals.hModules.hKernel32, "LoadLibraryExW");
    }

    static BOOL IsUnknownThreadAllowed(PVOID EntryPoint)
    {
        HMODULE ImageBase = NULL;
        RtlPcToFileHeader(EntryPoint, reinterpret_cast<PVOID*>(&ImageBase));
        if (ImageBase) {
            if (ImageBase != AvnGlobals.hModules.hNtdll
                && ImageBase != AvnGlobals.hModules.hKernel32
                && ImageBase != AvnGlobals.hModules.hKernelBase
            ) {
                return FALSE;
            }

            for (LPCVOID DeniedEp : FilterData.DeniedEntryPoints) {
                if (EntryPoint == DeniedEp) return FALSE;
            }

            return TRUE;
        }
        return FALSE; // Unable to obtain a module -> Dynamic memory -> Denied
    }

    DeclareHook(VOID, NTAPI, LdrInitializeThunk, PCONTEXT Context)
    {
#ifdef _AMD64_
        /*
            RCX = EntryPoint
            RDX = Argument
        */
        PVOID EntryPoint = (PVOID)Context->Rcx;
#else
        /*
            EAX = EntryPoint
            EBX = Argument
        */
        PVOID EntryPoint = (PVOID)Context->Eax;
#endif

        if (FilterData.ThreadPools.Exists(EntryPoint)) {
            Log(L"[v] Thread " + std::to_wstring(__pid()) + L" is a thread pool worker, allowed");
            return CallOriginal(LdrInitializeThunk)(Context);
        }

        BOOL IsKnownThread = FilterData.Threads.Unref(EntryPoint);

        if (!IsKnownThread) {
            if (IsUnknownThreadAllowed(EntryPoint)) {
                Log(L"[v] Thread " + std::to_wstring(__pid()) + L" has an unknown origin, but allowed EntryPoint");
            }
            else {
                Log(L"[x] Thread " + std::to_wstring(__tid()) + L" has an unknown origin and blocked");
                NtTerminateThread(NtCurrentThread(), 0);
            }
        }
        
#ifdef FEATURE_DLL_FILTER
        // Check an origin module of the EntryPoint:
        if (!DllFilter::IsAddressInKnownModule(EntryPoint)) {
#ifdef FEATURE_MEMORY_FILTER
            if (!MemoryFilter::IsMemoryKnown(EntryPoint)) {
                Log(L"[x] Entry point of thread " + std::to_wstring(__tid()) + L" is in unknown memory, thread is blocked");
                NtTerminateThread(NtCurrentThread(), 0);
            }
#else
            Log(L"[x] Entry point of thread " + std::to_wstring(__tid()) + L" is in unknown module, thread is blocked");
            NtTerminateThread(NtCurrentThread(), 0);
#endif
        } 
#elif defined FEATURE_MEMORY_FILTER
        if (!MemoryChecked && !MemoryFilter::IsMemoryKnown(EntryPoint)) {
            Log(L"[x] Entry point of thread " + std::to_wstring(__tid()) + L" is in unknown memory, thread is blocked");
            NtTerminateThread(NtCurrentThread(), 0);
        }
#endif

        CallOriginal(LdrInitializeThunk)(Context);
    }

    typedef struct _INITIAL_TEB {
        PVOID StackBase;
        PVOID StackLimit;
        PVOID StackCommit;
        PVOID StackCommitMax;
        PVOID StackReserved;
    } INITIAL_TEB, * PINITIAL_TEB;

    DeclareHook(
        NTSTATUS, NTAPI, NtCreateThread,
        OUT PHANDLE ThreadHandle,
        IN ACCESS_MASK DesiredAccess,
        IN OPTIONAL POBJECT_ATTRIBUTES ObjectAttributes,
        IN HANDLE ProcessHandle,
        OUT CLIENT_ID* ClientId,
        IN PCONTEXT ThreadContext,
        IN PINITIAL_TEB InitialTeb,
        IN BOOLEAN CreateSuspended
    ) {
#ifdef _AMD64_
        LPCVOID EntryPoint = reinterpret_cast<PVOID>(ThreadContext->Rcx);
#else
        LPCVOID EntryPoint = reinterpret_cast<PVOID>(ThreadContext->Ecx);
#endif

        FilterData.Threads.Ref(EntryPoint);

        NTSTATUS Status = CallOriginal(NtCreateThread)(
            ThreadHandle,
            DesiredAccess,
            ObjectAttributes,
            ProcessHandle,
            ClientId,
            ThreadContext,
            InitialTeb,
            CreateSuspended
        );

        if (!NT_SUCCESS(Status))
            FilterData.Threads.Unref(EntryPoint);

        return Status;
    }

    DeclareHook(
        NTSTATUS, NTAPI, NtCreateThreadEx,
        OUT PHANDLE ThreadHandle,
        IN ACCESS_MASK DesiredAccess,
        IN POBJECT_ATTRIBUTES ObjectAttributes,
        IN HANDLE ProcessHandle,
        IN LPTHREAD_START_ROUTINE lpStartAddress,
        IN LPVOID lpParameter,
        IN BOOL CreateSuspended,
        IN SIZE_T StackZeroBits,
        IN SIZE_T SizeOfStackCommit,
        IN SIZE_T SizeOfStackReserve,
        IN OPTIONAL PCONTEXT ProcessContext
    ) {
        HANDLE hThread = NULL;
        BOOL NeedToFilter = ProcessHandle == NtCurrentProcess() || GetProcessId(ProcessHandle) == FilterData.Pid;

        FilterData.Threads.Ref(lpStartAddress);
        
        NTSTATUS Status = CallOriginal(NtCreateThreadEx)(
            &hThread,
            DesiredAccess,
            ObjectAttributes,
            ProcessHandle,
            lpStartAddress,
            lpParameter,
            CreateSuspended,
            StackZeroBits,
            SizeOfStackCommit,
            SizeOfStackReserve,
            ProcessContext
        );
        
        if (!NT_SUCCESS(Status))
            FilterData.Threads.Unref(lpStartAddress);

        if (ThreadHandle)
            *ThreadHandle = hThread;

        return Status;
    }

    DeclareHook(
        NTSTATUS, NTAPI, NtCreateWorkerFactory,
        OUT PHANDLE WorkerFactoryHandleReturn,
        IN ACCESS_MASK DesiredAccess,
        IN OPTIONAL POBJECT_ATTRIBUTES ObjectAttributes,
        IN HANDLE CompletionPortHandle,
        IN HANDLE WorkerProcessHandle,
        IN PVOID StartRoutine,
        IN OPTIONAL PVOID StartParameter,
        IN OPTIONAL ULONG MaxThreadCount,
        IN OPTIONAL SIZE_T StackReserve,
        IN OPTIONAL SIZE_T StackCommit
    ) {
        FilterData.ThreadPools.Add(StartRoutine);
        if (__tid() == FilterData.TppInitTid) {
            FilterData.TppInitTid = 0;
            return STATUS_UNSUCCESSFUL;
        }

        return CallOriginal(NtCreateWorkerFactory)(
            WorkerFactoryHandleReturn,
            DesiredAccess,
            ObjectAttributes,
            CompletionPortHandle,
            WorkerProcessHandle,
            StartRoutine,
            StartParameter,
            MaxThreadCount,
            StackReserve,
            StackCommit
        );
    }

    BOOL EnableThreadsFilter()
    {
        if (FilterData.Enabled) return TRUE;

        if (!FilterData.pLdrInitializeThunk)
            FilterData.pLdrInitializeThunk = _GetProcAddress(AvnGlobals.hModules.hNtdll, "LdrInitializeThunk");

        if (!FilterData.pNtCreateThread)
            FilterData.pNtCreateThread = _GetProcAddress(AvnGlobals.hModules.hNtdll, "NtCreateThread");

        if (!FilterData.pLdrInitializeThunk || !FilterData.pNtCreateThread)
            return FALSE; // NtCreateThreadEx and others are optional, but LdrInitializeThunk and NtCreateThread are required!

        if (!FilterData.pNtCreateThreadEx)
            FilterData.pNtCreateThreadEx = _GetProcAddress(AvnGlobals.hModules.hNtdll, "NtCreateThreadEx");

        if (!FilterData.pNtCreateWorkerFactory)
            FilterData.pNtCreateWorkerFactory = _GetProcAddress(AvnGlobals.hModules.hNtdll, "NtCreateWorkerFactory");

        if (!FilterData._TpAllocPool)
            FilterData._TpAllocPool = reinterpret_cast<decltype(FilterData._TpAllocPool)>(_GetProcAddress(AvnGlobals.hModules.hNtdll, "TpAllocPool"));

        InitializeDeniedEntryPoints();
        FilterData.Pid = __pid();
        FilterData.TppInitTid = __tid();

        SetHookTarget(LdrInitializeThunk, FilterData.pLdrInitializeThunk);
        SetHookTarget(NtCreateThread, FilterData.pNtCreateThread);
        if (FilterData.pNtCreateThreadEx)
            SetHookTarget(NtCreateThreadEx, FilterData.pNtCreateThreadEx);

        if (FilterData.pNtCreateWorkerFactory)
            SetHookTarget(NtCreateWorkerFactory, FilterData.pNtCreateWorkerFactory);

        BOOL Status = EnableHook(LdrInitializeThunk) && EnableHook(NtCreateThread);
        if (FilterData.pNtCreateThreadEx)
            Status &= EnableHook(NtCreateThreadEx);

        if (FilterData.pNtCreateWorkerFactory)
            Status &= EnableHook(NtCreateWorkerFactory);

        if (Status && FilterData.pNtCreateWorkerFactory && FilterData._TpAllocPool) {
            PTP_POOL TpPool = NULL;
            FilterData._TpAllocPool(&TpPool, NULL); // TppWorkerThread address initialization
        }

        FilterData.Enabled = TRUE;

        if (!Status)
            DisableThreadsFilter();

        return FilterData.Enabled;
    }

    VOID DisableThreadsFilter()
    {
        if (!FilterData.Enabled) return;

        if (FilterData.pNtCreateThreadEx)
            DisableHook(NtCreateThreadEx);
        DisableHook(NtCreateThread);
        DisableHook(NtCreateWorkerFactory);
        DisableHook(LdrInitializeThunk);

        FilterData.Threads.Clear();
        FilterData.Enabled = FALSE;
    }
}
#endif