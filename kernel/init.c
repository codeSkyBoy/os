/*++

Copyright (c) 2012 Minoca Corp. All Rights Reserved

Module Name:

    init.c

Abstract:

    This module implements the kernel system startup.

Author:

    Evan Green 2-Jul-2012

Environment:

    Kernel Initialization

--*/

//
// ------------------------------------------------------------------- Includes
//

#include <minoca/kernel.h>
#include <minoca/bconflib.h>
#include <minoca/bootload.h>
#include <minoca/basevid.h>

//
// ---------------------------------------------------------------- Definitions
//

//
// ------------------------------------------------------ Data Type Definitions
//

typedef enum _KERNEL_SUBSYSTEM {
    KernelSubsystemInvalid,
    KernelSubsystemKernelDebugger,
    KernelSubsystemKernelExecutive,
    KernelSubsystemMemoryManager,
    KernelSubsystemObjectManager,
    KernelSubsystemAcpi,
    KernelSubsystemHardwareLayer,
    KernelSubsystemProcess,
    KernelSubsystemInputOutput,
    KernelSubsystemProfiler
} KERNEL_SUBSYSTEM, *PKERNEL_SUBSYSTEM;

typedef struct _SYSTEM_USAGE_CONTEXT {
    ULONGLONG TimeCounter;
    ULONGLONG TimeCounterFrequency;
    ULONGLONG CycleCounterFrequency;
    ULONGLONG UserCycles;
    ULONGLONG KernelCycles;
    ULONGLONG InterruptCycles;
    ULONGLONG IdleCycles;
    ULONGLONG TotalCycles;
    ULONG UserPercent;
    ULONG KernelPercent;
    ULONG InterruptPercent;
    ULONG IdlePercent;
} SYSTEM_USAGE_CONTEXT, *PSYSTEM_USAGE_CONTEXT;

//
// ----------------------------------------------- Internal Function Prototypes
//

VOID
KepApplicationProcessorStartup (
    PPROCESSOR_START_BLOCK StartBlock
    );

VOID
KepCompleteSystemInitialization (
    PVOID Parameter
    );

VOID
KepAcquireProcessorStartLock (
    VOID
    );

VOID
KepReleaseProcessorStartLock (
    VOID
    );

VOID
KepBannerThread (
    PVOID Context
    );

VOID
KepUpdateSystemUsage (
    PSYSTEM_USAGE_CONTEXT Context
    );

VOID
KepPrintFormattedMemoryUsage (
    PCHAR String,
    ULONG StringSize,
    ULONGLONG UsedValue,
    ULONGLONG TotalValue,
    PULONG Offset
    );

VOID
KepPrintFormattedSize (
    PCHAR String,
    ULONG StringSize,
    ULONGLONG Value,
    PULONG Offset
    );

VOID
KepPrintFormattedPercent (
    PCHAR String,
    ULONG StringSize,
    ULONG PercentTimesTen,
    PULONG Offset
    );

VOID
KepQueueDistributionTimer (
    VOID
    );

VOID
KepDistributionTimerDpcRoutine (
    PDPC Dpc
    );

VOID
KepDistributionTimerWorkRoutine (
    PVOID Parameter
    );

//
// -------------------------------------------------------------------- Globals
//

//
// Define an override that limits the system to one processor.
//

BOOL KeRunSingleProcessor = FALSE;

//
// Define a lock used to serializes parts of the AP startup execution.
//

volatile ULONG KeProcessorStartLock = 0;
volatile ULONG KeProcessorsReady = 0;
volatile BOOL KeAllProcessorsInitialize = FALSE;
volatile BOOL KeAllProcessorsGo = FALSE;

//
// ------------------------------------------------------------------ Functions
//

VOID
KepStartSystem (
    PKERNEL_INITIALIZATION_BLOCK Parameters
    )

/*++

Routine Description:

    This routine implements the first function called in the kernel from the
    boot loader.

Arguments:

    Parameters - Supplies information about the system and memory layout as
        set up by the loader.

Return Value:

    This function does not return.

--*/

{

    PBOOT_ENTRY BootEntry;
    PDEBUG_DEVICE_DESCRIPTION DebugDevice;
    KERNEL_SUBSYSTEM FailingSubsystem;
    ULONG ProcessorCount;
    KSTATUS Status;

    DebugDevice = NULL;
    FailingSubsystem = KernelSubsystemInvalid;

    //
    // Perform very basic processor initialization, preparing it to take
    // exceptions and use the serial port.
    //

    ArInitializeProcessor(FALSE, NULL);
    AcpiInitializePreDebugger(Parameters);
    Status = MmInitialize(Parameters, NULL, 0);
    if (!KSUCCESS(Status)) {
        FailingSubsystem = KernelSubsystemMemoryManager;
        goto StartSystemEnd;
    }

    HlInitializePreDebugger(Parameters, 0, &DebugDevice);

    //
    // Initialize the debugging subsystem.
    //

    BootEntry = Parameters->BootEntry;
    if ((BootEntry != NULL) &&
        ((BootEntry->Flags & BOOT_ENTRY_FLAG_DEBUG) != 0)) {

        Status = KdInitialize(DebugDevice, Parameters->KernelModule);
        if (!KSUCCESS(Status)) {
            FailingSubsystem = KernelSubsystemKernelDebugger;
            goto StartSystemEnd;
        }
    }

    //
    // Initialize the kernel executive.
    //

    Status = KeInitialize(0, Parameters);
    if (!KSUCCESS(Status)) {
        FailingSubsystem = KernelSubsystemKernelExecutive;
        goto StartSystemEnd;
    }

    //
    // Perform phase 1 MM Initialization.
    //

    Status = MmInitialize(Parameters, NULL, 1);
    if (!KSUCCESS(Status)) {
        FailingSubsystem = KernelSubsystemMemoryManager;
        goto StartSystemEnd;
    }

    //
    // Initialize the Object Manager.
    //

    Status = ObInitialize();
    if (!KSUCCESS(Status)) {
        FailingSubsystem = KernelSubsystemObjectManager;
        goto StartSystemEnd;
    }

    //
    // Perform phase 1 executive initialization, which sets up primitives like
    // queued locks and events.
    //

    Status = KeInitialize(1, Parameters);
    if (!KSUCCESS(Status)) {
        FailingSubsystem = KernelSubsystemKernelExecutive;
        goto StartSystemEnd;
    }

    //
    // Initialize ACPI.
    //

    Status = AcpiInitialize(Parameters);
    if (!KSUCCESS(Status)) {
        FailingSubsystem = KernelSubsystemAcpi;
        goto StartSystemEnd;
    }

    //
    // Initialize the hardware layer.
    //

    Status = HlInitialize(Parameters, 0);
    if (!KSUCCESS(Status)) {
        FailingSubsystem = KernelSubsystemHardwareLayer;
        goto StartSystemEnd;
    }

    //
    // Initialize the process and thread subsystem.
    //

    Status = PsInitialize(0,
                          Parameters,
                          Parameters->KernelStack.Buffer,
                          Parameters->KernelStack.Size);

    if (!KSUCCESS(Status)) {
        FailingSubsystem = KernelSubsystemProcess;
        goto StartSystemEnd;
    }

    //
    // Perform phase 1 hardware layer initialization. The scheduler becomes
    // active at this point.
    //

    Status = HlInitialize(Parameters, 1);
    if (!KSUCCESS(Status)) {
        FailingSubsystem = KernelSubsystemHardwareLayer;
        goto StartSystemEnd;
    }

    //
    // Now that the system is multithreaded, lock down MM.
    //

    Status = MmInitialize(Parameters, NULL, 2);
    if (!KSUCCESS(Status)) {
        FailingSubsystem = KernelSubsystemMemoryManager;
        goto StartSystemEnd;
    }

    //
    // Perform additional process initialization now that MM is fully up.
    //

    Status = PsInitialize(1, Parameters, NULL, 0);
    if (!KSUCCESS(Status)) {
        FailingSubsystem = KernelSubsystemProcess;
        goto StartSystemEnd;
    }

    //
    // Start all processors. Wait for all processors to initialize before
    // allowing the debugger to start broadcasting NMIs.
    //

    if (KeRunSingleProcessor == FALSE) {
        Status = HlStartAllProcessors(KepApplicationProcessorStartup,
                                      &ProcessorCount);

        if (!KSUCCESS(Status)) {
            FailingSubsystem = KernelSubsystemHardwareLayer;
            goto StartSystemEnd;
        }

    } else {
        ProcessorCount = 1;
    }

    KeAllProcessorsInitialize = TRUE;
    RtlAtomicAdd32(&KeProcessorsReady, 1);
    while (KeProcessorsReady != ProcessorCount) {
        ArProcessorYield();
    }

    KdEnableNmiBroadcast(TRUE);

    //
    // Perform phase 2 executive initialization, which creates things like the
    // worker threads.
    //

    Status = KeInitialize(2, Parameters);
    if (!KSUCCESS(Status)) {
        FailingSubsystem = KernelSubsystemKernelExecutive;
        goto StartSystemEnd;
    }

    //
    // Initialize the system profiler subsystem, which will start profiling
    // only if early profiling is enabled.
    //

    Status = SpInitializeProfiler();
    if (!KSUCCESS(Status)) {
        FailingSubsystem = KernelSubsystemProfiler;
        goto StartSystemEnd;
    }

    //
    // Create a thread to continue system initialization that may involve
    // blocking, letting this thread become the idle thread. After this point,
    // the idle thread really is the idle thread.
    //

    Status = PsCreateKernelThread(KepCompleteSystemInitialization,
                                  Parameters,
                                  "Init");

    if (!KSUCCESS(Status)) {
        goto StartSystemEnd;
    }

    //
    // Boot mappings will be freed by the thread just kicked off, so the
    // parameters are now untouchable.
    //

    Parameters = NULL;

StartSystemEnd:
    if (!KSUCCESS(Status)) {
        VidPrintString(0, 14, "Kernel Failure: 0x");
        VidPrintHexInteger(18, 14, Status);
        VidPrintString(0, 15, "Subsystem: ");
        VidPrintInteger(11, 15, FailingSubsystem);
        KeCrashSystem(CRASH_SYSTEM_INITIALIZATION_FAILURE,
                      FailingSubsystem,
                      Status,
                      0,
                      0);
    }

    //
    // Drop into the idle loop.
    //

    KeIdleLoop();
    return;
}

VOID
KepApplicationProcessorStartup (
    PPROCESSOR_START_BLOCK StartBlock
    )

/*++

Routine Description:

    This routine implements the main initialization routine for processors
    other than P0.

Arguments:

    StartBlock - Supplies a pointer to the processor start block that contains
        this processor's initialization information.

Return Value:

    This function does not return, this thread eventually becomes the idle
    thread.

--*/

{

    KSTATUS Status;

    //
    // Wait here until P0 says it's okay to initialize. This barrier allows
    // all processors to get out of the stub code as quickly as possible and
    // not have to worry about contending for non-paged pool locks while
    // allocating an idle stack.
    //

    while (KeAllProcessorsInitialize == FALSE) {
        ArProcessorYield();
    }

    KepAcquireProcessorStartLock();
    ArInitializeProcessor(FALSE, StartBlock->ProcessorStructures);

    //
    // Initialize the kernel executive.
    //

    Status = KeInitialize(0, NULL);
    if (!KSUCCESS(Status)) {
        goto ApplicationProcessorStartupEnd;
    }

    //
    // Perform phase 1 MM Initialization.
    //

    Status = MmInitialize(NULL, StartBlock, 1);
    if (!KSUCCESS(Status)) {
        goto ApplicationProcessorStartupEnd;
    }

    //
    // Perform phase 1 executive initialization.
    //

    Status = KeInitialize(1, NULL);
    if (!KSUCCESS(Status)) {
        goto ApplicationProcessorStartupEnd;
    }

    //
    // Initialize the hardware layer. The clock interrupt becomes active at
    // this point. As a result, this routine raises the run level from low to
    // dispatch to prevent the scheduler from becoming active before the
    // process and thread subsystem is initialized.
    //

    Status = HlInitialize(NULL, 0);
    if (!KSUCCESS(Status)) {
        goto ApplicationProcessorStartupEnd;
    }

    //
    // Initialize the process and thread subsystem.
    //

    Status = PsInitialize(0,
                          NULL,
                          StartBlock->StackBase,
                          StartBlock->StackSize);

    if (!KSUCCESS(Status)) {
        goto ApplicationProcessorStartupEnd;
    }

    //
    // Perform phase 1 hardware layer initialization.
    //

    Status = HlInitialize(NULL, 1);
    if (!KSUCCESS(Status)) {
        goto ApplicationProcessorStartupEnd;
    }

ApplicationProcessorStartupEnd:
    KeFreeProcessorStartBlock(StartBlock, FALSE);
    KepReleaseProcessorStartLock();
    StartBlock = NULL;

    //
    // On failure, take the system down.
    //

    if (!KSUCCESS(Status)) {
        KeCrashSystem(CRASH_SYSTEM_INITIALIZATION_FAILURE,
                      KeGetCurrentProcessorNumber(),
                      Status,
                      0,
                      0);
    }

    //
    // Wait until all processors are ready, and drop down to low level.
    //

    RtlAtomicAdd32(&KeProcessorsReady, 1);
    while (KeAllProcessorsGo == FALSE) {
        ArProcessorYield();
    }

    KeLowerRunLevel(RunLevelLow);
    KeIdleLoop();
    return;
}

//
// --------------------------------------------------------- Internal Functions
//

VOID
KepCompleteSystemInitialization (
    PVOID Parameter
    )

/*++

Routine Description:

    This routine completes initial kernel startup. It is performed on a
    separate thread to allow the startup thread to mature into the idle thread
    before blocking work starts. There is no guarantee that this routine will
    be executed exclusively on any one processor, the scheduler and all
    processors are active at this point.

Arguments:

    Parameter - Supplies information about the system and memory layout as
        set up by the loader, the kernel initialization block.

Return Value:

    None.

--*/

{

    KERNEL_SUBSYSTEM FailingSubsystem;
    PKERNEL_INITIALIZATION_BLOCK Parameters;
    KSTATUS Status;

    FailingSubsystem = KernelSubsystemInvalid;
    Parameters = (PKERNEL_INITIALIZATION_BLOCK)Parameter;

    //
    // Let all processors idle.
    //

    KeAllProcessorsGo = TRUE;

    //
    // Perform phase 0 initialization of the I/O subsystem, which will
    // initialize boot start drivers.
    //

    Status = IoInitialize(0, Parameters);
    if (!KSUCCESS(Status)) {
        FailingSubsystem = KernelSubsystemInputOutput;
        goto CompleteSystemInitializationEnd;
    }

    //
    // Perform phase 3 executive initialization, which signs up for entropy
    // interface notifications.
    //

    Status = KeInitialize(3, NULL);
    if (!KSUCCESS(Status)) {
        FailingSubsystem = KernelSubsystemKernelExecutive;
        goto CompleteSystemInitializationEnd;
    }

    //
    // Perform phase 4 initialization of the memory manager, which completes
    // initialization by freeing all boot allocations. From here on out, the
    // parameters pointer is inaccessible.
    //

    Status = MmInitialize(Parameters, NULL, 3);
    if (!KSUCCESS(Status)) {
        FailingSubsystem = KernelSubsystemMemoryManager;
        goto CompleteSystemInitializationEnd;
    }

    //
    // Fire up the banner thread.
    //

    PsCreateKernelThread(KepBannerThread, NULL, "KepBannerThread");

    //
    // Enable this for the free public builds.
    //

#if 0

    KepQueueDistributionTimer();

#endif

CompleteSystemInitializationEnd:
    if (!KSUCCESS(Status)) {
        VidPrintString(0, 24, "Failure: 0x");
        VidPrintHexInteger(11, 24, Status);
        KeCrashSystem(CRASH_SYSTEM_INITIALIZATION_FAILURE,
                      FailingSubsystem,
                      Status,
                      0,
                      0);
    }

    return;
}

VOID
KepAcquireProcessorStartLock (
    VOID
    )

/*++

Routine Description:

    This routine acquires the processor start lock.

Arguments:

    None.

Return Value:

    None.

--*/

{

    ULONG LockValue;

    while (TRUE) {
        LockValue = RtlAtomicCompareExchange32(&KeProcessorStartLock, 1, 0);
        if (LockValue == 0) {
            break;
        }

        ArProcessorYield();
    }

    return;
}

VOID
KepReleaseProcessorStartLock (
    VOID
    )

/*++

Routine Description:

    This routine releases the processor start lock.

Arguments:

    None.

Return Value:

    None.

--*/

{

    ULONG LockValue;

    LockValue = RtlAtomicExchange32(&KeProcessorStartLock, 0);

    //
    // Assert if the lock was not held.
    //

    ASSERT(LockValue != 0);

    return;
}

VOID
KepBannerThread (
    PVOID Context
    )

/*++

Routine Description:

    This routine prints an updated banner at the top of the screen.

Arguments:

    Context - Supplies an unused context pointer.

Return Value:

    None.

--*/

{

    CHAR BannerString[120];
    IO_CACHE_STATISTICS Cache;
    ULONGLONG Days;
    ULONGLONG Frequency;
    ULONGLONG Hours;
    IO_GLOBAL_STATISTICS IoStatistics;
    MM_STATISTICS Memory;
    ULONGLONG Minutes;
    ULONG Offset;
    ULONG PageSize;
    IO_GLOBAL_STATISTICS PreviousIoStatistics;
    ULONGLONG ReadDifference;
    ULONGLONG Seconds;
    ULONG Size;
    KSTATUS Status;
    ULONGLONG TimeCounter;
    SYSTEM_USAGE_CONTEXT UsageContext;
    UINTN UsedSize;
    ULONGLONG WriteDifference;

    Frequency = HlQueryTimeCounterFrequency();
    PageSize = MmPageSize();
    RtlZeroMemory(&Memory, sizeof(MM_STATISTICS));
    RtlZeroMemory(&Cache, sizeof(IO_CACHE_STATISTICS));
    RtlZeroMemory(&UsageContext, sizeof(SYSTEM_USAGE_CONTEXT));
    RtlZeroMemory(&PreviousIoStatistics, sizeof(IO_GLOBAL_STATISTICS));
    IoStatistics.Version = IO_GLOBAL_STATISTICS_VERSION;
    Memory.Version = MM_STATISTICS_VERSION;
    Cache.Version = IO_CACHE_STATISTICS_VERSION;
    while (TRUE) {
        Status = MmGetMemoryStatistics(&Memory);
        if (!KSUCCESS(Status)) {
            RtlDebugPrint("Failed to get MM statistics.\n");
            break;
        }

        Status = IoGetCacheStatistics(&Cache);
        if (!KSUCCESS(Status)) {
            RtlDebugPrint("Failed to get IO cache statistics.\n");
        }

        IoGetGlobalStatistics(&IoStatistics);
        TimeCounter = KeGetRecentTimeCounter();
        Seconds = TimeCounter / Frequency;
        Minutes = Seconds / 60;
        Seconds %= 60;
        Hours = Minutes / 60;
        Minutes %= 60;
        Days = Hours / 24;
        Hours %= 24;
        Offset = 0;
        Size = RtlStringCopy(BannerString + Offset,
                             "Memory Used/Total: ",
                             sizeof(BannerString) - Offset);

        if (Size != 0) {
            Offset += Size - 1;
        }

        KepPrintFormattedMemoryUsage(BannerString,
                                     sizeof(BannerString),
                                     Memory.AllocatedPhysicalPages * PageSize,
                                     Memory.PhysicalPages * PageSize,
                                     &Offset);

        Size = RtlStringCopy(BannerString + Offset,
                             "   Paged Pool: ",
                             sizeof(BannerString) - Offset);

        if (Size != 0) {
            Offset += Size - 1;
        }

        UsedSize = Memory.PagedPool.TotalHeapSize -
                   Memory.PagedPool.FreeListSize;

        KepPrintFormattedMemoryUsage(BannerString,
                                     sizeof(BannerString),
                                     UsedSize,
                                     Memory.PagedPool.TotalHeapSize,
                                     &Offset);

        Size = RtlStringCopy(BannerString + Offset,
                             "   Non-Paged Pool: ",
                             sizeof(BannerString) - Offset);

        if (Size != 0) {
            Offset += Size - 1;
        }

        UsedSize = Memory.NonPagedPool.TotalHeapSize -
                   Memory.NonPagedPool.FreeListSize;

        KepPrintFormattedMemoryUsage(BannerString,
                                     sizeof(BannerString),
                                     UsedSize,
                                     Memory.NonPagedPool.TotalHeapSize,
                                     &Offset);

        Size = RtlStringCopy(BannerString + Offset,
                             "   Cache: ",
                             sizeof(BannerString) - Offset);

        if (Size != 0) {
            Offset += Size - 1;
        }

        KepPrintFormattedMemoryUsage(BannerString,
                                     sizeof(BannerString),
                                     Cache.DirtyPageCount * PageSize,
                                     Cache.PhysicalPageCount * PageSize,
                                     &Offset);

        while (Offset < sizeof(BannerString) - 1) {
            BannerString[Offset] = ' ';
            Offset += 1;
        }

        BannerString[sizeof(BannerString) - 1] = '\0';
        VidPrintString(0, 0, BannerString);

        //
        // Also update the second line, which contains the system usage.
        //

        KepUpdateSystemUsage(&UsageContext);
        Offset = 0;
        Size = RtlStringCopy(BannerString,
                             "Uptime: ",
                             sizeof(BannerString) - Offset);

        Offset += Size - 1;
        if (Days == 0) {
            Size = RtlPrintToString(BannerString + Offset,
                                    sizeof(BannerString) - Offset,
                                    CharacterEncodingAscii,
                                    "%02I64d:%02I64d:%02I64d",
                                    Hours,
                                    Minutes,
                                    Seconds);

        } else {
            Size = RtlPrintToString(BannerString + Offset,
                                    sizeof(BannerString) - Offset,
                                    CharacterEncodingAscii,
                                    "%02I64d:%02I64d:%02I64d:%02I64d",
                                    Days,
                                    Hours,
                                    Minutes,
                                    Seconds);
        }

        if (Size != 0) {
            Offset += Size - 1;
        }

        Size = RtlStringCopy(BannerString + Offset,
                             "  CPU User: ",
                             sizeof(BannerString) - Offset);

        if (Size != 0) {
            Offset += Size - 1;
        }

        KepPrintFormattedPercent(BannerString,
                                 sizeof(BannerString),
                                 UsageContext.UserPercent,
                                 &Offset);

        Size = RtlStringCopy(BannerString + Offset,
                             "  Kernel: ",
                             sizeof(BannerString) - Offset);

        if (Size != 0) {
            Offset += Size - 1;
        }

        KepPrintFormattedPercent(BannerString,
                                 sizeof(BannerString),
                                 UsageContext.KernelPercent,
                                 &Offset);

        Size = RtlStringCopy(BannerString + Offset,
                             "  Interrupt: ",
                             sizeof(BannerString) - Offset);

        if (Size != 0) {
            Offset += Size - 1;
        }

        KepPrintFormattedPercent(BannerString,
                                 sizeof(BannerString),
                                 UsageContext.InterruptPercent,
                                 &Offset);

        Size = RtlStringCopy(BannerString + Offset,
                             "  Idle: ",
                             sizeof(BannerString) - Offset);

        if (Size != 0) {
            Offset += Size - 1;
        }

        KepPrintFormattedPercent(BannerString,
                                 sizeof(BannerString),
                                 UsageContext.IdlePercent,
                                 &Offset);

        ReadDifference = IoStatistics.BytesRead -
                         PreviousIoStatistics.BytesRead;

        WriteDifference = IoStatistics.BytesWritten -
                          PreviousIoStatistics.BytesWritten;

        Size = RtlStringCopy(BannerString + Offset,
                             "   IO: ",
                             sizeof(BannerString) - Offset);

        if (Size != 0) {
            Offset += Size - 1;
        }

        KepPrintFormattedMemoryUsage(BannerString,
                                     sizeof(BannerString),
                                     ReadDifference,
                                     WriteDifference,
                                     &Offset);

        ReadDifference = IoStatistics.PagingBytesRead -
                         PreviousIoStatistics.PagingBytesRead;

        WriteDifference = IoStatistics.PagingBytesWritten -
                          PreviousIoStatistics.PagingBytesWritten;

        if ((ReadDifference != 0) || (WriteDifference != 0)) {
            Size = RtlStringCopy(BannerString + Offset,
                                 "   Pg: ",
                                 sizeof(BannerString) - Offset);

            if (Size != 0) {
                Offset += Size - 1;
            }

            KepPrintFormattedMemoryUsage(BannerString,
                                         sizeof(BannerString),
                                         ReadDifference,
                                         WriteDifference,
                                         &Offset);
        }

        RtlCopyMemory(&PreviousIoStatistics,
                      &IoStatistics,
                      sizeof(IO_GLOBAL_STATISTICS));

        while (Offset < sizeof(BannerString) - 1) {
            BannerString[Offset] = ' ';
            Offset += 1;
        }

        BannerString[sizeof(BannerString) - 1] = '\0';
        VidPrintString(0, 1, BannerString);
        KeDelayExecution(TRUE, FALSE, MICROSECONDS_PER_SECOND);
    }

    return;
}

VOID
KepUpdateSystemUsage (
    PSYSTEM_USAGE_CONTEXT Context
    )

/*++

Routine Description:

    This routine updates the system usage information.

Arguments:

    Context - Supplies a pointer to the context information.

Return Value:

    None.

--*/

{

    PROCESSOR_CYCLE_ACCOUNTING Cycles;
    ULONGLONG DeltaTotal;
    ULONGLONG ExpectedTotalDelta;
    ULONGLONG IdleDelta;
    ULONGLONG InterruptDelta;
    ULONGLONG KernelDelta;
    ULONGLONG Ratio;
    ULONGLONG StoppedCycles;
    ULONGLONG TimeCounter;
    ULONGLONG TimeCounterDelta;
    ULONGLONG TotalCycles;
    ULONGLONG TotalDelta;
    ULONGLONG UserDelta;

    if (Context->TimeCounterFrequency == 0) {
        Context->TimeCounterFrequency = HlQueryTimeCounterFrequency();
    }

    if (Context->CycleCounterFrequency == 0) {
        Context->CycleCounterFrequency = HlQueryProcessorCounterFrequency();
    }

    //
    // Snap the time counter and cycle counters.
    //

    TimeCounter = HlQueryTimeCounter();
    KeGetTotalProcessorCycleAccounting(&Cycles);

    //
    // The cycle counter may not count while the processor is idle. Use the
    // time counter to figure out how many cycles there should have been, and
    // compare to how many there actually are. Any difference gets added to the
    // idle cycles.
    //

    TimeCounterDelta = TimeCounter - Context->TimeCounter;
    if (TimeCounterDelta == 0) {
        return;
    }

    Ratio = Context->CycleCounterFrequency * KeGetActiveProcessorCount() /
            Context->TimeCounterFrequency;

    ExpectedTotalDelta = TimeCounterDelta * Ratio;
    TotalCycles = Cycles.UserCycles + Cycles.KernelCycles +
                  Cycles.InterruptCycles + Cycles.IdleCycles;

    TotalDelta = TotalCycles - Context->TotalCycles;
    StoppedCycles = 0;
    if (ExpectedTotalDelta > TotalDelta) {
        StoppedCycles = ExpectedTotalDelta - TotalDelta;
    }

    //
    // Compute the differences between this time and last time.
    //

    UserDelta = Cycles.UserCycles - Context->UserCycles;
    KernelDelta = Cycles.KernelCycles - Context->KernelCycles;
    InterruptDelta = Cycles.InterruptCycles - Context->InterruptCycles;
    IdleDelta = Cycles.IdleCycles - Context->IdleCycles + StoppedCycles;
    DeltaTotal = UserDelta + KernelDelta + InterruptDelta + IdleDelta;

    //
    // Save this snapshot into the context as the new previous snapshot.
    //

    Context->TimeCounter = TimeCounter;
    Context->UserCycles = Cycles.UserCycles;
    Context->KernelCycles = Cycles.KernelCycles;
    Context->InterruptCycles = Cycles.InterruptCycles;
    Context->IdleCycles = Cycles.IdleCycles;
    Context->TotalCycles = TotalCycles;

    //
    // Finally, update the percent (times ten) values.
    //

    Context->UserPercent = UserDelta * 1000 / DeltaTotal;
    Context->KernelPercent = KernelDelta * 1000 / DeltaTotal;
    Context->InterruptPercent = InterruptDelta * 1000 / DeltaTotal;
    Context->IdlePercent = IdleDelta * 1000 / DeltaTotal;
    return;
}

VOID
KepPrintFormattedMemoryUsage (
    PCHAR String,
    ULONG StringSize,
    ULONGLONG UsedValue,
    ULONGLONG TotalValue,
    PULONG Offset
    )

/*++

Routine Description:

    This routine prints two formatted sizes a la 5.8M/64M.

Arguments:

    String - Supplies a pointer to the string buffer to print to.

    StringSize - Supplies the total size of the string buffer in bytes.

    UsedValue - Supplies the first value to print.

    TotalValue - Supplies the second value to print.

    Offset - Supplies a pointer that on input supplies the offset within the
        string to print. This value will be updated to the new end of the
        string.

Return Value:

    None.

--*/

{

    ULONG Size;

    KepPrintFormattedSize(String, StringSize, UsedValue, Offset);
    Size = RtlStringCopy(String + *Offset, "/", StringSize - *Offset);
    if (Size != 0) {
        *Offset += Size - 1;
    }

    KepPrintFormattedSize(String, StringSize, TotalValue, Offset);
    return;
}

VOID
KepPrintFormattedSize (
    PCHAR String,
    ULONG StringSize,
    ULONGLONG Value,
    PULONG Offset
    )

/*++

Routine Description:

    This routine prints a formatted size a la 5.8M (M for megabytes).

Arguments:

    String - Supplies a pointer to the string buffer to print to.

    StringSize - Supplies the total size of the string buffer in bytes.

    Value - Supplies the value in bytes to print.

    Offset - Supplies a pointer that on input supplies the offset within the
        string to print. This value will be updated to the new end of the
        string.

Return Value:

    None.

--*/

{

    ULONG Size;
    CHAR Suffix;

    Suffix = 'B';
    if (Value > 1024) {
        Suffix = 'K';
        Value = (Value * 10) / 1024;
        if (Value / 10 >= 1024) {
            Suffix = 'M';
            Value /= 1024;
            if (Value / 10 >= 1024) {
                Suffix = 'G';
                Value /= 1024;
            }
        }
    }

    ASSERT(Value < 1024 * 10);

    if (Suffix == 'B') {
        Size = RtlPrintToString(String + *Offset,
                                StringSize - *Offset,
                                CharacterEncodingAscii,
                                "%d",
                                (ULONG)Value);

    } else {
        if (Value < 100) {
            Size = RtlPrintToString(String + *Offset,
                                    StringSize - *Offset,
                                    CharacterEncodingAscii,
                                    "%d.%d%c",
                                    (ULONG)Value / 10,
                                    (ULONG)Value % 10,
                                    Suffix);

        } else {
            Size = RtlPrintToString(String + *Offset,
                                    StringSize - *Offset,
                                    CharacterEncodingAscii,
                                    "%d%c",
                                    (ULONG)Value / 10,
                                    Suffix);
        }
    }

    if (Size != 0) {
        *Offset += Size - 1;
    }

    return;
}

VOID
KepPrintFormattedPercent (
    PCHAR String,
    ULONG StringSize,
    ULONG PercentTimesTen,
    PULONG Offset
    )

/*++

Routine Description:

    This routine prints a formatted percentage a la 5.8% or 99%. The field
    width is always 4.

Arguments:

    String - Supplies a pointer to the string buffer to print to.

    StringSize - Supplies the total size of the string buffer in bytes.

    PercentTimesTen - Supplies ten times the percentage value. So 54.8% would
        have a value of 548. This value will be rounded to the precision that
        is printed.

    Offset - Supplies a pointer that on input supplies the offset within the
        string to print. This value will be updated to the new end of the
        string.

Return Value:

    None.

--*/

{

    ULONG Size;

    //
    // For values less than 10%, print the single digit and first decimal
    // point.
    //

    if (PercentTimesTen < 100) {
        Size = RtlPrintToString(String + *Offset,
                                StringSize - *Offset,
                                CharacterEncodingAscii,
                                "%d.%d%%",
                                PercentTimesTen / 10,
                                PercentTimesTen % 10);

    } else {
        PercentTimesTen += 5;
        Size = RtlPrintToString(String + *Offset,
                                StringSize - *Offset,
                                CharacterEncodingAscii,
                                "%3d%%",
                                PercentTimesTen / 10);
    }

    if (Size != 0) {
        *Offset += Size - 1;
    }

    return;
}

VOID
KepQueueDistributionTimer (
    VOID
    )

/*++

Routine Description:

    This routine queues the distribution timer.

Arguments:

    None.

Return Value:

    None.

--*/

{

    PDPC Dpc;
    ULONGLONG DueTime;
    ULONGLONG Interval;
    KSTATUS Status;
    PKTIMER Timer;

    Dpc = NULL;

    //
    // Create the reset timer that reboots the system every few days.
    //

    Timer = KeCreateTimer(MM_ALLOCATION_TAG);
    if (Timer == NULL) {
        goto QueueDistributionTimerEnd;
    }

    Dpc = KeCreateDpc(KepDistributionTimerDpcRoutine, Timer);
    if (Dpc == NULL) {
        goto QueueDistributionTimerEnd;
    }

    Interval = MICROSECONDS_PER_SECOND * SECONDS_PER_DAY * 3;
    DueTime = KeGetRecentTimeCounter() +
              KeConvertMicrosecondsToTimeTicks(Interval);

    Status = KeQueueTimer(Timer, TimerQueueSoftWake, DueTime, 0, 0, Dpc);
    if (!KSUCCESS(Status)) {
        goto QueueDistributionTimerEnd;
    }

    Timer = NULL;
    Dpc = NULL;

QueueDistributionTimerEnd:
    if (Timer != NULL) {
        KeDestroyTimer(Timer);
    }

    if (Dpc != NULL) {
        KeDestroyDpc(Dpc);
    }

    return;
}

VOID
KepDistributionTimerDpcRoutine (
    PDPC Dpc
    )

/*++

Routine Description:

    This routine implements the distribution DPC routine. It is called when the
    distribution timer fires.

Arguments:

    Dpc - Supplies a pointer to the DPC.

Return Value:

    None.

--*/

{

    KeCreateAndQueueWorkItem(NULL,
                             WorkPriorityNormal,
                             KepDistributionTimerWorkRoutine,
                             Dpc);

    return;
}

VOID
KepDistributionTimerWorkRoutine (
    PVOID Parameter
    )

/*++

Routine Description:

    This routine prototype represents a work item.

Arguments:

    Parameter - Supplies an optional parameter passed in by the creator of the
        work item.

Return Value:

    None.

--*/

{

    KSTATUS (*ActionRoutine)(SYSTEM_RESET_TYPE);
    PDPC Dpc;
    PKTIMER Timer;

    Dpc = Parameter;
    Timer = Dpc->UserData;
    KeDestroyTimer(Timer);
    KeDestroyDpc(Dpc);

    //
    // Reset the system. Be casually tricky by not just calling the routine
    // directly. Really it's not that tricky.
    //

    ActionRoutine = KeResetSystem;
    ActionRoutine(SystemResetShutdown);
    return;
}

