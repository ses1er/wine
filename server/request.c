/*
 * Server-side request handling
 *
 * Copyright (C) 1998 Alexandre Julliard
 */

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "winerror.h"
#include "winnt.h"
#include "winbase.h"
#define WANT_REQUEST_HANDLERS
#include "server.h"
#include "server/request.h"
#include "server/thread.h"

/* check that the string is NULL-terminated and that the len is correct */
#define CHECK_STRING(func,str,len) \
  do { if (((str)[(len)-1] || strlen(str) != (len)-1)) \
         fatal_protocol_error( "%s: invalid string '.*s'\n", (func), (len), (str) ); \
     } while(0)
 
struct thread *current = NULL;  /* thread handling the current request */

/* complain about a protocol error and terminate the client connection */
static void fatal_protocol_error( const char *err, ... )
{
    va_list args;

    va_start( args, err );
    fprintf( stderr, "Protocol error:%p: ", current );
    vfprintf( stderr, err, args );
    va_end( args );
    remove_client( current->client_fd, -2 );
}

/* call a request handler */
void call_req_handler( struct thread *thread, enum request req,
                       void *data, int len, int fd )
{
    const struct handler *handler = &req_handlers[req];
    char *ptr;

    current = thread;
    if ((req < 0) || (req >= REQ_NB_REQUESTS))
    {
        fatal_protocol_error( "unknown request %d\n", req );
        return;
    }

    if (len < handler->min_size)
    {
        fatal_protocol_error( "req %d bad length %d < %d)\n", req, len, handler->min_size );
        return;
    }

    /* now call the handler */
    if (current)
    {
        CLEAR_ERROR();
        if (debug_level) trace_request( req, data, len, fd );
    }
    len -= handler->min_size;
    ptr = (char *)data + handler->min_size;
    handler->handler( data, ptr, len, fd );
    current = NULL;
}

/* handle a client timeout (unused for now) */
void call_timeout_handler( struct thread *thread )
{
    current = thread;
    if (debug_level) trace_timeout();
    CLEAR_ERROR();
    thread_timeout();
    current = NULL;
}

/* a thread has been killed */
void call_kill_handler( struct thread *thread, int exit_code )
{
    /* must be reentrant WRT call_req_handler */
    struct thread *old_current = current;
    current = thread;
    if (current)
    {
        if (debug_level) trace_kill( exit_code );
        thread_killed( current, exit_code );
    }
    current = (old_current != thread) ? old_current : NULL;
}


/* create a new thread */
DECL_HANDLER(new_thread)
{
    struct new_thread_reply reply;
    struct thread *new_thread;
    int new_fd, err;

    if ((new_fd = dup(fd)) == -1)
    {
        new_thread = NULL;
        err = ERROR_TOO_MANY_OPEN_FILES;
        goto done;
    }
    if (!(new_thread = create_thread( new_fd, req->pid, &reply.thandle,
                                      &reply.phandle )))
    {
        close( new_fd );
        err = ERROR_OUTOFMEMORY;
        goto done;
    }
    reply.tid = new_thread;
    reply.pid = new_thread->process;
    err = ERROR_SUCCESS;

 done:
    if (!current)
    {
        /* first client doesn't have a current */
        struct iovec vec = { &reply, sizeof(reply) };
        send_reply_v( get_initial_client_fd(), err, -1, &vec, 1 );
    }
    else
    {
        SET_ERROR( err );
        send_reply( current, -1, 1, &reply, sizeof(reply) );
    }
}

/* create a new thread */
DECL_HANDLER(init_thread)
{
    if (current->state != STARTING)
    {
        fatal_protocol_error( "init_thread: already running\n" );
        return;
    }
    current->state    = RUNNING;
    current->unix_pid = req->unix_pid;
    if (!(current->name = mem_alloc( len + 1 ))) goto done;
    memcpy( current->name, data, len );
    current->name[len] = '\0';
    CLEAR_ERROR();
 done:
    send_reply( current, -1, 0 );
}

/* set the debug level */
DECL_HANDLER(set_debug)
{
    debug_level = req->level;
    /* Make sure last_req is initialized */
    current->last_req = REQ_SET_DEBUG;
    CLEAR_ERROR();
    send_reply( current, -1, 0 );
}

/* terminate a process */
DECL_HANDLER(terminate_process)
{
    struct process *process;

    if ((process = get_process_from_handle( req->handle, PROCESS_TERMINATE )))
    {
        kill_process( process, req->exit_code );
        release_object( process );
    }
    if (current) send_reply( current, -1, 0 );
}

/* terminate a thread */
DECL_HANDLER(terminate_thread)
{
    struct thread *thread;

    if ((thread = get_thread_from_handle( req->handle, THREAD_TERMINATE )))
    {
        kill_thread( thread, req->exit_code );
        release_object( thread );
    }
    if (current) send_reply( current, -1, 0 );
}

/* close a handle */
DECL_HANDLER(close_handle)
{
    close_handle( current->process, req->handle );
    send_reply( current, -1, 0 );
}

/* duplicate a handle */
DECL_HANDLER(dup_handle)
{
    struct dup_handle_reply reply = { -1 };
    struct process *src, *dst;

    if ((src = get_process_from_handle( req->src_process, PROCESS_DUP_HANDLE )))
    {
        if (req->options & DUP_HANDLE_MAKE_GLOBAL)
        {
            reply.handle = duplicate_handle( src, req->src_handle, NULL, -1,
                                             req->access, req->inherit, req->options );
        }
        else if ((dst = get_process_from_handle( req->dst_process, PROCESS_DUP_HANDLE )))
        {
            reply.handle = duplicate_handle( src, req->src_handle, dst, req->dst_handle,
                                             req->access, req->inherit, req->options );
            release_object( dst );
        }
        /* close the handle no matter what happened */
        if (req->options & DUP_HANDLE_CLOSE_SOURCE)
            close_handle( src, req->src_handle );
        release_object( src );
    }
    send_reply( current, -1, 1, &reply, sizeof(reply) );
}

/* fetch information about a process */
DECL_HANDLER(get_process_info)
{
    struct process *process;
    struct get_process_info_reply reply = { 0, 0 };

    if ((process = get_process_from_handle( req->handle, PROCESS_QUERY_INFORMATION )))
    {
        get_process_info( process, &reply );
        release_object( process );
    }
    send_reply( current, -1, 1, &reply, sizeof(reply) );
}

/* fetch information about a thread */
DECL_HANDLER(get_thread_info)
{
    struct thread *thread;
    struct get_thread_info_reply reply = { 0, 0 };

    if ((thread = get_thread_from_handle( req->handle, THREAD_QUERY_INFORMATION )))
    {
        get_thread_info( thread, &reply );
        release_object( thread );
    }
    send_reply( current, -1, 1, &reply, sizeof(reply) );
}

/* open a handle to a process */
DECL_HANDLER(open_process)
{
    struct open_process_reply reply = { -1 };
    struct process *process = get_process_from_id( req->pid );
    if (process)
    {
        reply.handle = alloc_handle( current->process, process,
                                     req->access, req->inherit );
        release_object( process );
    }
    send_reply( current, -1, 1, &reply, sizeof(reply) );
}

/* select on a handle list */
DECL_HANDLER(select)
{
    if (len != req->count * sizeof(int))
        fatal_protocol_error( "select: bad length %d for %d handles\n",
                              len, req->count );
    sleep_on( current, req->count, (int *)data, req->flags, req->timeout );
}

/* create an event */
DECL_HANDLER(create_event)
{
    struct create_event_reply reply = { -1 };
    struct object *obj;
    char *name = (char *)data;
    if (!len) name = NULL;
    else CHECK_STRING( "create_event", name, len );

    obj = create_event( name, req->manual_reset, req->initial_state );
    if (obj)
    {
        reply.handle = alloc_handle( current->process, obj, EVENT_ALL_ACCESS, req->inherit );
        release_object( obj );
    }
    send_reply( current, -1, 1, &reply, sizeof(reply) );
}

/* do an event operation */
DECL_HANDLER(event_op)
{
    switch(req->op)
    {
    case PULSE_EVENT:
        pulse_event( req->handle );
        break;
    case SET_EVENT:
        set_event( req->handle );
        break;
    case RESET_EVENT:
        reset_event( req->handle );
        break;
    default:
        fatal_protocol_error( "event_op: invalid operation %d\n", req->op );
    }
    send_reply( current, -1, 0 );
}

/* create a mutex */
DECL_HANDLER(create_mutex)
{
    struct create_mutex_reply reply = { -1 };
    struct object *obj;
    char *name = (char *)data;
    if (!len) name = NULL;
    else CHECK_STRING( "create_mutex", name, len );

    obj = create_mutex( name, req->owned );
    if (obj)
    {
        reply.handle = alloc_handle( current->process, obj, MUTEX_ALL_ACCESS, req->inherit );
        release_object( obj );
    }
    send_reply( current, -1, 1, &reply, sizeof(reply) );
}

/* release a mutex */
DECL_HANDLER(release_mutex)
{
    if (release_mutex( req->handle )) CLEAR_ERROR();
    send_reply( current, -1, 0 );
}

/* create a semaphore */
DECL_HANDLER(create_semaphore)
{
    struct create_semaphore_reply reply = { -1 };
    struct object *obj;
    char *name = (char *)data;
    if (!len) name = NULL;
    else CHECK_STRING( "create_semaphore", name, len );

    obj = create_semaphore( name, req->initial, req->max );
    if (obj)
    {
        reply.handle = alloc_handle( current->process, obj, SEMAPHORE_ALL_ACCESS, req->inherit );
        release_object( obj );
    }
    send_reply( current, -1, 1, &reply, sizeof(reply) );
}

/* release a semaphore */
DECL_HANDLER(release_semaphore)
{
    struct release_semaphore_reply reply;
    if (release_semaphore( req->handle, req->count, &reply.prev_count )) CLEAR_ERROR();
    send_reply( current, -1, 1, &reply, sizeof(reply) );
}

/* open a handle to a named object (event, mutex, semaphore) */
DECL_HANDLER(open_named_obj)
{
    struct open_named_obj_reply reply;
    char *name = (char *)data;
    if (!len) name = NULL;
    else CHECK_STRING( "open_named_obj", name, len );

    switch(req->type)
    {
    case OPEN_EVENT:
        reply.handle = open_event( req->access, req->inherit, name );
        break;
    case OPEN_MUTEX:
        reply.handle = open_mutex( req->access, req->inherit, name );
        break;
    case OPEN_SEMAPHORE:
        reply.handle = open_semaphore( req->access, req->inherit, name );
        break;
    default:
        fatal_protocol_error( "open_named_obj: invalid type %d\n", req->type );
    }
    send_reply( current, -1, 1, &reply, sizeof(reply) );
}

/* create a file */
DECL_HANDLER(create_file)
{
    struct create_file_reply reply = { -1 };
    struct object *obj;
    int new_fd;

    if ((new_fd = dup(fd)) == -1)
    {
        SET_ERROR( ERROR_TOO_MANY_OPEN_FILES );
        goto done;
    }
    if ((obj = create_file( new_fd )) != NULL)
    {
        reply.handle = alloc_handle( current->process, obj, req->access, req->inherit );
        release_object( obj );
    }
 done:
    send_reply( current, -1, 1, &reply, sizeof(reply) );
}

/* get a Unix handle to a file */
DECL_HANDLER(get_unix_handle)
{
    int handle = file_get_unix_handle( req->handle, req->access );
    send_reply( current, handle, 0 );
}

/* get a Unix fd to read from a file */
DECL_HANDLER(get_read_fd)
{
    struct object *obj;
    int read_fd;

    if ((obj = get_handle_obj( current->process, req->handle, GENERIC_READ, NULL )))
    {
        read_fd = obj->ops->get_read_fd( obj );
        release_object( obj );
    }
    else read_fd = -1;
    send_reply( current, read_fd, 0 );
}

/* get a Unix fd to write to a file */
DECL_HANDLER(get_write_fd)
{
    struct object *obj;
    int write_fd;

    if ((obj = get_handle_obj( current->process, req->handle, GENERIC_WRITE, NULL )))
    {
        write_fd = obj->ops->get_write_fd( obj );
        release_object( obj );
    }
    else write_fd = -1;
    send_reply( current, write_fd, 0 );
}

/* set a file current position */
DECL_HANDLER(set_file_pointer)
{
    struct set_file_pointer_reply reply = { req->low, req->high };
    set_file_pointer( req->handle, &reply.low, &reply.high, req->whence );
    send_reply( current, -1, 1, &reply, sizeof(reply) );
}

/* truncate (or extend) a file */
DECL_HANDLER(truncate_file)
{
    truncate_file( req->handle );
    send_reply( current, -1, 0 );
}

/* flush a file buffers */
DECL_HANDLER(flush_file)
{
    struct object *obj;

    if ((obj = get_handle_obj( current->process, req->handle, GENERIC_WRITE, NULL )))
    {
        obj->ops->flush( obj );
        release_object( obj );
    }
    send_reply( current, -1, 0 );
}

/* get a file information */
DECL_HANDLER(get_file_info)
{
    struct get_file_info_reply reply;
    get_file_info( req->handle, &reply );
    send_reply( current, -1, 1, &reply, sizeof(reply) );
}

/* create an anonymous pipe */
DECL_HANDLER(create_pipe)
{
    struct create_pipe_reply reply = { -1, -1 };
    struct object *obj[2];
    if (create_pipe( obj ))
    {
        reply.handle_read = alloc_handle( current->process, obj[0],
                                          STANDARD_RIGHTS_REQUIRED|SYNCHRONIZE|GENERIC_READ,
                                          req->inherit );
        if (reply.handle_read != -1)
        {
            reply.handle_write = alloc_handle( current->process, obj[1],
                                               STANDARD_RIGHTS_REQUIRED|SYNCHRONIZE|GENERIC_WRITE,
                                               req->inherit );
            if (reply.handle_write == -1)
                close_handle( current->process, reply.handle_read );
        }
        release_object( obj[0] );
        release_object( obj[1] );
    }
    send_reply( current, -1, 1, &reply, sizeof(reply) );
}

/* create a console */
DECL_HANDLER(create_console)
{
    struct create_console_reply reply = { -1, -1 };
    struct object *obj[2];
    if (create_console( fd, obj ))
    {
        reply.handle_read = alloc_handle( current->process, obj[0],
                                          STANDARD_RIGHTS_REQUIRED|SYNCHRONIZE|GENERIC_READ,
                                          req->inherit );
        if (reply.handle_read != -1)
        {
            reply.handle_write = alloc_handle( current->process, obj[1],
                                               STANDARD_RIGHTS_REQUIRED|SYNCHRONIZE|GENERIC_WRITE,
                                               req->inherit );
            if (reply.handle_write == -1)
                close_handle( current->process, reply.handle_read );
        }
        release_object( obj[0] );
        release_object( obj[1] );
    }
    send_reply( current, -1, 1, &reply, sizeof(reply) );
}

/* set a console fd */
DECL_HANDLER(set_console_fd)
{
    set_console_fd( req->handle, fd );
    send_reply( current, -1, 0 );
}

/* create a change notification */
DECL_HANDLER(create_change_notification)
{
    struct object *obj;
    struct create_change_notification_reply reply = { -1 };

    if ((obj = create_change_notification( req->subtree, req->filter )))
    {
        reply.handle = alloc_handle( current->process, obj,
                                     STANDARD_RIGHTS_REQUIRED|SYNCHRONIZE, 0 );
        release_object( obj );
    }
    send_reply( current, -1, 1, &reply, sizeof(reply) );
}

