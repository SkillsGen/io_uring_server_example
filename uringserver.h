#include <stdio.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <arpa/inet.h>
#include <linux/fs.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>

#include "linux_utils.h"
//#include <liburing.h>

#include <linux/io_uring.h>

#define QUEUE_DEPTH  256
#define PORT        5150

#define BLOCK_SZ    1024

#define ReadBarrier()    __asm__ __volatile__("":::"memory");
#define WriteBarrier()   __asm__ __volatile__("":::"memory");

struct submission_queue_ring
{
    u32 *Head;
    u32 *Tail;
    u32 *Mask;
    u32 *RingEntries;
    u32 *Flags;
    u32 *Array;

    u32 UnsubmittedHead;
    u32 UnsubmittedTail;
};

struct completion_queue_ring
{
    u32 *Head;
    u32 *Tail;
    u32 *Mask;
    u32 *RingEntries;
    struct io_uring_cqe *CompleteQueueEntries;
};

struct uring_queue
{
    s32 FileDescriptor;
#if LIBURING_STYLE
    s32 SubmissionCount;
#endif
    submission_queue_ring  SubmitRing;
    struct io_uring_sqe   *SubmitQueueEntries;
    completion_queue_ring  CompleteRing;
};

enum event_type
{
    Event_None,
    
    Event_Accept,
    Event_Read,
    Event_Write,

    Event_UDPAccept,
    Event_UDPRead,
    Event_UDPWrite,
};

struct entry_userdata
{
    event_type Type;
    
    sockaddr_in Address;
    socklen_t   AddressLength;
    s32 Socket;

    void *ReadBuffer; //TODO: Fixed kernel mapped buffers
    void *SendBuffer; //TODO: Fixed kernel mapped buffers
    u32   BufferSize;
    u32   BytesToSend;
};

#define DATA_BUFFER_SIZE 4*1024*1024
struct connection_event
{
    connection_event *Next;
    connection_event *Prev;
    
    event_type Type;
    struct connection_info *Connection;

    u32 BytesToTransfer;
    u32 BytesTransferred;
    u8  Buffer[DATA_BUFFER_SIZE];
};

struct connection_info
{
    b32 Alive;
    
    s32 Socket;
    sockaddr_in Address;   //NOTE: do not change, an accept event uses the size of sockaddr_in

    struct server_state *ServerState;
    
    connection_info *Next;
    connection_info *Prev;
};

struct server_state
{
    memory_arena *Arena;
    uring_queue URingQueue;

    s32 ListenSocket;
    s32 UDPListenSocket;
    
    connection_info  ConnectionsSentinel;
    connection_info *ConnectionsFreeList;

    connection_event  ConnectionEventsSentinel;
    connection_event *ConnectionEventsFreeList;
};

