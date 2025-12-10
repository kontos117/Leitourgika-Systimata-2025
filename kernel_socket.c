
#include "tinyos.h"
#include "kernel_socket.h"


Fid_t sys_Socket(port_t port)
{
	if(port>MAX_PORT || port<0) return NOFILE;

	FCB* fcb;
	Fid_t fid = 0;

	if(FCB_reserve(1,&fid,&fcb)==0) return NOFILE;

	// Initialise socket
	initialise_Socket(port, fcb);

	return fid;	
}

int sys_Listen(Fid_t sock)
{
	FCB* fcb = get_fcb(sock);
	if(!fcb) return NOFILE;

	// Check for the possible errors

	socket_cb* socket = (socket_cb*) fcb->streamobj;

	if(!socket) return NOFILE;

	if(PORT_MAP[socket->port] || socket->port==NOPORT) return NOFILE;

	if(socket->type!=SOCKET_UNBOUND) return NOFILE;
    
    // Set the PORT_MAP
    PORT_MAP[socket->port] = socket;
    // Mark socket as a listener 
	// initialise the fields of the union
	socket->type = SOCKET_LISTENER;
	rlnode_init(&socket->listener_s.queue, NULL);
	socket->listener_s.req_available = COND_INIT;

	return 0;
}


Fid_t sys_Accept(Fid_t lsock)
{
	FCB* fcb = get_fcb(lsock);
    // check if fcb is legal
    if(!fcb) return NOFILE;

    socket_cb* socket = (socket_cb*) fcb->streamobj;

    // check if it's a socket and a listener
    if(!socket || socket->type != SOCKET_LISTENER) return NOFILE;

    // increase refcount (to protect from Close while waiting)
    socket->refcount++;

    // while waiting for a request
    while(is_rlist_empty(&socket->listener_s.queue)) {
        // 5. Check if the port is still valid (listening socket closed)
        if(!PORT_MAP[socket->port] || socket->refcount == 0) return NOFILE; 
        
        // Wait for a request
        kernel_wait(&socket->listener_s.req_available, SCHED_PIPE);
    }
	socket->refcount--;
	

    // check if the port is still valid (after waking up)
    if(!PORT_MAP[socket->port]) return NOFILE;
    

    // take the first connection request
    rlnode* node = rlist_pop_front(&socket->listener_s.queue);
    connection_request* req = (connection_request*) node-> connection_request;

    req->admitted = 1;

    // client's socket (peer1)
    socket_cb* peer1 = req->peer;
    if(!peer1 || peer1->type != SOCKET_UNBOUND) return NOFILE;
	
    

    // try to construct peer (the server's new socket)

	Fid_t peer2_fid = sys_Socket(peer1->port);
	if(peer2_fid == NOFILE) return NOFILE;
    

	FCB* peer2_fcb = get_fcb(peer2_fid);
	if(peer2_fid==NOFILE) return NOFILE;
	

    socket_cb* peer2 = (socket_cb*) peer2_fcb->streamobj;
	if(!peer2) return NOFILE;
	

    // connect the 2 peers / initialise the connection

    // Create the two pipe control blocks
    pipe_cb* pipe_A_to_B = (pipe_cb*)xmalloc(sizeof(pipe_cb)); // peer1 writes, peer2 reads
    pipe_cb* pipe_B_to_A = (pipe_cb*)xmalloc(sizeof(pipe_cb)); // peer2 writes, peer1 reads

    initialise_SocketPipe(pipe_A_to_B, peer2->fcb, peer1->fcb); // Reader: peer2, Writer: peer1
    initialise_SocketPipe(pipe_B_to_A, peer1->fcb, peer2->fcb); // Reader: peer1, Writer: peer2

	setup_peer_connection(peer1, peer2, pipe_B_to_A, pipe_A_to_B);
    setup_peer_connection(peer2, peer1, pipe_A_to_B, pipe_B_to_A);

    // signal the Connect side
    kernel_signal(&req->connected_cv);
	//socket->refcount--;

    return peer2_fid; // Return the new socket ID for the server
}


int sys_Connect(Fid_t sock, port_t port, timeout_t timeout)
{
    FCB *fcb = get_fcb(sock);
    if(!fcb) return NOFILE;
    
    socket_cb *socket = (socket_cb *)fcb->streamobj;
    
    // do all the checks needed
    if(!socket || socket->type != SOCKET_UNBOUND) return NOFILE;
    if(port < 0 || port > MAX_PORT) return NOFILE;

    socket_cb *listener = PORT_MAP[port];
    // port does not have a listening socket
    if(!listener || listener->type != SOCKET_LISTENER) return NOFILE;

    // increase refcount (protects the socket while waiting)
    socket->refcount++;

    // build the request and initialize it
    connection_request* req = (connection_request*)xmalloc(sizeof(connection_request));
    req->admitted = 0;
    req->peer = socket;
    req->connected_cv = COND_INIT;
    rlnode_init(&req->queue_node, req); // Store the request pointer in the node

    // add request to the listener’s queue and signal listener
    rlist_push_back(&listener->listener_s.queue, &req->queue_node);
    kernel_signal(&listener->listener_s.req_available);

    // block for the specified amount of time (kernel_timedwait)
    int wait_status = kernel_timedwait(&req->connected_cv, SCHED_PIPE, 100*timeout);
    
    // decrease refcount immediately upon waking
    socket->refcount--;
	//fprintf(stderr, "refcount: %d\n", socket->refcount);
	
    // check the wait result and the admitted flag
    int result = -1; // Default to error

    if(wait_status == 0) result = -1; // Timeout error
    else if(wait_status == 1) {

		if(req->admitted == 1) result = 0; // Success
		else result = -1;
	}
	//fprintf(stderr, "req free\n");
	rlist_remove(&req->queue_node);
    free(req);
    return result;
}


int sys_ShutDown(Fid_t sock, shutdown_mode how)
{
	FCB* fcb = get_fcb(sock);
	if(!fcb) return NOFILE;

	socket_cb* socket = (socket_cb*) fcb->streamobj;
	if(socket->type!=SOCKET_PEER) return NOFILE;

	switch(how) {
		case SHUTDOWN_READ:
			// check if socket's reader pipe is still open
			if(socket->peer_s.read_pipe) {
				pipe_reader_close(socket->peer_s.read_pipe);
				socket->peer_s.read_pipe=NULL;
			}
			break;

		case SHUTDOWN_WRITE:
			// check if socket's writing pipe is still open
			if(socket->peer_s.write_pipe) {
				pipe_writer_close(socket->peer_s.write_pipe);
				socket->peer_s.write_pipe=NULL;
			}
			break;

		case SHUTDOWN_BOTH:
			// check if socket's both pipes are still open
			if(socket->peer_s.read_pipe) {
				pipe_reader_close(socket->peer_s.read_pipe);
				socket->peer_s.read_pipe=NULL;
			}		
			if(socket->peer_s.write_pipe) {
				pipe_writer_close(socket->peer_s.write_pipe);
				socket->peer_s.write_pipe=NULL;
			}
			break;

		default:
			return -1;
			break;
	}

	return 0;
}

file_ops socket_file_ops={
	.Open = NULL,
	.Read = socket_read,
	.Write = socket_write,
	.Close = socket_close
};

int socket_read(void* socket_cb_t, char *buf, unsigned int n) 
{
	socket_cb* peer_read = (socket_cb*) socket_cb_t;
	if(!peer_read) return -1;

	// check if read end is active and socket is peer type
	if(!peer_read->peer_s.read_pipe || peer_read->type != SOCKET_PEER) 
		return -1;
	else return pipe_read(peer_read->peer_s.read_pipe, buf, n);
	
}


int socket_write(void* socket_cb_t, const char *buf, unsigned int n)
{
	socket_cb* peer_write = (socket_cb*) socket_cb_t;
	if(!peer_write) return -1;

	// check if write end is active and socket is peer type
	if(!peer_write->peer_s.write_pipe || peer_write->type != SOCKET_PEER)
		return -1;
	else return pipe_write(peer_write->peer_s.write_pipe,buf,n);
}


int socket_close(void* socket_cb_t)
{
	socket_cb* socket = (socket_cb*) socket_cb_t;
	if(!socket) return -1;

	// if socket's type is Listener, clear PORT_MAP and broadcast available requests 
	if(socket->type==SOCKET_LISTENER){
		PORT_MAP[socket->port] = NULL;
		kernel_broadcast(&socket->listener_s.req_available);
		// else if the socket's type is Peer, close the ends needed
	} else if(socket->type==SOCKET_PEER) {
		if(socket->peer_s.read_pipe!=NULL) { 
			pipe_reader_close(socket->peer_s.read_pipe);
		    socket->peer_s.read_pipe=NULL;
		}		
	    if(socket->peer_s.write_pipe!=NULL) {
		    pipe_writer_close(socket->peer_s.write_pipe);
		    socket->peer_s.write_pipe=NULL;
		}
	}
	//fprintf(stderr, "refcount: %d\n", socket->refcount);

	if(socket->refcount==0) {
		//fprintf(stderr, "Socket free\n"); 
		free(socket);
	}
	return 0;
}

void initialise_Socket(port_t port, FCB* fcb)
{
	socket_cb* socketcb = (socket_cb*)xmalloc(sizeof(socket_cb));

	socketcb->refcount = 0;
	socketcb->fcb = fcb;
	socketcb->type = SOCKET_UNBOUND;
	socketcb->port = port;

	fcb->streamfunc = &socket_file_ops;
	fcb->streamobj = socketcb;
}

void initialise_SocketPipe(pipe_cb* pipe, FCB* reader_fcb, FCB* writer_fcb)
{
    pipe->reader = reader_fcb;
    pipe->writer = writer_fcb;
    pipe->w_position = 0;
    pipe->r_position = 0;
    pipe->has_space = COND_INIT; 
    pipe->has_data = COND_INIT;
    
}

void setup_peer_connection(socket_cb* current_peer, socket_cb* remote_peer, 
                             pipe_cb* in_pipe, pipe_cb* out_pipe)
{
    current_peer->type = SOCKET_PEER;
    current_peer->peer_s.peer = remote_peer;
    current_peer->peer_s.read_pipe = in_pipe;  
    current_peer->peer_s.write_pipe = out_pipe; 
}
