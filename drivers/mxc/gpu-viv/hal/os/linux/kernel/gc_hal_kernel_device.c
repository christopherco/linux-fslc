/****************************************************************************
*
*    The MIT License (MIT)
*
*    Copyright (c) 2014 - 2017 Vivante Corporation
*
*    Permission is hereby granted, free of charge, to any person obtaining a
*    copy of this software and associated documentation files (the "Software"),
*    to deal in the Software without restriction, including without limitation
*    the rights to use, copy, modify, merge, publish, distribute, sublicense,
*    and/or sell copies of the Software, and to permit persons to whom the
*    Software is furnished to do so, subject to the following conditions:
*
*    The above copyright notice and this permission notice shall be included in
*    all copies or substantial portions of the Software.
*
*    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
*    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
*    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
*    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
*    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
*    FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
*    DEALINGS IN THE SOFTWARE.
*
*****************************************************************************
*
*    The GPL License (GPL)
*
*    Copyright (C) 2014 - 2017 Vivante Corporation
*
*    This program is free software; you can redistribute it and/or
*    modify it under the terms of the GNU General Public License
*    as published by the Free Software Foundation; either version 2
*    of the License, or (at your option) any later version.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.
*
*    You should have received a copy of the GNU General Public License
*    along with this program; if not, write to the Free Software Foundation,
*    Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*
*****************************************************************************
*
*    Note: This software is released under dual MIT and GPL licenses. A
*    recipient may use this file under the terms of either the MIT license or
*    GPL License. If you wish to use only one license not the other, you can
*    indicate your decision by deleting one of the above license notices in your
*    version of this file.
*
*****************************************************************************/


#include "gc_hal_kernel_linux.h"
#include <linux/pagemap.h>
#include <linux/seq_file.h>
#include <linux/mman.h>
#include <linux/slab.h>

#define _GC_OBJ_ZONE    gcvZONE_DEVICE

#define DEBUG_FILE          "galcore_trace"
#define PARENT_FILE         "gpu"

#define gcdDEBUG_FS_WARN    "Experimental debug entry, may be removed in future release, do NOT rely on it!\n"

#ifdef FLAREON
    static struct dove_gpio_irq_handler gc500_handle;
#endif

static gckGALDEVICE galDevice;

extern gcTA globalTA[16];

/******************************************************************************\
******************************** Debugfs Support *******************************
\******************************************************************************/

/******************************************************************************\
***************************** DEBUG SHOW FUNCTIONS *****************************
\******************************************************************************/

int gc_info_show(struct seq_file* m, void* data)
{
    gcsINFO_NODE *node = m->private;
    gckGALDEVICE device = node->device;
    int i = 0;
    gceCHIPMODEL chipModel;
    gctUINT32 chipRevision;
    gctUINT32 productID = 0;
    gctUINT32 ecoID = 0;

    for (i = 0; i < gcdMAX_GPU_COUNT; i++)
    {
        if (device->irqLines[i] != -1)
        {
#if gcdENABLE_VG
            if (i == gcvCORE_VG)
            {
                chipModel = device->kernels[i]->vg->hardware->chipModel;
                chipRevision = device->kernels[i]->vg->hardware->chipRevision;
            }
            else
#endif
            {
                chipModel = device->kernels[i]->hardware->identity.chipModel;
                chipRevision = device->kernels[i]->hardware->identity.chipRevision;
                productID = device->kernels[i]->hardware->identity.productID;
                ecoID = device->kernels[i]->hardware->identity.ecoID;
            }

            seq_printf(m, "gpu      : %d\n", i);
            seq_printf(m, "model    : %4x\n", chipModel);
            seq_printf(m, "revision : %4x\n", chipRevision);
            seq_printf(m, "product  : %4x\n", productID);
            seq_printf(m, "eco      : %4x\n", ecoID);
            seq_printf(m, "\n");
        }
    }

    return 0;
}

int gc_clients_show(struct seq_file* m, void* data)
{
    gcsINFO_NODE *node = m->private;
    gckGALDEVICE device = node->device;

    gckKERNEL kernel = _GetValidKernel(device);

    gcsDATABASE_PTR database;
    gctINT i, pid;
    char name[24];

    seq_printf(m, "%-8s%s\n", "PID", "NAME");
    seq_printf(m, "------------------------\n");

    /* Acquire the database mutex. */
    gcmkVERIFY_OK(
        gckOS_AcquireMutex(kernel->os, kernel->db->dbMutex, gcvINFINITE));

    /* Walk the databases. */
    for (i = 0; i < gcmCOUNTOF(kernel->db->db); ++i)
    {
        for (database = kernel->db->db[i];
             database != gcvNULL;
             database = database->next)
        {
            pid = database->processID;

            gcmkVERIFY_OK(gckOS_GetProcessNameByPid(pid, gcmSIZEOF(name), name));

            seq_printf(m, "%-8d%s\n", pid, name);
        }
    }

    /* Release the database mutex. */
    gcmkVERIFY_OK(gckOS_ReleaseMutex(kernel->os, kernel->db->dbMutex));

    /* Success. */
    return 0;
}

static void
_CounterAdd(
    gcsDATABASE_COUNTERS * Dest,
    gcsDATABASE_COUNTERS * Src
    )
{
    Dest->bytes += Src->bytes;
    Dest->maxBytes += Src->maxBytes;
    Dest->totalBytes += Src->totalBytes;
}

static void
_CounterPrint(
    gcsDATABASE_COUNTERS * Counter,
    gctCONST_STRING Name,
    struct seq_file* m
    )
{
    seq_printf(m, "    %s:\n", Name);
    seq_printf(m, "        Used  : %10llu B\n", Counter->bytes);
}

int gc_meminfo_show(struct seq_file* m, void* data)
{
    gcsINFO_NODE *node = m->private;
    gckGALDEVICE device = node->device;
    gckKERNEL kernel = _GetValidKernel(device);
    gckVIDMEM memory;
    gceSTATUS status;
    gcsDATABASE_PTR database;
    gctUINT32 i;

    gctUINT32 free = 0, used = 0, total = 0;

    gcsDATABASE_COUNTERS contiguousCounter = {0, 0, 0};
    gcsDATABASE_COUNTERS virtualCounter = {0, 0, 0};
    gcsDATABASE_COUNTERS nonPagedCounter = {0, 0, 0};

    status = gckKERNEL_GetVideoMemoryPool(kernel, gcvPOOL_SYSTEM, &memory);

    if (gcmIS_SUCCESS(status))
    {
        gcmkVERIFY_OK(
            gckOS_AcquireMutex(memory->os, memory->mutex, gcvINFINITE));

        free  = memory->freeBytes;
        used  = memory->bytes - memory->freeBytes;
        total = memory->bytes;

        gcmkVERIFY_OK(gckOS_ReleaseMutex(memory->os, memory->mutex));
    }

    seq_printf(m, "VIDEO MEMORY:\n");
    seq_printf(m, "    gcvPOOL_SYSTEM:\n");
    seq_printf(m, "        Free  : %10u B\n", free);
    seq_printf(m, "        Used  : %10u B\n", used);
    seq_printf(m, "        Total : %10u B\n", total);

    /* Acquire the database mutex. */
    gcmkVERIFY_OK(
        gckOS_AcquireMutex(kernel->os, kernel->db->dbMutex, gcvINFINITE));

    /* Walk the databases. */
    for (i = 0; i < gcmCOUNTOF(kernel->db->db); ++i)
    {
        for (database = kernel->db->db[i];
             database != gcvNULL;
             database = database->next)
        {
            gcsDATABASE_COUNTERS * counter = &database->vidMemPool[gcvPOOL_CONTIGUOUS];
            _CounterAdd(&contiguousCounter, counter);

            counter = &database->vidMemPool[gcvPOOL_VIRTUAL];
            _CounterAdd(&virtualCounter, counter);


            counter = &database->nonPaged;
            _CounterAdd(&nonPagedCounter, counter);
        }
    }

    /* Release the database mutex. */
    gcmkVERIFY_OK(gckOS_ReleaseMutex(kernel->os, kernel->db->dbMutex));

    _CounterPrint(&contiguousCounter, "gcvPOOL_CONTIGUOUS", m);
    _CounterPrint(&virtualCounter, "gcvPOOL_VIRTUAL", m);

    seq_printf(m, "\n");

    seq_printf(m, "NON PAGED MEMORY:\n");
    seq_printf(m, "    Used  : %10llu B\n", nonPagedCounter.bytes);

    return 0;
}

static int
_ShowRecord(
    IN struct seq_file *File,
    IN gcsDATABASE_RECORD_PTR Record
    )
{
    static const char * recordTypes[gcvDB_NUM_TYPES] = {
        "Unknown",
        "VideoMemory",
        "CommandBuffer",
        "NonPaged",
        "Contiguous",
        "Signal",
        "VidMemLock",
        "Context",
        "Idel",
        "MapMemory",
        "MapUserMemory",
        "ShBuf",
    };

    seq_printf(File, "%-14s %3d %16p %16zu %16zu\n",
        recordTypes[Record->type],
        Record->kernel->core,
        Record->data,
        (size_t) Record->physical,
        Record->bytes
        );

    return 0;
}

static int
_ShowRecords(
    IN struct seq_file *File,
    IN gcsDATABASE_PTR Database
    )
{
    gctUINT i;

    seq_printf(File, "Records:\n");

    seq_printf(File, "%14s %3s %16s %16s %16s\n",
               "Type", "GPU", "Data/Node", "Physical/Node", "Bytes");

    for (i = 0; i < gcmCOUNTOF(Database->list); i++)
    {
        gcsDATABASE_RECORD_PTR record = Database->list[i];

        while (record != NULL)
        {
            _ShowRecord(File, record);
            record = record->next;
        }
    }

    return 0;
}

static void
_ShowCounters(
    struct seq_file *File,
    gcsDATABASE_PTR Database
    )
{
    gctUINT i = 0;

    static const char * surfaceTypes[gcvSURF_NUM_TYPES] = {
        "Unknown",
        "Index",
        "Vertex",
        "Texture",
        "RenderTarget",
        "Depth",
        "Bitmap",
        "TileStatus",
        "Image",
        "Mask",
        "Scissor",
        "HZ",
        "ICache",
        "TxDesc",
        "Fence",
        "TFBHeader",
    };

    static const char * poolTypes[gcvPOOL_NUMBER_OF_POOLS] = {
        "Unknown",
        "Default",
        "Local",
        "Internal",
        "External",
        "Unified",
        "System",
        "Virtual",
        "User",
        "Contiguous",
    };

    static const char * otherCounterNames[] = {
        "AllocNonPaged",
        "AllocContiguous",
        "MapUserMemory",
        "MapMemory",
    };

    gcsDATABASE_COUNTERS * otherCounters[] = {
        &Database->nonPaged,
        &Database->contiguous,
        &Database->mapUserMemory,
        &Database->mapMemory,
    };

    seq_printf(File, "%-16s %16s %16s %16s\n", "", "Current", "Maximum", "Total");

    /* Print surface type counters. */
    seq_printf(File, "%-16s %16lld %16lld %16lld\n",
               "All-Types",
               Database->vidMem.bytes,
               Database->vidMem.maxBytes,
               Database->vidMem.totalBytes);

    for (i = 1; i < gcvSURF_NUM_TYPES; i++)
    {
        seq_printf(File, "%-16s %16lld %16lld %16lld\n",
                   surfaceTypes[i],
                   Database->vidMemType[i].bytes,
                   Database->vidMemType[i].maxBytes,
                   Database->vidMemType[i].totalBytes);
    }
    seq_puts(File, "\n");

    /* Print surface pool counters. */
    seq_printf(File, "%-16s %16lld %16lld %16lld\n",
               "All-Pools",
               Database->vidMem.bytes,
               Database->vidMem.maxBytes,
               Database->vidMem.totalBytes);

    for (i = 1; i < gcvPOOL_NUMBER_OF_POOLS; i++)
    {
        seq_printf(File, "%-16s %16lld %16lld %16lld\n",
                   poolTypes[i],
                   Database->vidMemPool[i].bytes,
                   Database->vidMemPool[i].maxBytes,
                   Database->vidMemPool[i].totalBytes);
    }
    seq_puts(File, "\n");

    /* Print other counters. */
    for (i = 0; i < gcmCOUNTOF(otherCounterNames); i++)
    {
        seq_printf(File, "%-16s %16lld %16lld %16lld\n",
                   otherCounterNames[i],
                   otherCounters[i]->bytes,
                   otherCounters[i]->maxBytes,
                   otherCounters[i]->totalBytes);
    }
    seq_puts(File, "\n");
}

static void
_ShowProcess(
    IN struct seq_file *File,
    IN gcsDATABASE_PTR Database
    )
{
    gctINT pid;
    char name[24];

    /* Process ID and name */
    pid = Database->processID;
    gcmkVERIFY_OK(gckOS_GetProcessNameByPid(pid, gcmSIZEOF(name), name));

    seq_printf(File, "--------------------------------------------------------------------------------\n");
    seq_printf(File, "Process: %-8d %s\n", pid, name);

    /* Detailed records */
    _ShowRecords(File, Database);

    seq_printf(File, "Counters:\n");

    _ShowCounters(File, Database);
}

static void
_ShowProcesses(
    IN struct seq_file * File,
    IN gckKERNEL Kernel
    )
{
    gcsDATABASE_PTR database;
    gctINT i;
    static gctUINT64 idleTime = 0;

    /* Acquire the database mutex. */
    gcmkVERIFY_OK(
        gckOS_AcquireMutex(Kernel->os, Kernel->db->dbMutex, gcvINFINITE));

    if (Kernel->db->idleTime)
    {
        /* Record idle time if DB upated. */
        idleTime = Kernel->db->idleTime;
        Kernel->db->idleTime = 0;
    }

    /* Idle time since last call */
    seq_printf(File, "GPU Idle: %llu ns\n",  idleTime);

    /* Walk the databases. */
    for (i = 0; i < gcmCOUNTOF(Kernel->db->db); ++i)
    {
        for (database = Kernel->db->db[i];
             database != gcvNULL;
             database = database->next)
        {
            _ShowProcess(File, database);
        }
    }

    /* Release the database mutex. */
    gcmkVERIFY_OK(gckOS_ReleaseMutex(Kernel->os, Kernel->db->dbMutex));
}

static int
gc_db_show(struct seq_file *m, void *data)
{
    gcsINFO_NODE *node = m->private;
    gckGALDEVICE device = node->device;
    gckKERNEL kernel = _GetValidKernel(device);
    _ShowProcesses(m, kernel);
    return 0 ;
}

static int
gc_version_show(struct seq_file *m, void *data)
{
    gcsINFO_NODE *node = m->private;
    gckGALDEVICE device = node->device;
    gcsPLATFORM * platform = device->platform;
    gctCONST_STRING name;

    seq_printf(m, "%s built at %s\n",  gcvVERSION_STRING, HOST);

    if (platform->ops->name)
    {
        platform->ops->name(platform, &name);
        seq_printf(m, "Platform path: %s\n", name);
    }
    else
    {
        seq_printf(m, "Code path: %s\n", __FILE__);
    }

    return 0 ;
}

/*******************************************************************************
**
** Show PM state timer.
**
** Entry is called as 'idle' for compatible reason, it shows more information
** than idle actually.
**
**  Start: Start time of this counting period.
**  End: End time of this counting peroid.
**  On: Time GPU stays in gcvPOWER_0N.
**  Off: Time GPU stays in gcvPOWER_0FF.
**  Idle: Time GPU stays in gcvPOWER_IDLE.
**  Suspend: Time GPU stays in gcvPOWER_SUSPEND.
*/
static int
gc_idle_show(struct seq_file *m, void *data)
{
    gcsINFO_NODE *node = m->private;
    gckGALDEVICE device = node->device;
    gckKERNEL kernel = _GetValidKernel(device);

    gctUINT64 start;
    gctUINT64 end;
    gctUINT64 on;
    gctUINT64 off;
    gctUINT64 idle;
    gctUINT64 suspend;

    gckHARDWARE_QueryStateTimer(kernel->hardware, &start, &end, &on, &off, &idle, &suspend);

    /* Idle time since last call */
    seq_printf(m, "Start:   %llu ns\n",  start);
    seq_printf(m, "End:     %llu ns\n",  end);
    seq_printf(m, "On:      %llu ns\n",  on);
    seq_printf(m, "Off:     %llu ns\n",  off);
    seq_printf(m, "Idle:    %llu ns\n",  idle);
    seq_printf(m, "Suspend: %llu ns\n",  suspend);

    return 0 ;
}

extern void
_DumpState(
    IN gckKERNEL Kernel
    );

/*******************************************************************************
**
** Show PM state timer.
**
** Entry is called as 'idle' for compatible reason, it shows more information
** than idle actually.
**
**  Start: Start time of this counting period.
**  End: End time of this counting peroid.
**  On: Time GPU stays in gcvPOWER_0N.
**  Off: Time GPU stays in gcvPOWER_0FF.
**  Idle: Time GPU stays in gcvPOWER_IDLE.
**  Suspend: Time GPU stays in gcvPOWER_SUSPEND.
*/

static int dumpCore = 0;

static int
gc_dump_trigger_show(struct seq_file *m, void *data)
{
#if gcdENABLE_3D || gcdENABLE_2D
    gcsINFO_NODE *node = m->private;
    gckGALDEVICE device = node->device;
    gckKERNEL kernel = gcvNULL;

    if (dumpCore >= gcvCORE_MAJOR && dumpCore < gcvCORE_COUNT)
    {
        kernel = device->kernels[dumpCore];
    }
#endif

    seq_printf(m, gcdDEBUG_FS_WARN);

#if gcdENABLE_3D || gcdENABLE_2D
    seq_printf(m, "Get dump from /proc/kmsg or /sys/kernel/debug/gc/galcore_trace\n");

    if (kernel && kernel->hardware->powerManagement == gcvFALSE)
    {
        _DumpState(kernel);
    }
#endif

    return 0;
}

static int dumpProcess = 0;


static int gc_vidmem_show(struct seq_file *m, void *unused)
{
    gceSTATUS status;
    gcsDATABASE_PTR database;
    gcsINFO_NODE *node = m->private;
    gckGALDEVICE device = node->device;
    char name[64];
    int i;

    gckKERNEL kernel = _GetValidKernel(device);

    if (dumpProcess == 0)
    {
        /* Acquire the database mutex. */
        gcmkVERIFY_OK(
        gckOS_AcquireMutex(kernel->os, kernel->db->dbMutex, gcvINFINITE));

        for (i = 0; i < gcmCOUNTOF(kernel->db->db); i++)
        {
            for (database = kernel->db->db[i];
                 database != gcvNULL;
                 database = database->next)
            {
                gckOS_GetProcessNameByPid(database->processID, gcmSIZEOF(name), name);
                seq_printf(m, "VidMem Usage (Process %d: %s):\n", database->processID, name);
                _ShowCounters(m, database);
                seq_puts(m, "\n");
            }
        }

        /* Release the database mutex. */
        gcmkVERIFY_OK(gckOS_ReleaseMutex(kernel->os, kernel->db->dbMutex));
    }
    else
    {
        /* Find the database. */
        status = gckKERNEL_FindDatabase(kernel, dumpProcess, gcvFALSE, &database);

        if (gcmIS_ERROR(status))
        {
            seq_printf(m, "ERROR: process %d not found\n", dumpProcess);
            return 0;
        }

        gckOS_GetProcessNameByPid(dumpProcess, gcmSIZEOF(name), name);
        seq_printf(m, "VidMem Usage (Process %d: %s):\n", dumpProcess, name);
        _ShowCounters(m, database);
    }

    return 0;
}

static int gc_vidmem_write(const char __user *buf, size_t count, void* data)
{
    dumpProcess = simple_strtol(buf, NULL, 0);
    return count;
}

static int gc_dump_trigger_write(const char __user *buf, size_t count, void* data)
{
    dumpCore = simple_strtol(buf, NULL, 0);
    return count;
}

static gcsINFO InfoList[] =
{
    {"info", gc_info_show},
    {"clients", gc_clients_show},
    {"meminfo", gc_meminfo_show},
    {"idle", gc_idle_show},
    {"database", gc_db_show},
    {"version", gc_version_show},
    {"vidmem", gc_vidmem_show, gc_vidmem_write},
    {"dump_trigger", gc_dump_trigger_show, gc_dump_trigger_write},
};

static gceSTATUS
_DebugfsInit(
    IN gckGALDEVICE Device
    )
{
    gceSTATUS status;

    gckDEBUGFS_DIR dir = &Device->debugfsDir;

    gcmkONERROR(gckDEBUGFS_DIR_Init(dir, gcvNULL, "gc"));

    gcmkONERROR(gckDEBUGFS_DIR_CreateFiles(dir, InfoList, gcmCOUNTOF(InfoList), Device));

    return gcvSTATUS_OK;

OnError:
    return status;
}

static void
_DebugfsCleanup(
    IN gckGALDEVICE Device
    )
{
    gckDEBUGFS_DIR dir = &Device->debugfsDir;

    if (Device->debugfsDir.root)
    {
        gcmkVERIFY_OK(gckDEBUGFS_DIR_RemoveFiles(dir, InfoList, gcmCOUNTOF(InfoList)));

        gckDEBUGFS_DIR_Deinit(dir);
    }
}


/******************************************************************************\
*************************** Memory Allocation Wrappers *************************
\******************************************************************************/

static gceSTATUS
_AllocateMemory(
    IN gckGALDEVICE Device,
    IN gctSIZE_T Bytes,
    OUT gctPOINTER *Logical,
    OUT gctPHYS_ADDR *Physical,
    OUT gctUINT32 *PhysAddr
    )
{
    gceSTATUS status;
    gctPHYS_ADDR_T physAddr;

    gcmkHEADER_ARG("Device=0x%x Bytes=%lu", Device, Bytes);

    gcmkVERIFY_ARGUMENT(Device != NULL);
    gcmkVERIFY_ARGUMENT(Logical != NULL);
    gcmkVERIFY_ARGUMENT(Physical != NULL);
    gcmkVERIFY_ARGUMENT(PhysAddr != NULL);

    gcmkONERROR(gckOS_AllocateContiguous(
        Device->os, gcvFALSE, &Bytes, Physical, Logical
        ));

    gcmkONERROR(gckOS_GetPhysicalAddress(
        Device->os, *Logical, &physAddr
        ));

    gcmkSAFECASTPHYSADDRT(*PhysAddr, physAddr);

    /* Success. */
    gcmkFOOTER_ARG(
        "*Logical=0x%x *Physical=0x%x *PhysAddr=0x%08x",
        *Logical, *Physical, *PhysAddr
        );

    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}

static gceSTATUS
_FreeMemory(
    IN gckGALDEVICE Device,
    IN gctPOINTER Logical,
    IN gctPHYS_ADDR Physical)
{
    gceSTATUS status;

    gcmkHEADER_ARG("Device=0x%x Logical=0x%x Physical=0x%x",
                   Device, Logical, Physical);

    gcmkVERIFY_ARGUMENT(Device != NULL);

    status = gckOS_FreeContiguous(
        Device->os, Physical, Logical,
        ((PLINUX_MDL) Physical)->numPages * PAGE_SIZE
        );

    gcmkFOOTER();
    return status;
}

static gceSTATUS
_SetupVidMem(
    IN gckGALDEVICE Device,
    IN gctUINT32 ContiguousBase,
    IN gctSIZE_T ContiguousSize,
    IN gctSIZE_T BankSize,
    IN gcsDEVICE_CONSTRUCT_ARGS * Args
    )
{
    gceSTATUS status;
    gctUINT32 physAddr = ~0U;
    gckGALDEVICE device = Device;
    struct resource* mem_region;

    /* set up the contiguous memory */
    device->contiguousSize = ContiguousSize;

    if (ContiguousSize > 0)
    {
        if (ContiguousBase == 0)
        {
            while (device->contiguousSize > 0)
            {
                /* Allocate contiguous memory. */
                status = _AllocateMemory(
                    device,
                    device->contiguousSize,
                    &device->contiguousBase,
                    &device->contiguousPhysical,
                    &physAddr
                    );

                if (gcmIS_SUCCESS(status))
                {
                    status = gckVIDMEM_Construct(
                        device->os,
                        physAddr | device->systemMemoryBaseAddress,
                        device->contiguousSize,
                        64,
                        BankSize,
                        &device->contiguousVidMem
                        );

                    if (gcmIS_SUCCESS(status))
                    {
                        device->contiguousRequested = gcvTRUE;
                        device->requestedContiguousBase = physAddr;
                        break;
                    }

                    gcmkONERROR(_FreeMemory(
                        device,
                        device->contiguousBase,
                        device->contiguousPhysical
                        ));

                    device->contiguousBase     = gcvNULL;
                    device->contiguousPhysical = gcvNULL;
                }

                if (device->contiguousSize <= (4 << 20))
                {
                    device->contiguousSize = 0;
                }
                else
                {
                    device->contiguousSize -= (4 << 20);
                }
            }
        }
        else
        {
            /* Create the contiguous memory heap. */
            status = gckVIDMEM_Construct(
                device->os,
                ContiguousBase | device->systemMemoryBaseAddress,
                ContiguousSize,
                64, BankSize,
                &device->contiguousVidMem
                );

            if (gcmIS_ERROR(status))
            {
                /* Error, disable contiguous memory pool. */
                device->contiguousVidMem = gcvNULL;
                device->contiguousSize   = 0;
            }
            else
            {
                if (Args->contiguousRequested == gcvFALSE)
                {
                    mem_region = request_mem_region(
                        ContiguousBase, ContiguousSize, "galcore managed memory"
                        );

                    if (mem_region == gcvNULL)
                    {
                        gcmkTRACE_ZONE(
                            gcvLEVEL_ERROR, gcvZONE_DRIVER,
                            "%s(%d): Failed to claim %ld bytes @ 0x%08X\n",
                            __FUNCTION__, __LINE__,
                            ContiguousSize, ContiguousBase
                            );

                        gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
                    }
                }

                device->requestedContiguousBase  = ContiguousBase;
                device->requestedContiguousSize  = ContiguousSize;
                device->contiguousRequested      = Args->contiguousRequested;

                device->contiguousPhysical = gcvNULL;
                device->contiguousPhysicalName = 0;
                device->contiguousSize     = ContiguousSize;
                device->contiguousMapped   = gcvTRUE;
            }
        }
    }

    return gcvSTATUS_OK;
OnError:
    return status;
}

void
_SetupRegisterPhysical(
    IN gckGALDEVICE Device,
    IN gcsDEVICE_CONSTRUCT_ARGS * Args
    )
{
    gctINT *irqs = Args->irqs;
    gctUINT *registerBases = Args->registerBases;
    gctUINT *registerSizes = Args->registerSizes;

    gctINT i = 0;

    for (i = 0; i < gcvCORE_COUNT; i++)
    {
        if (irqs[i] != -1)
        {
            Device->requestedRegisterMemBases[i] = registerBases[i];
            Device->requestedRegisterMemSizes[i] = registerSizes[i];

            gcmkTRACE_ZONE(gcvLEVEL_INFO, _GC_OBJ_ZONE,
                           "Get register base %llx of core %d",
                           registerBases[i], i);
        }
    }
}

/******************************************************************************\
******************************* Interrupt Handler ******************************
\******************************************************************************/
static irqreturn_t isrRoutine(int irq, void *ctxt)
{
    gceSTATUS status;
    gckGALDEVICE device;
    gceCORE Core = (gceCORE) gcmPTR2INT32(ctxt);

    device = galDevice;

    /* Call kernel interrupt notification. */
    status = gckKERNEL_Notify(device->kernels[Core], gcvNOTIFY_INTERRUPT, gcvTRUE);

    if (gcmIS_SUCCESS(status))
    {
        up(&device->semas[Core]);

        return IRQ_HANDLED;
    }

    return IRQ_NONE;
}

static int threadRoutine(void *ctxt)
{
    gckGALDEVICE device = galDevice;
    gceCORE core = (gceCORE) gcmPTR2INT32(ctxt);
    gctUINT i;

    gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_DRIVER,
                   "Starting isr Thread with extension=%p",
                   device);

    if (core != gcvCORE_VG)
    {
        /* Make kernel update page table of this thread to include entry related to command buffer.*/
        for (i = 0; i < gcdCOMMAND_QUEUES; i++)
        {
            gctUINT32 data = *(gctUINT32_PTR)device->kernels[core]->command->queues[i].logical;

            data = 0;
        }
    }

    for (;;)
    {
        static int down;

        down = down_interruptible(&device->semas[core]);
        if (down); /*To make gcc 4.6 happye*/

        if (device->killThread == gcvTRUE)
        {
            /* The daemon exits. */
            while (!kthread_should_stop())
            {
                gckOS_Delay(device->os, 1);
            }

            return 0;
        }

        gckKERNEL_Notify(device->kernels[core],
                         gcvNOTIFY_INTERRUPT,
                         gcvFALSE);
    }
}

static irqreturn_t isrRoutineVG(int irq, void *ctxt)
{
#if gcdENABLE_VG
    gceSTATUS status;
    gckGALDEVICE device;

    device = (gckGALDEVICE) ctxt;

    /* Serve the interrupt. */
    status = gckVGINTERRUPT_Enque(device->kernels[gcvCORE_VG]->vg->interrupt);

    /* Determine the return value. */
    return (status == gcvSTATUS_NOT_OUR_INTERRUPT)
        ? IRQ_RETVAL(0)
        : IRQ_RETVAL(1);
#else
    return IRQ_NONE;
#endif
}

/******************************************************************************\
******************************* gckGALDEVICE Code ******************************
\******************************************************************************/

static gceSTATUS
_StartThread(
    IN int (*ThreadRoutine)(void *data),
    IN gceCORE Core
    )
{
    gceSTATUS status;
    gckGALDEVICE device = galDevice;
    struct task_struct * task;

    if (device->kernels[Core] != gcvNULL)
    {
        /* Start the kernel thread. */
        task = kthread_run(ThreadRoutine, (void *)Core, "galcore deamon thread for core[%d]", Core);

        if (IS_ERR(task))
        {
            gcmkTRACE_ZONE(
                gcvLEVEL_ERROR, gcvZONE_DRIVER,
                "%s(%d): Could not start the kernel thread.\n",
                __FUNCTION__, __LINE__
                );

            gcmkONERROR(gcvSTATUS_GENERIC_IO);
        }

        device->threadCtxts[Core]         = task;
        device->threadInitializeds[Core]  = gcvTRUE;
    }
    else
    {
        device->threadInitializeds[Core]  = gcvFALSE;
    }

    return gcvSTATUS_OK;

OnError:
    return status;
}

/*******************************************************************************
**
**  gckGALDEVICE_Construct
**
**  Constructor.
**
**  INPUT:
**
**  OUTPUT:
**
**      gckGALDEVICE * Device
**          Pointer to a variable receiving the gckGALDEVICE object pointer on
**          success.
*/
gceSTATUS
gckGALDEVICE_Construct(
    IN gctINT IrqLine,
    IN gctUINT32 RegisterMemBase,
    IN gctSIZE_T RegisterMemSize,
    IN gctINT IrqLine2D,
    IN gctUINT32 RegisterMemBase2D,
    IN gctSIZE_T RegisterMemSize2D,
    IN gctINT IrqLineVG,
    IN gctUINT32 RegisterMemBaseVG,
    IN gctSIZE_T RegisterMemSizeVG,
    IN gctUINT32 ContiguousBase,
    IN gctSIZE_T ContiguousSize,
    IN gctSIZE_T BankSize,
    IN gctINT FastClear,
    IN gctINT Compression,
    IN gctUINT32 PhysBaseAddr,
    IN gctUINT32 PhysSize,
    IN gctINT Signal,
    IN gctUINT LogFileSize,
    IN gctINT PowerManagement,
    IN gctINT GpuProfiler,
    IN gcsDEVICE_CONSTRUCT_ARGS * Args,
    OUT gckGALDEVICE *Device
    )
{
    gctUINT32 internalBaseAddress = 0, internalAlignment = 0;
    gctUINT32 externalBaseAddress = 0, externalAlignment = 0;
    gctUINT32 horizontalTileSize, verticalTileSize;
    struct resource* mem_region;
    gctUINT32 physical;
    gckGALDEVICE device;
    gceSTATUS status;
    gctINT32 i;
    gceHARDWARE_TYPE type;
    gckKERNEL kernel = gcvNULL;

    gcmkHEADER_ARG("IrqLine=%d RegisterMemBase=0x%08x RegisterMemSize=%u "
                   "IrqLine2D=%d RegisterMemBase2D=0x%08x RegisterMemSize2D=%u "
                   "IrqLineVG=%d RegisterMemBaseVG=0x%08x RegisterMemSizeVG=%u "
                   "ContiguousBase=0x%08x ContiguousSize=%lu BankSize=%lu "
                   "FastClear=%d Compression=%d PhysBaseAddr=0x%x PhysSize=%d Signal=%d",
                   IrqLine, RegisterMemBase, RegisterMemSize,
                   IrqLine2D, RegisterMemBase2D, RegisterMemSize2D,
                   IrqLineVG, RegisterMemBaseVG, RegisterMemSizeVG,
                   ContiguousBase, ContiguousSize, BankSize, FastClear, Compression,
                   PhysBaseAddr, PhysSize, Signal);

#if !gcdENABLE_3D
    IrqLine = -1;
#endif

#if !gcdENABLE_2D
    IrqLine2D = -1;
#endif
    /* Allocate device structure. */
    device = kmalloc(sizeof(struct _gckGALDEVICE), GFP_KERNEL | __GFP_NOWARN);
    if (!device)
    {
        gcmkONERROR(gcvSTATUS_OUT_OF_MEMORY);
    }

    memset(device, 0, sizeof(struct _gckGALDEVICE));

    device->dbgNode = gcvNULL;

    device->platform = Args->platform;

    device->args = *Args;

    /* set up the contiguous memory */
    device->contiguousSize = ContiguousSize;

    /* Clear irq lines. */
    for (i = 0; i < gcdMAX_GPU_COUNT; i++)
    {
        device->irqLines[i] = -1;
    }

    gcmkONERROR(_DebugfsInit(device));

    if (gckDEBUGFS_CreateNode(
            device, LogFileSize, device->debugfsDir.root ,DEBUG_FILE, &(device->dbgNode)))
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): Failed to create  the debug file system  %s/%s \n",
            __FUNCTION__, __LINE__,
            PARENT_FILE, DEBUG_FILE
        );
    }
    else if (LogFileSize)
    {
        gckDEBUGFS_SetCurrentNode(device->dbgNode);
    }

    _SetupRegisterPhysical(device, Args);

    if (IrqLine != -1)
    {
        device->requestedRegisterMemBases[gcvCORE_MAJOR] = RegisterMemBase;
        device->requestedRegisterMemSizes[gcvCORE_MAJOR] = RegisterMemSize;
    }

    if (IrqLine2D != -1)
    {
        device->requestedRegisterMemBases[gcvCORE_2D] = RegisterMemBase2D;
        device->requestedRegisterMemSizes[gcvCORE_2D] = RegisterMemSize2D;
    }

    if (IrqLineVG != -1)
    {
        device->requestedRegisterMemBases[gcvCORE_VG] = RegisterMemBaseVG;
        device->requestedRegisterMemSizes[gcvCORE_VG] = RegisterMemSizeVG;
    }
#if gcdDEC_ENABLE_AHB
    {
        device->requestedRegisterMemBases[gcvCORE_DEC] = Args->registerMemBaseDEC300;
        device->requestedRegisterMemSizes[gcvCORE_DEC] = Args->registerMemSizeDEC300;
    }
#endif


    for (i = gcvCORE_MAJOR; i < gcvCORE_COUNT; i++)
    {
        if (Args->irqs[i] != -1)
        {
            device->requestedRegisterMemBases[i] = Args->registerBases[i];
            device->requestedRegisterMemSizes[i] = Args->registerSizes[i];

            gcmkTRACE_ZONE(gcvLEVEL_INFO, gcvZONE_DEVICE,
                           "%s(%d): Core = %d, RegiseterBase = %x",
                           __FUNCTION__, __LINE__,
                           i, Args->registerBases[i]
                           );
        }
    }

    /* Initialize the ISR. */
    device->irqLines[gcvCORE_MAJOR] = IrqLine;
    device->irqLines[gcvCORE_2D] = IrqLine2D;
    device->irqLines[gcvCORE_VG] = IrqLineVG;

    for (i = gcvCORE_MAJOR; i < gcvCORE_COUNT; i++)
    {
        if (Args->irqs[i] != -1)
        {
            device->irqLines[i] = Args->irqs[i];
        }
    }

    device->requestedContiguousBase  = 0;
    device->requestedContiguousSize  = 0;

    for (i = 0; i < gcdMAX_GPU_COUNT; i++)
    {
        physical = device->requestedRegisterMemBases[i];

        /* Set up register memory region. */
        if ( physical != 0)
        {

            if ( Args->registerMemMapped )
            {
                device->registerBases[i] = Args->registerMemAddress;
                device->requestedRegisterMemBases[i] = 0;

            } else {

                mem_region = request_mem_region(physical,
                        device->requestedRegisterMemSizes[i],
                        "galcore register region");

                if (mem_region == gcvNULL)
                {
                    gcmkTRACE_ZONE(
                            gcvLEVEL_ERROR, gcvZONE_DRIVER,
                            "%s(%d): Failed to claim %lu bytes @ 0x%08X\n",
                            __FUNCTION__, __LINE__,
                            physical, device->requestedRegisterMemSizes[i]
                     );

                gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
                }

                device->registerBases[i] = (gctPOINTER) ioremap_nocache(
                        physical, device->requestedRegisterMemSizes[i]);

                if (device->registerBases[i] == gcvNULL)
                {
                    gcmkTRACE_ZONE(
                            gcvLEVEL_ERROR, gcvZONE_DRIVER,
                            "%s(%d): Unable to map %ld bytes @ 0x%08X\n",
                            __FUNCTION__, __LINE__,
                            physical, device->requestedRegisterMemSizes[i]
                    );

                    gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
                }
            }

            physical += device->requestedRegisterMemSizes[i];
        }
    }

    /* Set the base address */
    device->baseAddress = device->physBase = PhysBaseAddr;
    device->physSize = PhysSize;

    /* Construct the gckOS object. */
    gcmkONERROR(gckOS_Construct(device, &device->os));

    /* Construct the gckDEVICE object for os independent core management. */
    gcmkONERROR(gckDEVICE_Construct(device->os, &device->device));

    if (device->irqLines[gcvCORE_MAJOR] != -1)
    {
        gcmkONERROR(gctaOS_ConstructOS(device->os, &device->taos));
    }

    gcmkONERROR(_SetupVidMem(device, ContiguousBase, ContiguousSize, BankSize, Args));

    if (device->irqLines[gcvCORE_MAJOR] != -1)
    {
        gcmkONERROR(gcTA_Construct(device->taos, gcvCORE_MAJOR, &globalTA[gcvCORE_MAJOR]));

        gcmkONERROR(gckDEVICE_AddCore(device->device, gcvCORE_MAJOR, Args->chipIDs[gcvCORE_MAJOR], device, &device->kernels[gcvCORE_MAJOR]));

        /* Setup the ISR manager. */
        gcmkONERROR(gckHARDWARE_SetIsrManager(
            device->kernels[gcvCORE_MAJOR]->hardware,
            (gctISRMANAGERFUNC) gckGALDEVICE_Enable_ISR,
            (gctISRMANAGERFUNC) gckGALDEVICE_Disable_ISR,
            (gctPOINTER)gcvCORE_MAJOR
            ));

        gcmkONERROR(gckHARDWARE_SetFastClear(
            device->kernels[gcvCORE_MAJOR]->hardware, FastClear, Compression
            ));

        gcmkONERROR(gckHARDWARE_SetPowerManagement(
            device->kernels[gcvCORE_MAJOR]->hardware, PowerManagement
            ));

#if gcdENABLE_FSCALE_VAL_ADJUST
        gcmkONERROR(gckHARDWARE_SetMinFscaleValue(
            device->kernels[gcvCORE_MAJOR]->hardware, Args->gpu3DMinClock
            ));
#endif

        gcmkONERROR(gckHARDWARE_SetGpuProfiler(
            device->kernels[gcvCORE_MAJOR]->hardware, GpuProfiler
            ));
    }
    else
    {
        device->kernels[gcvCORE_MAJOR] = gcvNULL;
    }

    if (device->irqLines[gcvCORE_2D] != -1)
    {
        gcmkONERROR(gckDEVICE_AddCore(device->device, gcvCORE_2D, gcvCHIP_ID_DEFAULT, device, &device->kernels[gcvCORE_2D]));

        /* Verify the hardware type */
        gcmkONERROR(gckHARDWARE_GetType(device->kernels[gcvCORE_2D]->hardware, &type));

        if (type != gcvHARDWARE_2D)
        {
            gcmkTRACE_ZONE(
                gcvLEVEL_ERROR, gcvZONE_DRIVER,
                "%s(%d): Unexpected hardware type: %d\n",
                __FUNCTION__, __LINE__,
                type
                );

            gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
        }

        /* Setup the ISR manager. */
        gcmkONERROR(gckHARDWARE_SetIsrManager(
            device->kernels[gcvCORE_2D]->hardware,
            (gctISRMANAGERFUNC) gckGALDEVICE_Enable_ISR,
            (gctISRMANAGERFUNC) gckGALDEVICE_Disable_ISR,
            (gctPOINTER)gcvCORE_2D
            ));

        gcmkONERROR(gckHARDWARE_SetPowerManagement(
            device->kernels[gcvCORE_2D]->hardware, PowerManagement
            ));

#if gcdENABLE_FSCALE_VAL_ADJUST
        gcmkONERROR(gckHARDWARE_SetMinFscaleValue(
            device->kernels[gcvCORE_2D]->hardware, 1
            ));
#endif
    }
    else
    {
        device->kernels[gcvCORE_2D] = gcvNULL;
    }

    if (device->irqLines[gcvCORE_VG] != -1)
    {
#if gcdENABLE_VG
        gcmkONERROR(gckDEVICE_AddCore(device->device, gcvCORE_VG, gcvCHIP_ID_DEFAULT, device, &device->kernels[gcvCORE_VG]));

        gcmkONERROR(gckVGHARDWARE_SetPowerManagement(
            device->kernels[gcvCORE_VG]->vg->hardware,
            PowerManagement
            ));
#endif
    }
    else
    {
        device->kernels[gcvCORE_VG] = gcvNULL;
    }

    /* Add core for multiple core. */
    for (i = gcvCORE_3D1; i <= gcvCORE_3D3; i++)
    {
        if (Args->irqs[i] != -1)
        {
            gcmkONERROR(gcTA_Construct(device->taos, (gceCORE)i, &globalTA[i]));
            gckDEVICE_AddCore(device->device, i, Args->chipIDs[i], device, &device->kernels[i]);

            gcmkONERROR(
            gckHARDWARE_SetFastClear(device->kernels[i]->hardware,
                 FastClear,
                Compression));

            gcmkONERROR(gckHARDWARE_SetPowerManagement(
                device->kernels[i]->hardware, PowerManagement
                ));

            gcmkONERROR(gckHARDWARE_SetGpuProfiler(
                device->kernels[i]->hardware, GpuProfiler
                ));
        }
    }

    /* Initialize the kernel thread semaphores. */
    for (i = 0; i < gcdMAX_GPU_COUNT; i++)
    {
        if (device->irqLines[i] != -1) sema_init(&device->semas[i], 0);
    }

    device->signal = Signal;

    for (i = 0; i < gcdMAX_GPU_COUNT; i++)
    {
        if (device->kernels[i] != gcvNULL) break;
    }

    if (i == gcdMAX_GPU_COUNT)
    {
        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }

#if gcdENABLE_VG
    if (i == gcvCORE_VG)
    {
        /* Query the ceiling of the system memory. */
        gcmkONERROR(gckVGHARDWARE_QuerySystemMemory(
                device->kernels[i]->vg->hardware,
                &device->systemMemorySize,
                &device->systemMemoryBaseAddress
                ));
            /* query the amount of video memory */
        gcmkONERROR(gckVGHARDWARE_QueryMemory(
            device->kernels[i]->vg->hardware,
            &device->internalSize, &internalBaseAddress, &internalAlignment,
            &device->externalSize, &externalBaseAddress, &externalAlignment,
            &horizontalTileSize, &verticalTileSize
            ));
    }
    else
#endif
    {
        /* Query the ceiling of the system memory. */
        gcmkONERROR(gckHARDWARE_QuerySystemMemory(
                device->kernels[i]->hardware,
                &device->systemMemorySize,
                &device->systemMemoryBaseAddress
                ));

            /* query the amount of video memory */
        gcmkONERROR(gckHARDWARE_QueryMemory(
            device->kernels[i]->hardware,
            &device->internalSize, &internalBaseAddress, &internalAlignment,
            &device->externalSize, &externalBaseAddress, &externalAlignment,
            &horizontalTileSize, &verticalTileSize
            ));
    }


    /* Grab the first availiable kernel */
    for (i = 0; i < gcdMAX_GPU_COUNT; i++)
    {
        if (device->irqLines[i] != -1)
        {
            kernel = device->kernels[i];
            break;
        }
    }

    /* Set up the internal memory region. */
    if (device->internalSize > 0)
    {
        status = gckVIDMEM_Construct(
            device->os,
            internalBaseAddress, device->internalSize, internalAlignment,
            0, &device->internalVidMem
            );

        if (gcmIS_ERROR(status))
        {
            /* Error, disable internal heap. */
            device->internalSize = 0;
        }
        else
        {
            /* Map internal memory. */
            device->internalLogical
                = (gctPOINTER) ioremap_nocache(physical, device->internalSize);

            if (device->internalLogical == gcvNULL)
            {
                gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
            }

            device->internalPhysical = (gctPHYS_ADDR)(gctUINTPTR_T) physical;
            device->internalPhysicalName = gcmPTR_TO_NAME(device->internalPhysical);
            physical += device->internalSize;
        }
    }

    if (device->externalSize > 0)
    {
        /* create the external memory heap */
        status = gckVIDMEM_Construct(
            device->os,
            externalBaseAddress, device->externalSize, externalAlignment,
            0, &device->externalVidMem
            );

        if (gcmIS_ERROR(status))
        {
            /* Error, disable internal heap. */
            device->externalSize = 0;
        }
        else
        {
            /* Map external memory. */
            device->externalLogical
                = (gctPOINTER) ioremap_nocache(physical, device->externalSize);

            if (device->externalLogical == gcvNULL)
            {
                gcmkONERROR(gcvSTATUS_OUT_OF_RESOURCES);
            }

            device->externalPhysical = (gctPHYS_ADDR)(gctUINTPTR_T) physical;
            device->externalPhysicalName = gcmPTR_TO_NAME(device->externalPhysical);
            physical += device->externalSize;
        }
    }

    if (device->contiguousPhysical)
    {
        device->contiguousPhysicalName = gcmPTR_TO_NAME(device->contiguousPhysical);
    }

    /* Return pointer to the device. */
    *Device = galDevice = device;

    gcmkFOOTER_ARG("*Device=0x%x", * Device);
    return gcvSTATUS_OK;

OnError:
    /* Roll back. */
    gcmkVERIFY_OK(gckGALDEVICE_Destroy(device));

    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckGALDEVICE_Destroy
**
**  Class destructor.
**
**  INPUT:
**
**      Nothing.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      Nothing.
*/
gceSTATUS
gckGALDEVICE_Destroy(
    gckGALDEVICE Device)
{
    gctINT i;
    gckKERNEL kernel = gcvNULL;

    gcmkHEADER_ARG("Device=0x%x", Device);

    if (Device != gcvNULL)
    {
        /* Grab the first availiable kernel */
        for (i = 0; i < gcdMAX_GPU_COUNT; i++)
        {
            if (Device->irqLines[i] != -1)
            {
                kernel = Device->kernels[i];
                break;
            }
        }

        if (Device->internalPhysicalName != 0)
        {
            gcmRELEASE_NAME(Device->internalPhysicalName);
            Device->internalPhysicalName = 0;
        }
        if (Device->externalPhysicalName != 0)
        {
            gcmRELEASE_NAME(Device->externalPhysicalName);
            Device->externalPhysicalName = 0;
        }
        if (Device->contiguousPhysicalName != 0)
        {
            gcmRELEASE_NAME(Device->contiguousPhysicalName);
            Device->contiguousPhysicalName = 0;
        }


        for (i = 0; i < gcdMAX_GPU_COUNT; i++)
        {
            if (Device->kernels[i] != gcvNULL)
            {
                Device->kernels[i] = gcvNULL;
            }
        }

        if (Device->internalLogical != gcvNULL)
        {
            /* Unmap the internal memory. */
            iounmap(Device->internalLogical);
            Device->internalLogical = gcvNULL;
        }

        if (Device->internalVidMem != gcvNULL)
        {
            /* Destroy the internal heap. */
            gcmkVERIFY_OK(gckVIDMEM_Destroy(Device->internalVidMem));
            Device->internalVidMem = gcvNULL;
        }

        if (Device->externalLogical != gcvNULL)
        {
            /* Unmap the external memory. */
            iounmap(Device->externalLogical);
            Device->externalLogical = gcvNULL;
        }

        if (Device->externalVidMem != gcvNULL)
        {
            /* destroy the external heap */
            gcmkVERIFY_OK(gckVIDMEM_Destroy(Device->externalVidMem));
            Device->externalVidMem = gcvNULL;
        }

        if (Device->contiguousBase != gcvNULL)
        {
            if (Device->contiguousMapped == gcvFALSE)
            {
                gcmkVERIFY_OK(_FreeMemory(
                    Device,
                    Device->contiguousBase,
                    Device->contiguousPhysical
                    ));
            }

            Device->contiguousBase     = gcvNULL;
            Device->contiguousPhysical = gcvNULL;
        }

        if (Device->requestedContiguousBase != 0
         && Device->contiguousRequested == gcvFALSE
        )
        {
            release_mem_region(Device->requestedContiguousBase, Device->requestedContiguousSize);
            Device->requestedContiguousBase = 0;
            Device->requestedContiguousSize = 0;
        }

        if (Device->contiguousVidMem != gcvNULL)
        {
            /* Destroy the contiguous heap. */
            gcmkVERIFY_OK(gckVIDMEM_Destroy(Device->contiguousVidMem));
            Device->contiguousVidMem = gcvNULL;
        }

        for (i = 0; i < gcdMAX_GPU_COUNT; i++)
        {
            if (Device->registerBases[i] != gcvNULL)
            {
                /* Unmap register memory. */
                if ( Device->requestedRegisterMemBases[i] != 0 )
                    iounmap(Device->registerBases[i]);

                if (Device->requestedRegisterMemBases[i] != 0)
                {
                    release_mem_region(Device->requestedRegisterMemBases[i],
                            Device->requestedRegisterMemSizes[i]);
                }

                Device->registerBases[i] = gcvNULL;
                Device->requestedRegisterMemBases[i] = 0;
                Device->requestedRegisterMemSizes[i] = 0;
            }
        }

        if (Device->device)
        {
            gcmkVERIFY_OK(gckDEVICE_Destroy(Device->os, Device->device));

            for (i = 0; i < gcdMAX_GPU_COUNT; i++)
            {
                if (globalTA[i])
                {
                    gcTA_Destroy(globalTA[i]);
                    globalTA[i] = gcvNULL;
                }
            }

            Device->device = gcvNULL;
        }

        if (Device->taos)
        {
            gcmkVERIFY_OK(gctaOS_DestroyOS(Device->taos));
            Device->taos = gcvNULL;
        }

        /* Destroy the gckOS object. */
        if (Device->os != gcvNULL)
        {
            gcmkVERIFY_OK(gckOS_Destroy(Device->os));
            Device->os = gcvNULL;
        }

        if (Device->dbgNode)
        {
            gckDEBUGFS_FreeNode(Device->dbgNode);

            if(Device->dbgNode != gcvNULL)
            {
                kfree(Device->dbgNode);
                Device->dbgNode = gcvNULL;
            }
        }

        _DebugfsCleanup(Device);

        /* Free the device. */
        kfree(Device);
    }

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gckGALDEVICE_Setup_ISR
**
**  Start the ISR routine.
**
**  INPUT:
**
**      gckGALDEVICE Device
**          Pointer to an gckGALDEVICE object.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      gcvSTATUS_OK
**          Setup successfully.
**      gcvSTATUS_GENERIC_IO
**          Setup failed.
*/
gceSTATUS
gckGALDEVICE_Setup_ISR(
    IN gceCORE Core
    )
{
    gceSTATUS status;
    gctINT ret = 0;
    gckGALDEVICE Device = galDevice;

    gcmkHEADER_ARG("Device=0x%x Core=%d", Device, Core);

    gcmkVERIFY_ARGUMENT(Device != NULL);

    if (Device->irqLines[Core] < 0)
    {
        gcmkONERROR(gcvSTATUS_GENERIC_IO);
    }

    /* Hook up the isr based on the irq line. */
#ifdef FLAREON
    gc500_handle.dev_id    = Device;
    switch (Core) {
        case gcvCORE_MAJOR:
            gc500_handle.dev_name  = "galcore interrupt service";
            gc500_handle.handler   = isrRoutine;
            break;
        case gcvCORE_2D:
            gc500_handle.dev_name  = "galcore 2D interrupt service";
            gc500_handle.handler   = isrRoutine;
            break;
        case gcvCORE_VG:
            gc500_handle.dev_name  = "galcore VG interrupt service";
            gc500_handle.handler   = isrRoutineVG;
            break;
        default:
            break;
    }
    gc500_handle.intr_gen  = GPIO_INTR_LEVEL_TRIGGER;
    gc500_handle.intr_trig = GPIO_TRIG_HIGH_LEVEL;

    ret = dove_gpio_request(
        DOVE_GPIO0_7, &gc500_handle
        );
#else
    switch (Core) {
        case gcvCORE_MAJOR:
            ret = request_irq(
                Device->irqLines[Core], isrRoutine, gcdIRQF_FLAG,
                "galcore interrupt service", (gctPOINTER)Core
                );
            break;
        case gcvCORE_2D:
            ret = request_irq(
                Device->irqLines[Core], isrRoutine, gcdIRQF_FLAG,
                "galcore 2D interrupt service", (gctPOINTER)Core
                );
            break;
        case gcvCORE_VG:
            ret = request_irq(
                Device->irqLines[Core], isrRoutineVG, gcdIRQF_FLAG,
                "galcore VG interrupt service", Device
                );
            break;
        default:
            break;
    }

    if (ret != 0)
    {
        gcmkTRACE_ZONE(
            gcvLEVEL_ERROR, gcvZONE_DRIVER,
            "%s(%d): Could not register irq line %d (error=%d)\n",
            __FUNCTION__, __LINE__,
            Device->irqLines[Core], ret
            );

        gcmkONERROR(gcvSTATUS_GENERIC_IO);
    }

    Device->isrEnabled[Core] = 1;

    /* Mark ISR as initialized. */
    Device->isrInitializeds[Core] = gcvTRUE;
#endif

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}

gceSTATUS
gckGALDEVICE_Enable_ISR(
    IN gceCORE Core
    )
{
    gceSTATUS status;
    gckGALDEVICE Device = galDevice;

    gcmkHEADER_ARG("Device=0x%x Core=%d", Device, Core);

    gcmkVERIFY_ARGUMENT(Device != NULL);

    if (Device->irqLines[Core] < 0)
    {
        gcmkONERROR(gcvSTATUS_GENERIC_IO);
    }

    if (Device->isrEnabled[Core] == 0)
    {
        enable_irq(Device->irqLines[Core]);
        /* Mark ISR as initialized. */
        Device->isrEnabled[Core] = gcvTRUE;
    }
    Device->isrEnabled[Core]++;

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckGALDEVICE_Release_ISR
**
**  Release the irq line.
**
**  INPUT:
**
**      gckGALDEVICE Device
**          Pointer to an gckGALDEVICE object.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      Nothing.
*/
gceSTATUS
gckGALDEVICE_Release_ISR(
    IN gceCORE Core
    )
{
    gckGALDEVICE Device = galDevice;
    gcmkHEADER_ARG("Device=0x%x", Device);

    gcmkVERIFY_ARGUMENT(Device != NULL);

    /* release the irq */
    if (Device->isrInitializeds[Core])
    {
#ifdef FLAREON
        dove_gpio_free(DOVE_GPIO0_7, "galcore interrupt service");
#else
        free_irq(Device->irqLines[Core], (gctPOINTER)Core);
#endif
        Device->isrInitializeds[Core] = gcvFALSE;
    }

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;
}

gceSTATUS
gckGALDEVICE_Disable_ISR(
    IN gceCORE Core
    )
{
    gckGALDEVICE Device = galDevice;
    gcmkHEADER_ARG("Device=0x%x Core=%d", Device, Core);

    gcmkVERIFY_ARGUMENT(Device != NULL);

    /* disable the irq */
    if (Device->isrEnabled[Core] > 0)
    {
        Device->isrEnabled[Core]--;
        if (Device->isrEnabled[Core] == 0)
            disable_irq(Device->irqLines[Core]);
    }

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gckGALDEVICE_Start_Threads
**
**  Start the daemon threads.
**
**  INPUT:
**
**      gckGALDEVICE Device
**          Pointer to an gckGALDEVICE object.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      gcvSTATUS_OK
**          Start successfully.
**      gcvSTATUS_GENERIC_IO
**          Start failed.
*/
gceSTATUS
gckGALDEVICE_Start_Threads(
    IN gckGALDEVICE Device
    )
{
    gceSTATUS status;

    gcmkHEADER_ARG("Device=0x%x", Device);

    gcmkVERIFY_ARGUMENT(Device != NULL);

    gcmkONERROR(_StartThread(threadRoutine, gcvCORE_MAJOR));
    gcmkONERROR(_StartThread(threadRoutine, gcvCORE_2D));

    gcmkONERROR(_StartThread(threadRoutine, gcvCORE_VG));

    {
        gctUINTPTR_T i = gcvCORE_3D1;

        for (; i <= gcvCORE_3D3; i++)
        {
            gcmkONERROR(_StartThread(threadRoutine, i));
        }
    }

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckGALDEVICE_Stop_Threads
**
**  Stop the gal device, including the following actions: stop the daemon
**  thread, release the irq.
**
**  INPUT:
**
**      gckGALDEVICE Device
**          Pointer to an gckGALDEVICE object.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      Nothing.
*/
gceSTATUS
gckGALDEVICE_Stop_Threads(
    gckGALDEVICE Device
    )
{
    gctINT i;

    gcmkHEADER_ARG("Device=0x%x", Device);

    gcmkVERIFY_ARGUMENT(Device != NULL);

    for (i = 0; i < gcdMAX_GPU_COUNT; i++)
    {
        /* Stop the kernel threads. */
        if (Device->threadInitializeds[i])
        {
            Device->killThread = gcvTRUE;
            up(&Device->semas[i]);

            kthread_stop(Device->threadCtxts[i]);
            Device->threadCtxts[i]        = gcvNULL;
            Device->threadInitializeds[i] = gcvFALSE;
        }
    }

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;
}

/*******************************************************************************
**
**  gckGALDEVICE_Start
**
**  Start the gal device, including the following actions: setup the isr routine
**  and start the daemoni thread.
**
**  INPUT:
**
**      gckGALDEVICE Device
**          Pointer to an gckGALDEVICE object.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      gcvSTATUS_OK
**          Start successfully.
*/
gceSTATUS
gckGALDEVICE_Start(
    IN gckGALDEVICE Device
    )
{
    gceSTATUS status;
    gctUINT i;

    gcmkHEADER_ARG("Device=0x%x", Device);

    /* Start the kernel thread. */
    gcmkONERROR(gckGALDEVICE_Start_Threads(Device));

    for (i = 0; i < gcvCORE_COUNT; i++)
    {
        if (i == gcvCORE_VG)
        {
            continue;
        }

        if (Device->kernels[i] != gcvNULL)
        {
            /* Setup the ISR routine. */
            gcmkONERROR(gckGALDEVICE_Setup_ISR(i));

            /* Switch to SUSPEND power state. */
            gcmkONERROR(gckHARDWARE_SetPowerManagementState(
                Device->kernels[i]->hardware, gcvPOWER_OFF_ATPOWERON
                ));
        }
    }

    if (Device->kernels[gcvCORE_VG] != gcvNULL)
    {
        /* Setup the ISR routine. */
        gcmkONERROR(gckGALDEVICE_Setup_ISR(gcvCORE_VG));

#if gcdENABLE_VG
        /* Switch to SUSPEND power state. */
        gcmkONERROR(gckVGHARDWARE_SetPowerManagementState(
            Device->kernels[gcvCORE_VG]->vg->hardware, gcvPOWER_OFF_ATPOWERON
            ));
#endif
    }

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckGALDEVICE_Stop
**
**  Stop the gal device, including the following actions: stop the daemon
**  thread, release the irq.
**
**  INPUT:
**
**      gckGALDEVICE Device
**          Pointer to an gckGALDEVICE object.
**
**  OUTPUT:
**
**      Nothing.
**
**  RETURNS:
**
**      Nothing.
*/
gceSTATUS
gckGALDEVICE_Stop(
    gckGALDEVICE Device
    )
{
    gceSTATUS status;
    gctUINT i;

    gcmkHEADER_ARG("Device=0x%x", Device);

    gcmkVERIFY_ARGUMENT(Device != NULL);

    for (i = 0; i < gcvCORE_COUNT; i++)
    {
        if (i == gcvCORE_VG)
        {
            continue;
        }

        if (Device->kernels[i] != gcvNULL)
        {
            /* Switch to OFF power state. */
            gcmkONERROR(gckHARDWARE_SetPowerManagementState(
                Device->kernels[i]->hardware, gcvPOWER_OFF
                ));

            /* Remove the ISR routine. */
            gcmkONERROR(gckGALDEVICE_Release_ISR(i));
        }
    }

    if (Device->kernels[gcvCORE_VG] != gcvNULL)
    {
        /* Setup the ISR routine. */
        gcmkONERROR(gckGALDEVICE_Release_ISR(gcvCORE_VG));

#if gcdENABLE_VG
        /* Switch to OFF power state. */
        gcmkONERROR(gckVGHARDWARE_SetPowerManagementState(
            Device->kernels[gcvCORE_VG]->vg->hardware, gcvPOWER_OFF
            ));
#endif
    }

    /* Stop the kernel thread. */
    gcmkONERROR(gckGALDEVICE_Stop_Threads(Device));

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return status;
}

/*******************************************************************************
**
**  gckGALDEVICE_AddCore
**
**  Add a core after gckGALDevice is constructed.
**
**  INPUT:
**
**  OUTPUT:
**
*/
gceSTATUS
gckGALDEVICE_AddCore(
    IN gckGALDEVICE Device,
    IN gcsDEVICE_CONSTRUCT_ARGS * Args
    )
{
    gceSTATUS status;
    gceCORE core = gcvCORE_COUNT;
    gctUINT i = 0;

    gcmkHEADER();
    gcmkVERIFY_ARGUMENT(Device != gcvNULL);

    /* Find which core is added. */
    for (i = 0; i < gcvCORE_COUNT; i++)
    {
        if (Args->irqs[i] != -1)
        {
            core = i;
            break;
        }
    }

    if (i == gcvCORE_COUNT)
    {
        gcmkPRINT("[galcore]: No valid core information found");
        gcmkONERROR(gcvSTATUS_INVALID_ARGUMENT);
    }


    gcmkPRINT("[galcore]: add core[%d]", core);

    /* Record irq, registerBase, registerSize. */
    Device->irqLines[core] = Args->irqs[core];
    _SetupRegisterPhysical(Device, Args);

    /* Map register memory.*/

    /* Add a platform indepedent framework. */
    gcmkONERROR(gckDEVICE_AddCore(
        Device->device,
        core,
        Args->chipIDs[core],
        Device,
        &Device->kernels[core]
        ));

    /* Start thread routine. */
    _StartThread(threadRoutine, core);

    /* Register ISR. */
    gckGALDEVICE_Setup_ISR(core);

    /* Set default power management state. */
    gcmkONERROR(gckHARDWARE_SetPowerManagementState(
        Device->kernels[core]->hardware, gcvPOWER_OFF_ATPOWERON
        ));

    gcmkFOOTER_NO();
    return gcvSTATUS_OK;

OnError:
    gcmkFOOTER();
    return gcvSTATUS_OK;
}
