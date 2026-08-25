(sec-backends-upcie-mproc)=
# uPCIe (multi-process)

The **upcie** backend supports a **multi-process** mode in which several
independent processes share NVMe controllers. One process **owns** a
controller: it opened the device, allocated the DMA memory, and holds the
admin queue. Any number of **clients** attach to what it owns and drive
their own I/O without re-initializing anything.

Ownership is not something a library user falls into. The server is
{ref}`sec-tools-homi`, which exists to be one, and a client that finds
nobody serving simply runs on its own.

```bash
# The server holds the controller for as long as it runs
homi start --shm_id 1 --be upcie 0000:03:00.0

# Clients name the same id and attach to it
xnvme info 0000:03:00.0 --be upcie --shm_id 1
```

(sec-backends-upcie-mproc-model)=
## Process model

### Rendezvous

An server listens on a unix socket named for the identifier clients pass,
`/tmp/xnvme-homi-<shm_id>.sock`. A client derives the same name from
`--shm_id` and connects; finding nothing there means nobody is serving that
identifier, and the process builds its own runtime instead.

A socket rather than a shared segment because a segment cannot carry a file
descriptor, and under `vfio-pci` a descriptor is the only thing that allocations
access to a device. It is a filesystem path rather than an abstract name
because an abstract address cannot be reached from another network namespace,
which a containerised client would need.

### What crosses at attach

Descriptors, and offsets into what they describe:

- the DMA heap, which the client maps to see the same memory;
- BAR0, so that the client rings its own doorbells;
- the offset of a **runtime record**, written once by the server, naming the
  controller and where to find a description of the heap.

The heap description carries the physical address of each granule. The server
read those when it allocated, which needs `CAP_SYS_ADMIN`; leaving them where
the client will map them anyway is what lets an unprivileged client
translate at all.

### What a client does for itself, and what it asks for

A client submits I/O on queues it was allocated, ringing its own doorbell.
Nothing is on the socket during I/O.

Everything else is a request, because the resources belong to the server:

- **Queues.** Creating one means an admin command and memory from the heap, so
  the client asks and receives an identifier plus the offsets of its
  submission queue, completion queue and PRP scratch.
- **Memory.** The heap's allocator is the server's and its free list has no
  lock, so `xnvme_buf_alloc()` asks for an offset.
- **Admin commands.** There is one admin queue and it belongs to the server.
  The payload does not travel with the request: the command names an address
  the device can already reach, so an identify lands in the client's own
  buffer and only the command and its completion cross.

### Liveness

The connection is the liveness signal. A client that exits cleanly hands its
queues and memory back; one that is killed mid-command does not, and the
socket closing is what tells the server to reclaim them, queues first so that
the controller loses its reach on an address before the address stops
resolving.

Nothing is left behind for anyone to clean up, and nothing has to be told
apart from debris: a socket that answers has a process behind it.

(sec-backends-upcie-mproc-vulns)=
## Limitations

### No isolation between processes

A client maps BAR0 and can therefore ring any doorbell and write `CC`, which
is to say it can reset the controller. Under `vfio-pci` it also holds the
device descriptor. Clients are inside one trust domain by construction;
where that is unacceptable, the answer is the kernel driver, which arbitrates
because it owns the device.

### One controller per server

The protocol names no controller, so a server serves the one it holds. Serving
several from one process is not implemented and is refused rather than guessed
at.

### The server has to be answering

Attaching, allocating and admin all depend on the server being responsive.
Under the arrangement this replaced, a server stuck in a poll loop blocked
nobody, because those were reads of shared memory. That is the price of having
one place where a policy could be applied and of a record that needs no lock.

### GPU clients

A controller behind an IOMMU cannot yet DMA into VRAM: `IOMMU_IOAS_MAP_FILE`
does not accept dma-bufs exported by CUDA or HIP. GPU workloads therefore stay
on `uio_pci_generic` until that changes, and the vfio path serves
CPU-submitted I/O into host memory.
