# io_uring_server_example
io_uring example

g++ -g uringserver.cpp

This is just a cleaned up version of a program i made for testing. It
might not include any fixes that ended up in the full program, so it's
just for demonstration/getting a feel for it.
The program first queues an Event_Accept entry. When this returns, it
accepts the connection by grabbing a connection_info struct and
associating that with the socket and all further events on that socket.
Then a Event_Read entry is queued on that socket with a connection_event
struct that has the data buffer aswell as the connection_info
It will echo back any data sent to it with an Event_Write.
The UDP socket is not initialized.

#including liburing.h sometimes doesn't work so that functionality is
reproduced in the main file in two different ways. LIBURING_STYLE is
fine and probably preferred.

the linux_utils.h is just for my convenience.