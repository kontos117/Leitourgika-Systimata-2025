
#include "tinyos.h"
#include "kernel_streams.h"
#include "kernel_cc.h"	


int sys_Pipe(pipe_t* pipe)
{
	if(pipe == NULL) return -1;

	FCB* fcb[2];
	Fid_t fid[2];

	// Fails to reserve FCB
	if(!FCB_reserve(2, fid, fcb)) return -1; 

	initialize_Pipe(fcb);
	
	pipe->read = fid[0];
	pipe->write = fid[1];

	return 0;
}

int pipe_write(void* pipecb_t, const char *buf, unsigned int n) 
{
	int written_from_buf = 0;
	
	pipe_cb* pipe = (pipe_cb*) pipecb_t;
	// check if reader and writer are open or the pipe is null
	if(!pipe->reader || !pipe->writer || !pipe)
		return -1;

	/*  
		Loop while buffer is full and if the reader is open.
		The buffer is full when no position is available. The positions available are given by 
		the equation available = w_position - r_position and because we have a circular buffer
		we need to do the modulo of PIPE_BUFFER_SIZE
	*/

	while(available_space(pipe, 1) == 0 && pipe->reader) {
		kernel_wait(&pipe->has_space, SCHED_PIPE);
	}

	// check if reader closed
	if(!pipe->reader) return -1;
	
	// while buffer isn't full and the characters written from the buf are fewer than n
	
	while(available_space(pipe, 1) != 0 && written_from_buf < n) {
		// Copy data
		pipe->BUFFER[pipe->w_position] = buf[written_from_buf];
		// Increase indices
		written_from_buf++;
		pipe->w_position++;  
		
		// Check if the writing reached buffer's end and set it 0 to start again
		if(pipe->w_position >= PIPE_BUFFER_SIZE) pipe->w_position = 0;
	}
	kernel_broadcast(&pipe->has_data);
	return written_from_buf;
}

int pipe_read(void* pipecb_t, char *buf, unsigned int n)
{
	int read_from_buf = 0;

	pipe_cb* pipe = (pipe_cb*) pipecb_t;
	// check if reader has exited or pipe is null
	if(!pipe->reader || !pipe) return -1;

	// while buffer is empty and the writter open, wait
	while(available_space(pipe, 0) == 0 && pipe->writer && pipe->writer) {
		kernel_wait(&pipe->has_data, SCHED_PIPE);
	}
	
	// while buffer isn't empty and the characters read 
	// from the buf are fewer than n
	while(available_space(pipe, 0) != 0 && read_from_buf < n) {
		// Copy data
		buf[read_from_buf] = pipe->BUFFER[pipe->r_position];
		//Increase indices
		read_from_buf++;
		pipe->r_position++;
		// Check if reading reached buffer's size and set it 0 to start again
		if(pipe->r_position >= PIPE_BUFFER_SIZE) pipe->r_position = 0;
	}
	kernel_broadcast(&pipe->has_space);
	return read_from_buf;
}

int pipe_writer_close(void* _pipecb)
{
	pipe_cb* pipe = (pipe_cb*) _pipecb;
	if(pipe == NULL) return -1;

	pipe->writer = NULL;
	kernel_broadcast(&pipe->has_data);
	if(pipe->reader == NULL) free(pipe);
	return 0;
}

int pipe_reader_close(void* _pipecb)
{
	pipe_cb* pipe = (pipe_cb*) _pipecb;
	if(pipe == NULL) return -1;

	pipe->reader = NULL;
	kernel_broadcast(&pipe->has_space);
	if(pipe->writer == NULL) free(pipe);
	return 0;
}

static file_ops reader_file_ops = {
    .Open = NULL,
    .Read = pipe_read,
    .Write = NULL,
    .Close = pipe_reader_close
};

static file_ops writer_file_ops = {
    .Open = NULL,
    .Read = NULL,
    .Write = pipe_write,
    .Close = pipe_writer_close
};

void initialize_Pipe(FCB** fcb) 
{
	pipe_cb* pipe = (pipe_cb*)xmalloc(sizeof(pipe_cb)); 
	
	pipe->reader = fcb[0];
	pipe->writer = fcb[1];

	pipe->w_position = 0;
	pipe->r_position = 0;

	pipe->has_space = COND_INIT; 
	pipe->has_data = COND_INIT;
	
	fcb[0]->streamobj = pipe;
	fcb[0]->streamfunc = &reader_file_ops;

	fcb[1]->streamobj = pipe;
	fcb[1]->streamfunc = &writer_file_ops;

}

int available_space(pipe_cb* pipe, int func) 
{
	if(func) return (pipe->w_position + 1) % PIPE_BUFFER_SIZE - pipe->r_position;
	else return pipe->w_position - pipe->r_position;
}

