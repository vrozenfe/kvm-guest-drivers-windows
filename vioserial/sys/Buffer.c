#include "precomp.h"
#include "vioser.h"

#if defined(EVENT_TRACING)
#include "Buffer.tmh"
#endif

// Number of ring descriptors that fits into one page.
#define INDIRECT_DESCRIPTORS 256

PPORT_BUFFER
VIOSerialAllocateBuffer(
    IN size_t buf_size
)
{
    PPORT_BUFFER buf;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "--> %s\n", __FUNCTION__);

    buf = ExAllocatePoolWithTag(
                                 NonPagedPool,
                                 sizeof(PORT_BUFFER),
                                 VIOSERIAL_DRIVER_MEMORY_TAG
                                 );
    if (buf == NULL)
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_QUEUEING, "ExAllocatePoolWithTag failed, %s::%d\n", __FUNCTION__, __LINE__);
        return NULL;
    }
    buf->va_buf = ExAllocatePoolWithTag(
                                 NonPagedPool,
                                 buf_size,
                                 VIOSERIAL_DRIVER_MEMORY_TAG
                                 );
    if(buf->va_buf == NULL)
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_QUEUEING, "ExAllocatePoolWithTag failed, %s::%d\n", __FUNCTION__, __LINE__);
        ExFreePoolWithTag(buf, VIOSERIAL_DRIVER_MEMORY_TAG);
        return NULL;
    }
    buf->pa_buf = MmGetPhysicalAddress(buf->va_buf);
    buf->len = 0;
    buf->offset = 0;
    buf->size = buf_size;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "<-- %s\n", __FUNCTION__);
    return buf;
}

size_t VIOSerialSendBuffers(IN PVIOSERIAL_PORT Port,
                            IN PVOID Buffer,
                            IN size_t Length)
{
    PHYSICAL_ADDRESS HighestAcceptable;
    PVOID InDirectBuffer;
    struct virtqueue *vq = GetOutQueue(Port);
    struct VirtIOBufferDescriptor sg[INDIRECT_DESCRIPTORS];
    int ret;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_WRITE,
        "--> %s Buffer: %p Length: %d\n", __FUNCTION__, Buffer, Length);

    if (BYTES_TO_PAGES(Length) > INDIRECT_DESCRIPTORS)
    {
        return 0;
    }

    HighestAcceptable.QuadPart = 0xFFFFFFFFFF;
    InDirectBuffer = MmAllocateContiguousMemory(PAGE_SIZE, HighestAcceptable);
    if (InDirectBuffer)
    {
        PVOID buffer = Buffer;
        size_t length = Length;
        int i = 0;

        while (length > 0)
        {
            sg[i].physAddr = MmGetPhysicalAddress(buffer);
            sg[i].ulSize = min(length, PAGE_SIZE);

            buffer = (PVOID)((LONG_PTR)buffer + sg[i].ulSize);
            length -= sg[i].ulSize;
            i += 1;
        }

        WdfSpinLockAcquire(Port->OutVqLock);
        VIOSerialReclaimConsumedBuffers(Port);
        ret = vq->vq_ops->add_buf(vq, sg, i, 0, Buffer,
            InDirectBuffer, MmGetPhysicalAddress(InDirectBuffer).QuadPart);
        vq->vq_ops->kick(vq);

        if (ret < 0)
        {
            Length = 0;
            TraceEvents(TRACE_LEVEL_ERROR, DBG_WRITE,
                "Error adding buffer to queue (ret = %d)\n", ret);
        }
        else if (ret == 0)
        {
            Port->OutVqFull = TRUE;
        }

        WdfSpinLockRelease(Port->OutVqLock);
        MmFreeContiguousMemory(InDirectBuffer);
    }
    else
    {
        Length = 0;
        TraceEvents(TRACE_LEVEL_ERROR, DBG_WRITE,
            "Error allocating contiguous memory.\n");
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_WRITE, "<-- %s\n", __FUNCTION__);

    return Length;
}

VOID
VIOSerialFreeBuffer(
    IN PPORT_BUFFER buf
)
{
    ASSERT(buf);
    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "--> %s  buf = %p, buf->va_buf = %p\n", __FUNCTION__, buf, buf->va_buf);
    if (buf->va_buf)
    {
        ExFreePoolWithTag(buf->va_buf, VIOSERIAL_DRIVER_MEMORY_TAG);
        buf->va_buf = NULL;
    }
    ExFreePoolWithTag(buf, VIOSERIAL_DRIVER_MEMORY_TAG);
    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "<-- %s\n", __FUNCTION__);
}

VOID
VIOSerialReclaimConsumedBuffers(
    IN PVIOSERIAL_PORT port
)
{
    PVOID buf;
    UINT len;
    struct virtqueue *vq = GetOutQueue(port);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "--> %s\n", __FUNCTION__);

    while (vq && (buf = vq->vq_ops->get_buf(vq, &len)) != NULL)
    {
        KeStallExecutionProcessor(100);
        port->OutVqFull = FALSE;
    }
    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "<-- %s port->OutVqFull = %d\n", __FUNCTION__, port->OutVqFull);
}

// this procedure must be called with port InBuf spinlock held
SSIZE_T
VIOSerialFillReadBufLocked(
    IN PVIOSERIAL_PORT port,
    IN PVOID outbuf,
    IN SIZE_T count
)
{
    PPORT_BUFFER buf;
    NTSTATUS  status = STATUS_SUCCESS;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "--> %s\n", __FUNCTION__);

    if (!count || !VIOSerialPortHasDataLocked(port))
        return 0;

    buf = port->InBuf;
    count = min(count, buf->len - buf->offset);

    RtlCopyMemory(outbuf, (PVOID)((LONG_PTR)buf->va_buf + buf->offset), count);

    buf->offset += count;

    if (buf->offset == buf->len)
    {
        port->InBuf = NULL;

        status = VIOSerialAddInBuf(GetInQueue(port), buf);
        if (!NT_SUCCESS(status))
        {
           TraceEvents(TRACE_LEVEL_ERROR, DBG_QUEUEING, "%s::%d  VIOSerialAddInBuf failed\n", __FUNCTION__, __LINE__);
        }
    }
    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "<-- %s\n", __FUNCTION__);
    return count;
}


NTSTATUS
VIOSerialAddInBuf(
    IN struct virtqueue *vq,
    IN PPORT_BUFFER buf)
{
    NTSTATUS  status = STATUS_SUCCESS;
    struct VirtIOBufferDescriptor sg;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "--> %s  buf = %p\n", __FUNCTION__, buf);
    if (buf == NULL)
    {
        ASSERT(0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (vq == NULL)
    {
        ASSERT(0);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    sg.physAddr = buf->pa_buf;
    sg.ulSize = buf->size;

    if(0 > vq->vq_ops->add_buf(vq, &sg, 0, 1, buf, NULL, 0))
    {
        TraceEvents(TRACE_LEVEL_ERROR, DBG_QUEUEING, "<-- %s cannot add_buf\n", __FUNCTION__);
        status = STATUS_INSUFFICIENT_RESOURCES;
    }

    vq->vq_ops->kick(vq);
    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "<-- %s\n", __FUNCTION__);
    return status;
}


PVOID
VIOSerialGetInBuf(
    IN PVIOSERIAL_PORT port
)
{
    PPORT_BUFFER buf = NULL;
    struct virtqueue *vq = GetInQueue(port);
    UINT len;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "--> %s\n", __FUNCTION__);

    if (vq)
    {
        buf = vq->vq_ops->get_buf(vq, &len);
        if (buf)
        {
           buf->len = len;
           buf->offset = 0;
        }
    }
    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_QUEUEING, "<-- %s\n", __FUNCTION__);
    return buf;
}
