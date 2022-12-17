#include "uringserver.h"

#define LIBURING_STYLE 1

#if 1
int
io_uring_setup(u32 entries, struct io_uring_params *p)
{
    return (int) syscall(__NR_io_uring_setup, entries, p);
}

int io_uring_enter(s32 ring_fd, unsigned int to_submit,
		   unsigned int min_complete, unsigned int flags)
{
    return (int) syscall(__NR_io_uring_enter, ring_fd, to_submit, min_complete,
			 flags, NULL, 0);
}
#endif

inline connection_info *
GetNewConnectionInfo(server_state *ServerState)
{
    connection_info *Result;

    if(ServerState->ConnectionsFreeList == 0)
    {
	Result = PushStruct(ServerState->Arena, connection_info);
	ZeroStruct(*Result);
    }
    else
    {
	Result = ServerState->ConnectionsFreeList;
	ServerState->ConnectionsFreeList = Result->Prev;
    }

    Result->Prev =  ServerState->ConnectionsSentinel.Prev;
    Result->Next = &ServerState->ConnectionsSentinel;
    Result->Prev->Next = Result;

    ServerState->ConnectionsSentinel.Prev = Result;

    Result->Alive       = true;
    Result->ServerState = ServerState;

    return(Result);
}

inline void
FreeConnection(connection_info *Connection)
{
    server_state *ServerState = Connection->ServerState;

    Assert(Connection->Alive);
    Connection->Alive = false;
    Connection->Next->Prev = Connection->Prev;
    Connection->Prev->Next = Connection->Next;

    ZeroStruct(*Connection);

    Connection->Prev = ServerState->ConnectionsFreeList;
    ServerState->ConnectionsFreeList = Connection;
}

inline connection_event *
GetNewConnectionEvent(server_state *ServerState)
{
    connection_event *Result;

    if(ServerState->ConnectionEventsFreeList == 0)
    {
	Result = PushStruct(ServerState->Arena, connection_event);
	ZeroStruct(*Result);
    }
    else
    {
	Result = ServerState->ConnectionEventsFreeList;
	ServerState->ConnectionEventsFreeList = Result->Prev;
    }

    Result->Prev =  ServerState->ConnectionEventsSentinel.Prev;
    Result->Next = &ServerState->ConnectionEventsSentinel;
    Result->Prev->Next = Result;
    
    return(Result);
}

inline void
FreeConnectionEvent(server_state *ServerState, connection_event *Event)
{
    Event->Next->Prev = Event->Prev;
    Event->Prev->Next = Event->Next;

    ZeroStruct(*Event);

    Event->Prev = ServerState->ConnectionEventsFreeList;
    ServerState->ConnectionEventsFreeList = Event;
}

s32
CreateQueue(uring_queue *URingQueue)
{
    
    submission_queue_ring *SubmitRing   = &URingQueue->SubmitRing;
    completion_queue_ring *CompleteRing = &URingQueue->CompleteRing;

    struct io_uring_params Params;
    memset(&Params, 0, sizeof(Params));
    URingQueue->FileDescriptor = io_uring_setup(QUEUE_DEPTH, &Params);

    Assert(Params.features & IORING_FEAT_FAST_POLL);
    
    if(URingQueue->FileDescriptor < 0)
    {
	perror("io_uring_setup");
	return(1);
    }
    
    s32 SubmitRingSize   = Params.sq_off.array + Params.sq_entries * sizeof(u32);
    s32 CompleteRingSize = Params.cq_off.cqes  + Params.cq_entries * sizeof(struct io_uring_cqe);

    //NOTE: We can assume this on kernel 5.4 and above
    if(Params.features & IORING_FEAT_SINGLE_MMAP)
    {
	if(SubmitRingSize < CompleteRingSize)
	{
	    SubmitRingSize = CompleteRingSize;
	}
    }

    void *SubmitQueuePtr = mmap(0, SubmitRingSize,
				PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_POPULATE,
				URingQueue->FileDescriptor,
				IORING_OFF_SQ_RING);

    if(SubmitQueuePtr == MAP_FAILED)
    {
	perror("mmap");
	return(1);
    }

    void *CompleteQueuePtr;
    //NOTE: We can assume this on kernel 5.4 and above
    if(Params.features & IORING_FEAT_SINGLE_MMAP)
    {
	CompleteQueuePtr = SubmitQueuePtr;
    }
    else
    {
	CompleteQueuePtr = mmap(0, SubmitRingSize,
				PROT_READ | PROT_WRITE,
				MAP_SHARED | MAP_POPULATE,
				URingQueue->FileDescriptor,
				IORING_OFF_SQ_RING);

	if(CompleteQueuePtr == MAP_FAILED)
	{
	    perror("mmap");
	    return(1);
	}
    }

    SubmitRing->Head        = (u32 *)((char *)SubmitQueuePtr + Params.sq_off.head);
    SubmitRing->Tail        = (u32 *)((char *)SubmitQueuePtr + Params.sq_off.tail);
    SubmitRing->Mask        = (u32 *)((char *)SubmitQueuePtr + Params.sq_off.ring_mask);
    SubmitRing->RingEntries = (u32 *)((char *)SubmitQueuePtr + Params.sq_off.ring_entries);
    SubmitRing->Flags       = (u32 *)((char *)SubmitQueuePtr + Params.sq_off.flags);
    SubmitRing->Array       = (u32 *)((char *)SubmitQueuePtr + Params.sq_off.array);
    SubmitRing->UnsubmittedHead = 0;
    SubmitRing->UnsubmittedTail = 0;

    URingQueue->SubmitQueueEntries = (io_uring_sqe *)mmap(0, Params.sq_entries * sizeof(io_uring_sqe),
							 PROT_READ|PROT_WRITE,
							 MAP_SHARED|MAP_POPULATE,
							 URingQueue->FileDescriptor, IORING_OFF_SQES);
    if(URingQueue->SubmitQueueEntries == MAP_FAILED)
    {
	perror("mmap");
	return(1);
    }

    CompleteRing->Head        = (u32 *)((char *)CompleteQueuePtr + Params.cq_off.head);
    CompleteRing->Tail        = (u32 *)((char *)CompleteQueuePtr + Params.cq_off.tail);
    CompleteRing->Mask        = (u32 *)((char *)CompleteQueuePtr + Params.cq_off.ring_mask);
    CompleteRing->RingEntries = (u32 *)((char *)CompleteQueuePtr + Params.cq_off.ring_entries);
    CompleteRing->CompleteQueueEntries = (io_uring_cqe *)((char *)CompleteQueuePtr +
							  Params.cq_off.cqes);
    return(0);
}


u32
SubmitEntries(uring_queue *URingQueue)
{
    u32 Result = 0;
#if LIBURING_STYLE   
    submission_queue_ring *SubmitRing = &URingQueue->SubmitRing;
    ReadBarrier();

    if(*SubmitRing->Head != *SubmitRing->Tail)
    {
	Result = *SubmitRing->RingEntries;
    }
    else
    {
	if(SubmitRing->UnsubmittedHead == SubmitRing->UnsubmittedTail)
	{
	    return(0);
	}

	u32 Tail     = *SubmitRing->Tail;
	u32 NextTail =  Tail;
	u32 Mask     = *SubmitRing->Mask;    

	u32 ToSubmit = SubmitRing->UnsubmittedTail - SubmitRing->UnsubmittedHead;
	while(ToSubmit--)
	{
	    NextTail++;

	    ReadBarrier();

	    u32 ArrayIndex = Tail & Mask;
	    SubmitRing->Array[ArrayIndex] = SubmitRing->UnsubmittedHead & Mask;
	    Tail = NextTail;

	    SubmitRing->UnsubmittedHead++;
	    Result++;
	}

	if(!Result)
	{
	    return(0);
	}

	if(Tail != *SubmitRing->Tail)
	{
	    WriteBarrier();
	    *SubmitRing->Tail = Tail;
	    WriteBarrier();
	}
    }

    s32 Submitted = io_uring_enter(URingQueue->FileDescriptor, Result, 1,
				   IORING_ENTER_GETEVENTS);
    Assert(Submitted == Result);    
#else
    Result = __atomic_exchange_n(&URingQueue->SubmissionCount, 0, __ATOMIC_SEQ_CST);
#endif
    return(Result);
}

io_uring_sqe *
GetNewSubmissionEntry(uring_queue *URingQueue)
{
    submission_queue_ring *SubmitRing = &URingQueue->SubmitRing;

    printf("SubmitRing: Head: %u, Tail: %u, Entries: %u\n",
	   *SubmitRing->Head,
	   *SubmitRing->Tail,
	   *SubmitRing->RingEntries);
    
    io_uring_sqe *Result = {};

#if LIBURING_STYLE
    u32 NextTail = SubmitRing->UnsubmittedTail + 1;
    if((NextTail - SubmitRing->UnsubmittedHead) > *SubmitRing->RingEntries)
    {
	Assert(!"submit queue is full\n");
    }
    else
    {
	u32 Index = SubmitRing->UnsubmittedTail & *SubmitRing->Mask;
	Result = URingQueue->SubmitQueueEntries + Index;
	SubmitRing->UnsubmittedTail = NextTail;
    }
#else
    u32 Tail = *SubmitRing->Tail;
    u32 NextTail = Tail + 1;
    
    ReadBarrier();
    u32 Index = Tail & *SubmitRing->Mask;    
    Result = URingQueue->SubmitQueueEntries + Index;    
    SubmitRing->Array[Index] = Index;
    
    if(*SubmitRing->Tail != NextTail)
    {
	*SubmitRing->Tail = NextTail;
	WriteBarrier();
    }
    
    __atomic_add_fetch(&URingQueue->SubmissionCount, 1, __ATOMIC_SEQ_CST);    
#endif
    
    return(Result);    
}

void
AddAcceptEntry(server_state *ServerState)
{
    io_uring_sqe *SubmitQueueEntry = GetNewSubmissionEntry(&ServerState->URingQueue);
    
    connection_event *Event = GetNewConnectionEvent(ServerState);
    Event->Type = Event_Accept;
    Event->BytesToTransfer = sizeof(sockaddr_in);
	
    SubmitQueueEntry->fd        = ServerState->ListenSocket;
    SubmitQueueEntry->flags     = 0;
    SubmitQueueEntry->opcode    = IORING_OP_ACCEPT;
    SubmitQueueEntry->addr      = (u32 long)&Event->Buffer;
    SubmitQueueEntry->len       = 0;
    SubmitQueueEntry->off       = (u32 long)&Event->BytesToTransfer;
    SubmitQueueEntry->user_data = (u32 long)Event;
}

void
AddReadEntry(server_state *ServerState, connection_info *Connection)
{
    connection_event *NewEvent = GetNewConnectionEvent(ServerState);
    NewEvent->Type       = Event_Read;
    NewEvent->Connection = Connection;

    io_uring_sqe *SubmitQueueEntry = GetNewSubmissionEntry(&ServerState->URingQueue);
    SubmitQueueEntry->fd        = Connection->Socket;
    SubmitQueueEntry->flags     = 0;
    SubmitQueueEntry->opcode    = IORING_OP_READ;
    SubmitQueueEntry->addr      = (u32 long)NewEvent->Buffer;
    SubmitQueueEntry->len       = DATA_BUFFER_SIZE;
    SubmitQueueEntry->off       = 0;
    SubmitQueueEntry->user_data = (u32 long)NewEvent;
}
void
ContinueReadEvent(server_state *ServerState, connection_event *Event)
{
    io_uring_sqe *SubmitQueueEntry = GetNewSubmissionEntry(&ServerState->URingQueue);    
    SubmitQueueEntry->fd        = Event->Connection->Socket;
    SubmitQueueEntry->flags     = 0;
    SubmitQueueEntry->opcode    = IORING_OP_READ;
    SubmitQueueEntry->addr      = (u32 long)Event->Buffer + Event->BytesTransferred;
    SubmitQueueEntry->len       = DATA_BUFFER_SIZE - Event->BytesTransferred;
    SubmitQueueEntry->off       = 0;
    SubmitQueueEntry->user_data = (u32 long)Event;
}

//NOTE: this is so we can see the address of the sender 
struct recv_msg_buffer
{
    msghdr      Header;
    sockaddr_in Address;
    iovec       IOVector;
    u8          Buffer[0];
};

void
AddUDPReadEntry(server_state *ServerState, connection_info *Connection)
{
    connection_event *NewEvent = GetNewConnectionEvent(ServerState);
    NewEvent->Type       = Event_UDPRead;
    NewEvent->Connection = Connection;

    recv_msg_buffer *MessageBuffer = (recv_msg_buffer *)NewEvent->Buffer;
    *MessageBuffer = {};
    
    MessageBuffer->Header.msg_name    = &MessageBuffer->Address;
    MessageBuffer->Header.msg_namelen = sizeof(MessageBuffer->Address);
    MessageBuffer->Header.msg_iov     = &MessageBuffer->IOVector;
    MessageBuffer->Header.msg_iovlen  = 1;

    MessageBuffer->IOVector.iov_base = MessageBuffer->Buffer;
    MessageBuffer->IOVector.iov_len  = DATA_BUFFER_SIZE - sizeof(recv_msg_buffer);
    
    io_uring_sqe *SubmitQueueEntry = GetNewSubmissionEntry(&ServerState->URingQueue);
    SubmitQueueEntry->fd        = ServerState->UDPListenSocket;
    SubmitQueueEntry->flags     = 0;
    SubmitQueueEntry->opcode    = IORING_OP_RECVMSG;
    SubmitQueueEntry->addr      = (u32 long)MessageBuffer;
    SubmitQueueEntry->len       = DATA_BUFFER_SIZE;
    SubmitQueueEntry->off       = 0;
    SubmitQueueEntry->user_data = (u32 long)NewEvent;
}

void
AddWriteEntry(server_state *ServerState, connection_info *Connection, u8 *Data, u32 BytesToSend)
{
    connection_event *NewEvent = GetNewConnectionEvent(ServerState);
    NewEvent->Type       = Event_Write;
    NewEvent->Connection = Connection;
    NewEvent->BytesToTransfer = BytesToSend;
    DataCopy(Data, NewEvent->Buffer, BytesToSend);

    io_uring_sqe *SubmitQueueEntry = GetNewSubmissionEntry(&ServerState->URingQueue);
    SubmitQueueEntry->fd        = NewEvent->Connection->Socket;
    SubmitQueueEntry->flags     = 0;
    SubmitQueueEntry->opcode    = IORING_OP_WRITE;
    SubmitQueueEntry->addr      = (u32 long)NewEvent->Buffer;
    SubmitQueueEntry->len       = NewEvent->BytesToTransfer;
    SubmitQueueEntry->off       = 0;
    SubmitQueueEntry->user_data = (u32 long)NewEvent;
}

void
ContinueWriteEvent(server_state *ServerState, connection_event *Event)
{
    io_uring_sqe *SubmitQueueEntry = GetNewSubmissionEntry(&ServerState->URingQueue);
    SubmitQueueEntry->fd        = Event->Connection->Socket;
    SubmitQueueEntry->flags     = 0;
    SubmitQueueEntry->opcode    = IORING_OP_WRITE;
    SubmitQueueEntry->addr      = (u32 long)Event->Buffer + Event->BytesTransferred;
    SubmitQueueEntry->len       = Event->BytesToTransfer - Event->BytesTransferred;
    SubmitQueueEntry->off       = 0;
    SubmitQueueEntry->user_data = (u32 long)Event;
}

void
ProcessQueue(server_state *ServerState)
{
    uring_queue *URingQueue = &ServerState->URingQueue;
    s32 ListenSocket = ServerState->ListenSocket;    
    completion_queue_ring *CompleteRing = &URingQueue->CompleteRing;
    
    u32 Head = *CompleteRing->Head;
    u32 NextHead = Head + 1;
    
    s32 EntriesProcessed = 0;
    do
    {
	ReadBarrier();
	if(Head == *CompleteRing->Tail)
	{
	    printf("nothing in queue\n");
	    break;
	}
	
	u32 Index = Head & *CompleteRing->Mask;
	io_uring_cqe *Entry = CompleteRing->CompleteQueueEntries + Index;

	printf("EntryResult: %d\n", Entry->res);

	connection_event *Event = (connection_event *)Entry->user_data;

	printf("Eventype: %d\n", Event->Type);	
	if(Entry->res < 0)
	{
	    printf("CQE Entry without result. error: %s\n", strerror(-Entry->res));
	    if(Entry->res == -104)
	    {
		//NOTE: Connection reset by peer
		Assert(Event->Connection);
		if(Event->Connection)
		{
		    printf("Disconnecting");
		    
		    close(Event->Connection->Socket);
		    FreeConnection(Event->Connection);
		    FreeConnectionEvent(ServerState, Event);
		    goto CONTINUE;
		}
	    }
	    Assert(0);
	}

	switch(Event->Type)
	{
	    case Event_Accept:
	    {
		AddAcceptEntry(ServerState);
		
		Assert(!Event->Connection);		
		printf("Client accepted with socket: %d\n", Entry->res);
		connection_info *NewConnection = GetNewConnectionInfo(ServerState);
		NewConnection->Alive   = true;
		NewConnection->Socket  = Entry->res;
		NewConnection->Address = *(sockaddr_in *)Event->Buffer;
		
		AddReadEntry(ServerState, NewConnection);		
	    } break;

            case Event_UDPRead:
            {
		recv_msg_buffer *RecvMsg = (recv_msg_buffer *)Event->Buffer;
		char *IPString = inet_ntoa(RecvMsg->Address.sin_addr);
		printf("UDP Address: %s\n", IPString);
		u32 Hex = *(u32 *)RecvMsg->Buffer;

		u32 BreakHere = 34;
	    } break;
	    
	    case Event_Read:
	    {
		Assert(Event->Connection);		
		
		printf("BytesReceived: %d\n", Entry->res);
		if(Entry->res == 0)
		{
		    //NOTE: Connection closed
		    printf("Socket: %d disconnected\n", Event->Connection->Socket);
		    close(Event->Connection->Socket);
		    FreeConnection(Event->Connection);
		    FreeConnectionEvent(ServerState, Event);
		}
		else
		{
#if 1
		    u8 *Data = Event->Buffer;
		    u32 BytesTransferred = Entry->res;
		    printf("0x%08x\n", *(u32 *)Event->Buffer);

		    //NOTE: Send back
		    AddWriteEntry(ServerState, Event->Connection, Event->Buffer, BytesTransferred);
			    
		    
		    AddReadEntry(ServerState, Event->Connection);
		    FreeConnectionEvent(ServerState, Event);
		    
#else //NOTE: for testing
		    
		    encrypted_live_message *EncryptedLiveMessage = (encrypted_live_message *)Event->Buffer;
		    encrypted_live_message_header *Header = &EncryptedLiveMessage->Header;

		    u32 BytesTransferred = Entry->res;
		    while(BytesTransferred > 0)
		    {
			if(Header->Size > BytesTransferred)
			{
			    if(Event->BytesToTransfer == 0)
			    {
				printf("New transfer: %u\n", Header->Size);
				Event->BytesToTransfer = Header->Size;
				Event->BytesTransferred = 0;
			    }

			    Event->BytesTransferred += BytesTransferred;
			    printf("ToTransfer: %u, ThisTime: %u, Totasl: %u\n",
				   Event->BytesToTransfer, BytesTransferred, Event->BytesTransferred);

			    if(Event->BytesTransferred < Event->BytesToTransfer)
			    {
				ContinueReadEvent(ServerState, Event);
				goto CONTINUE;
			    }
			}
			
			if(Event->BytesToTransfer > BytesTransferred)
			{
			    BytesTransferred = Event->BytesToTransfer;
			}
			Event->BytesToTransfer = 0;

			u8 *Dest = (u8 *)malloc(Header->Size);
			u8 *Source = (u8 *)(Header + 1);

			DataCopy(Source, Dest, Header->Size);
			//NOTE: Decrypt
			b32 DataValid = false;
			if(DataValid)
			{			
			    //NOTE: process data
			}

			u32 Size = Header->Size;
			BytesTransferred -= Size;
			if(BytesTransferred > 0)
			{
			    EncryptedLiveMessage = (encrypted_live_message *)((u8 *)EncryptedLiveMessage + Size);
			    Header = &EncryptedLiveMessage->Header;
			}
		    }
		    Assert(BytesTransferred = 0);
		    
		    AddReadEntry(ServerState, Event->Connection);
		    FreeConnectionEvent(ServerState, Event);
#endif
		}
	    } break;
	    case Event_Write:
	    {
		u32 BytesSent = Entry->res;
		printf("Sendevent. BytesSent: %u\n", BytesSent);

		Event->BytesTransferred += BytesSent;
		if(Event->BytesTransferred < Event->BytesToTransfer)
		{
		    printf("CONTINUING RIGHT EVENT\n");
                    //TODO: Continue transfer		    
		}
		else
		{
		    FreeConnectionEvent(ServerState, Event);
		}
	    } break;

	    InvalidDefaultCase;
	}

CONTINUE:
	if(*CompleteRing->Head != NextHead)
	{
	    *CompleteRing->Head = NextHead;
	    WriteBarrier();
	}
	
	EntriesProcessed++;
	Head++;
	NextHead++;
    } while(1);

    *CompleteRing->Head = Head;
    WriteBarrier();
//    printf("EntriesProcessed: %d\n", EntriesProcessed);
}

s32
main(void)
{
    printf("hello world\n");

    u32 ServerMemorySize = Megabytes(50);
    memory_arena ServerArena = {};
    ServerArena.Size = ServerMemorySize;
    ServerArena.Base = (u8 *)malloc(ServerArena.Size);

    server_state *ServerState = PushStruct(&ServerArena, server_state);
    ZeroStruct(*ServerState);

    ServerState->Arena = &ServerArena;
    ServerState->ConnectionsSentinel.Next = &ServerState->ConnectionsSentinel;
    ServerState->ConnectionsSentinel.Prev = &ServerState->ConnectionsSentinel;
    ServerState->ConnectionEventsSentinel.Next = &ServerState->ConnectionEventsSentinel;
    ServerState->ConnectionEventsSentinel.Prev = &ServerState->ConnectionEventsSentinel;

    //NOTE: TCP Socket
    ServerState->ListenSocket = socket(AF_INET, SOCK_STREAM, 0);
    if(ServerState->ListenSocket == -1)
    {
	printf("socket() error\n");
	InvalidCodePath;
	return(1);
    }
    
    int True = 1;
    setsockopt(ServerState->ListenSocket, SOL_SOCKET, SO_REUSEADDR, &True, sizeof(True));

    sockaddr_in Address = {};
    Address.sin_family      = AF_INET;
    Address.sin_addr.s_addr = htonl(INADDR_ANY);
    Address.sin_port        = htons(PORT);

    if(bind(ServerState->ListenSocket, (sockaddr *)&Address, sizeof(Address)) < 0)
    {
	printf("bind() error\n");
	InvalidCodePath;
	return(1);
    }

    if(listen(ServerState->ListenSocket, 10) < 0)

    {
	printf("listen() error\n");
	Assert(0);
	return(1);
    }

    
    uring_queue *URingQueue = &ServerState->URingQueue;
    CreateQueue(URingQueue);

    AddAcceptEntry(ServerState);

//    AddReadEntry(ServerState, 0);
    
    do
    {
#if LIBURING_STYLE
	s32 SubmissionCount = SubmitEntries(URingQueue);
	Assert(SubmissionCount >= 0);
#else
	s32 SubmissionCount = SubmitEntries(URingQueue);
	if(io_uring_enter(URingQueue->FileDescriptor, SubmissionCount,
			  1, IORING_ENTER_GETEVENTS) < 0)
	{
	    printf("io_uring_enter");
	    return 1;
	}
#endif
	
	ProcessQueue(ServerState);
	sleep(1);
    } while(1);
    
    return(0);
}
