/* Copyright STIFTELSEN SINTEF 2012
 * 
 * This file is part of the Tinia Framework.
 * 
 * The Tinia Framework is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * The Tinia Framework is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 * 
 * You should have received a copy of the GNU Affero General Public License
 * along with the Tinia Framework.  If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <assert.h>
#include <pthread.h>
#include <string.h> // memset + strerror
#include <time.h>

#include <errno.h>

// shmem stuff
#include <sys/mman.h>
#include <sys/stat.h>        /* For mode constants */
#include <fcntl.h>           /* For O_* constants */

#include <tinia/ipc/ipc_msg.h>
#include "ipc_msg_internal.h"


tinia_ipc_msg_server_t*
ipc_msg_server_create(const char*       jobid,
                       tinia_ipc_msg_log_func_t logger_f,
                       void*            logger_d  )
{
    static const char* who = "tinia.ipc.msg.server.create";
    char errnobuf[256];
    int rc;
            
    // block SIGTERM (we will grab it in mainloop)
    sigset_t signal_mask;
    sigemptyset (&signal_mask);
    sigaddset (&signal_mask, SIGTERM);
    rc = pthread_sigmask (SIG_BLOCK, &signal_mask, NULL);
    if( rc != 0 ) {
        logger_f( logger_d, 0, who,
                  "pthread_sigmask failed: %s",
                  ipc_msg_strerror_wrap(rc, errnobuf, sizeof(errnobuf) ) );
        return NULL;
    }
    
    // -------------------------------------------------------------------------
    // --- first, do stuff that doesn't requere server structure ---------------
    // -------------------------------------------------------------------------

    // --- create name of shared memory ----------------------------------------
    char path[256];
    if( ipc_msg_shmem_path( logger_f,
                            logger_d,
                            path,
                            sizeof(path),
                            jobid ) != 0 )
    {
        return NULL;
    }

    // --- get page size, used to pad header size ------------------------------    
    long int page_size = sysconf( _SC_PAGESIZE );
    if( page_size == -1 ) {
        logger_f( logger_d, 0, who,
                  "sysconf( _SC_PAGESIZE ) failed: %s",
                  ipc_msg_strerror_wrap(errno, errnobuf, sizeof(errnobuf) ) );
        return NULL;
    }    

    // -------------------------------------------------------------------------
    // --- allocate structure and begin initialization -------------------------
    // -------------------------------------------------------------------------
    tinia_ipc_msg_server_t* server = (tinia_ipc_msg_server_t*)malloc( sizeof(tinia_ipc_msg_server_t) );
    server->logger_f = logger_f;
    server->logger_d = logger_d;
    server->shmem_name = strdup( path );
    server->shmem_base = MAP_FAILED;
    server->shmem_total_size = 0;
    server->shmem_header_ptr = (tinia_ipc_msg_header_t*)MAP_FAILED;
    server->shmem_header_size = ((sizeof(tinia_ipc_msg_header_t)+page_size-1)/page_size)*page_size; // multiple of page size
    server->shmem_payload_ptr = MAP_FAILED;
    server->shmem_payload_size = 0;
    server->deferred_notification_event = 0;
    
    size_t requested_payload_size = 8*1024*1024;   //TINIA_IPC_MSG_PART_MIN_BYTES;
    if( ipc_msg_fake_shmem != 0 ) {
        
        rc = pthread_mutex_lock( &ipc_msg_fake_shmem_lock );
        if( rc != 0 ) {
            server->logger_f( server->logger_d, 0, who,
                              "pthread_mutex_lock( ipc_msg_fake_shmem_lock ) failed: %s",
                              ipc_msg_strerror_wrap(rc, errnobuf, sizeof(errnobuf) ) );
            return NULL;
        }
        else {
            
            ipc_msg_fake_shmem_size = ((requested_payload_size+server->shmem_header_size+page_size-1)/page_size)*page_size;
            ipc_msg_fake_shmem_ptr = malloc( ipc_msg_fake_shmem_size );
            if( ipc_msg_fake_shmem_ptr == NULL ) {
                server->logger_f( server->logger_d, 0, who,
                                  "malloc( ipc_msg_fake_shmem_size ) failed." );
                pthread_mutex_unlock( &ipc_msg_fake_shmem_lock );
                return NULL;
            }
            server->shmem_total_size = ipc_msg_fake_shmem_size;
            server->shmem_payload_size = server->shmem_total_size - server->shmem_header_size;
            server->shmem_base = ipc_msg_fake_shmem_ptr;
            ipc_msg_fake_shmem_users++;

            rc = pthread_mutex_unlock( &ipc_msg_fake_shmem_lock );
            if( rc != 0 ) {
                server->logger_f( server->logger_d, 0, who,
                                  "pthread_mutex_unlock( ipc_msg_fake_shmem_lock ) failed: %s",
                                  ipc_msg_strerror_wrap(rc, errnobuf, sizeof(errnobuf) ) );
            }
        }        
    }
    else {
        // --- remove old shared memory segment if it hasn't been cleaned up -------
        if( shm_unlink( path ) == 0 ) {
            server->logger_f( server->logger_d, 2, who,
                              "Removed existing shared memory segment '%s'\n",
                              path );
        }
        
        // --- create and open -----------------------------------------------------
        // NOTE: mode 0600, only user can communicate. Change if necessary.
        int fd = shm_open( server->shmem_name, O_RDWR | O_CREAT | O_EXCL, 0600 );
        if( fd < 0 ) {
            server->logger_f( server->logger_d, 0, who,
                              "Failed to create shared memory: %s",
                              ipc_msg_strerror_wrap(errno, errnobuf, sizeof(errnobuf) ) );
            ipc_msg_server_delete( server );
            return NULL;
        }
        
        // --- set size of shared memory segment -----------------------------------
        if( ftruncate( fd, requested_payload_size+server->shmem_header_size ) != 0 ) {
            server->logger_f( server->logger_d, 0, who,
                              "Failed to set shared memory size to %ld: %s",
                              (requested_payload_size+server->shmem_header_size),
                              ipc_msg_strerror_wrap(errno, errnobuf, sizeof(errnobuf) ) );
            close( fd );
            ipc_msg_server_delete( server );
            return NULL;
        }
        
        // --- query actual size of shared memory ----------------------------------
        struct stat fstat_buf;
        if( fstat( fd, &fstat_buf ) != 0 ) {
            server->logger_f( server->logger_d, 0, who,
                              "Failed to get shared memory size: %s",
                              ipc_msg_strerror_wrap(errno, errnobuf, sizeof(errnobuf) ) );
            close( fd );
            ipc_msg_server_delete( server );
            return NULL;
        }
        server->shmem_total_size = fstat_buf.st_size;
        if( server->shmem_total_size < requested_payload_size+server->shmem_header_size ) {
            // shouldn't happen
            server->logger_f( server->logger_d, 0, who,
                              "shmem size is less than requested!" );
            close( fd );
            ipc_msg_server_delete( server );
            return NULL;
        }
        server->shmem_payload_size = server->shmem_total_size - server->shmem_header_size;
        
        // --- map memory into address space ---------------------------------------
        server->shmem_base = mmap( NULL,
                                   server->shmem_total_size,
                                   PROT_READ | PROT_WRITE,
                                   MAP_SHARED,
                                   fd, 0 );
        if( server->shmem_base == MAP_FAILED ) {
            server->logger_f( server->logger_d, 0, who,
                              "Failed to map shared memory: %s",
                              ipc_msg_strerror_wrap(errno, errnobuf, sizeof(errnobuf) ) );
            close( fd );
            ipc_msg_server_delete( server );
            return NULL;
        }
        close( fd );
    }
    
    server->shmem_header_ptr = (tinia_ipc_msg_header_t*)server->shmem_base;
    server->shmem_payload_ptr = (char*)server->shmem_base + server->shmem_header_size;
    
    server->shmem_header_ptr->header_size = server->shmem_header_size;
    server->shmem_header_ptr->payload_size = server->shmem_payload_size;
    
    //fprintf( stderr, "I: %s: header=%lu bytes, payload=%lu bytes, total=%lu bytes.\n",
    //         server->shmem_name,
    //         server->shmem_header_size, server->shmem_payload_size, server->shmem_total_size );

    // -------------------------------------------------------------------------
    // --- shared memory is set up and mapped, set up pthreads-stuff -----------
    // -------------------------------------------------------------------------
    
    server->shmem_header_ptr->initialized = 0;
    server->shmem_header_ptr->state = IPC_MSG_STATE_ERROR;

    pthread_mutexattr_t mutexattr;
    pthread_condattr_t condattr;

    server->thread_id = pthread_self();
    
    int failed = 0;

#define CHECK(A) \
do { \
    if(failed==0) { \
        if((rc=A)!=0) { \
            server->logger_f( server->logger_d, 0, who, "%s: %s",\
                              #A, ipc_msg_strerror_wrap(rc, errnobuf, sizeof(errnobuf) ) ); \
            failed=1; \
        } \
    } \
} while(0)
    
    // --- initialize transaction lock -----------------------------------------
    CHECK( pthread_mutexattr_init( &mutexattr ) );
    CHECK( pthread_mutexattr_setpshared( &mutexattr, PTHREAD_PROCESS_SHARED ) );
#ifdef DEBUG
    CHECK( pthread_mutexattr_settype( &mutexattr, PTHREAD_MUTEX_ERRORCHECK ) );
#endif
    CHECK( pthread_mutex_init( &server->shmem_header_ptr->transaction_lock, &mutexattr ) );
    CHECK( pthread_mutexattr_destroy( &mutexattr ) );

    // --- initialize operation lock -----------------------------------------------
    CHECK( pthread_mutexattr_init( &mutexattr ) );
    CHECK( pthread_mutexattr_setpshared( &mutexattr, PTHREAD_PROCESS_SHARED ) );
#ifdef DEBUG
    CHECK( pthread_mutexattr_settype( &mutexattr, PTHREAD_MUTEX_ERRORCHECK ) );
#endif
    CHECK( pthread_mutex_init( &server->shmem_header_ptr->operation_lock, &mutexattr ) );
    CHECK( pthread_mutexattr_destroy( &mutexattr ) );

    // --- initialize deferred_notification lock -------------------------------
    CHECK( pthread_mutexattr_init( &mutexattr ) );
#ifdef DEBUG
    CHECK( pthread_mutexattr_settype( &mutexattr, PTHREAD_MUTEX_ERRORCHECK ) );
#endif
    CHECK( pthread_mutex_init( &server->deferred_notification_lock, &mutexattr ) );
    CHECK( pthread_mutexattr_destroy( &mutexattr ) );

    // --- initialize notification condition variable --------------------------
    CHECK( pthread_condattr_init( &condattr ) );
    CHECK( pthread_condattr_setpshared( &condattr, PTHREAD_PROCESS_SHARED ) );
    CHECK( pthread_cond_init( &server->shmem_header_ptr->notification_event, &condattr ) );
    CHECK( pthread_condattr_destroy( &condattr ) );

    // --- initialize server wakeup condition variable -------------------------
    CHECK( pthread_condattr_init( &condattr ) );
    CHECK( pthread_condattr_setpshared( &condattr, PTHREAD_PROCESS_SHARED ) );
    CHECK( pthread_cond_init( &server->shmem_header_ptr->server_event, &condattr ) );
    CHECK( pthread_condattr_destroy( &condattr ) );

    // --- initialize client wakeup condition variable -------------------------
    CHECK( pthread_condattr_init( &condattr ) );
    CHECK( pthread_condattr_setpshared( &condattr, PTHREAD_PROCESS_SHARED ) );
    CHECK( pthread_cond_init( &server->shmem_header_ptr->client_event, &condattr ) );
    CHECK( pthread_condattr_destroy( &condattr ) );


#undef CHECK
    
    // --- if some of the pthread-init-stuff has failed, give up ---------------
    if( failed != 0 ) {
        ipc_msg_server_delete( server );
        return NULL;
    }

    server->shmem_header_ptr->initialized = 1;
    return server;
}

int
ipc_msg_server_delete( tinia_ipc_msg_server_t* server )
{
    static const char* who = "tinia.ipc.msg.server.delete";
    char errnobuf[256];

    int rc;
    if( server == NULL ) {
        return -1;
    }
    
    if( (server->shmem_header_ptr != (tinia_ipc_msg_header_t*)MAP_FAILED)
            && (server->shmem_header_ptr->initialized == 1 ) )
    {
#define CHECK(A) \
do { \
    rc=A; \
    if(rc!=0) { \
        server->logger_f( server->logger_d, 0, who, "%s: %s", #A, \
                          ipc_msg_strerror_wrap(rc, errnobuf, sizeof(errnobuf) ) ); \
    } \
} while(0)
        CHECK( pthread_cond_destroy( &server->shmem_header_ptr->client_event ) );
        CHECK( pthread_cond_destroy( &server->shmem_header_ptr->server_event ) );
        CHECK( pthread_cond_destroy( &server->shmem_header_ptr->notification_event ) );
        CHECK( pthread_mutex_destroy( &server->shmem_header_ptr->operation_lock ) );
        CHECK( pthread_mutex_destroy( &server->shmem_header_ptr->transaction_lock ) );
#undef CHECK
        server->shmem_header_ptr->initialized = 0;
    }

    if( ipc_msg_fake_shmem ) {
        rc = pthread_mutex_lock( &ipc_msg_fake_shmem_lock );
        if( rc != 0 ) {
            server->logger_f( server->logger_d, 0, who,
                              "pthread_mutex_lock( ipc_msg_fake_shmem_lock ) failed: %s",
                              ipc_msg_strerror_wrap(rc, errnobuf, sizeof(errnobuf) ) );
        }
        else {
            ipc_msg_fake_shmem_users--;
            if( server->shmem_base != NULL ) {
                free( server->shmem_base );
            }
            server->shmem_base = MAP_FAILED;
            ipc_msg_fake_shmem_ptr = NULL;
            ipc_msg_fake_shmem_size = 0;
            
            rc = pthread_mutex_unlock( &ipc_msg_fake_shmem_lock );
            if( rc != 0 ) {
                server->logger_f( server->logger_d, 0, who,
                                  "pthread_mutex_unlock( ipc_msg_fake_shmem_lock ) failed: %s",
                                  ipc_msg_strerror_wrap(rc, errnobuf, sizeof(errnobuf) ) );
            }
        }
    }
    else {
        if( server->shmem_base != MAP_FAILED ) {
            if( munmap( server->shmem_base, server->shmem_total_size ) != 0 ) {
                server->logger_f( server->logger_d, 0, who,
                                  "Failed to unmap shared memory failed: %s",
                                  ipc_msg_strerror_wrap(errno, errnobuf, sizeof(errnobuf) ) );
            }
            server->shmem_base = MAP_FAILED;
        }
        
        if( server->shmem_name != NULL ) {
            if( shm_unlink( server->shmem_name ) != 0 ) {
                server->logger_f( server->logger_d, 0, who,
                                  "Failed to unlink shared memory: %s",
                                  ipc_msg_strerror_wrap(errno, errnobuf, sizeof(errnobuf) ) );
            }
        }
    }
    if( server->shmem_name != NULL ) {
        free( server->shmem_name );
        server->shmem_name = NULL;
    }
        
    free( server );
    return 0;
}

int
ipc_msg_server_signal_client( char* errnobuf,
                              size_t errnobuf_size,
                              tinia_ipc_msg_server_t* server )
{
    static const char* who = "tinia.ipc.msg.server.signal.client";
    server->shmem_header_ptr->client_event_predicate = 1;
    int rc = pthread_cond_signal( &server->shmem_header_ptr->client_event );
    if( rc != 0 ) {
        server->logger_f( server->logger_d, 0, who,
                          "pthread_cond_signal( client_event ): %s",
                          ipc_msg_strerror_wrap(rc, errnobuf, errnobuf_size ) );
        server->shmem_header_ptr->state = IPC_MSG_STATE_ERROR;
        return -2;
    }
    return 0;
}

int
ipc_msg_server_send( char* errnobuf,
                     size_t errnobuf_size,
                     tinia_ipc_msg_server_t* server,
                     tinia_ipc_msg_output_handler_func_t output_handler,
                     void* output_handler_data )
{
    static const char* who = "tinia.ipc.msg.server.send";
    
    int ret = 0;

    tinia_ipc_msg_producer_func_t producer = NULL;
    void* producer_data = NULL;

    struct timespec timeout;
    clock_gettime( CLOCK_REALTIME, &timeout );
    timeout.tv_sec += 5;

    
    // --- send train of reply messages ----------------------------------------
    int part, more;
    for( part=0, more=1; (more==1) && (ret==0); part++ ) {
       
        // --- In first part, we determine the producer ------------------------
        if( part == 0 ) { // If first part, let output handler determine producer
            if( output_handler( &producer, &producer_data, output_handler_data ) != 0 )
            {
                server->logger_f( server->logger_d, 0, who,
                                  "Output handler failed." );
                ret = -1;
                server->shmem_header_ptr->state = IPC_MSG_STATE_ERROR;
                ipc_msg_server_signal_client( errnobuf, errnobuf_size, server );
                break;
            }
            if( producer == NULL ) {
                server->logger_f( server->logger_d, 0, who,
                                  "Output handler returned nullptr." );
                ret = -2;
                server->shmem_header_ptr->state = IPC_MSG_STATE_ERROR;
                ipc_msg_server_signal_client( errnobuf, errnobuf_size, server );
                break;
            }
        }

        // --- check that state is as expected (and not error) -----------------
        if( server->shmem_header_ptr->state != IPC_MSG_STATE_SERVER_TO_CLIENT ) {
            server->logger_f( server->logger_d, 0, who,
                              "Got state %d, expected state %d.",
                              server->shmem_header_ptr->state,
                              IPC_MSG_STATE_SERVER_TO_CLIENT );
            ret = -1;
            server->shmem_header_ptr->state = IPC_MSG_STATE_ERROR;
            ipc_msg_server_signal_client( errnobuf, errnobuf_size, server );
            break;
        }

        // --- produce message part --------------------------------------------
        int more = 0;
        size_t bytes = 0;
        if( producer( producer_data,
                      &more,
                      (char*)server->shmem_payload_ptr,
                      &bytes,
                      server->shmem_payload_size,
                      part ) != 0 )
        {
            server->logger_f( server->logger_d, 0, who, "Producer failed." );
            ret = -1;
            server->shmem_header_ptr->state = IPC_MSG_STATE_ERROR;
            ipc_msg_server_signal_client( errnobuf, errnobuf_size, server );
            break;
        }
        
        // --- signal the client that it might process this part ---------------
        server->shmem_header_ptr->part  = part;
        server->shmem_header_ptr->more  = more;
        server->shmem_header_ptr->bytes = bytes;
        if( (ret = ipc_msg_server_signal_client( errnobuf, errnobuf_size, server ) ) != 0 ) {
            break;
        }
        
        // --- if this was the last part, break out ----------------------------
        if( more == 0 ) {
            break;
        }

        // --- wait for buffer to be ready for the next part -------------------        
        if( (ret = ipc_msg_server_wait_client( errnobuf, errnobuf_size, server ) ) != 0 ) {
            break;
        }
    }
    return ret;
}

int
ipc_msg_server_notify( tinia_ipc_msg_server_t* server )
{
    static const char* who = "tinia.ipc.msg.server.mainloop.notify";
    char errnobuf[256];
    int rc, ret = 0;

    if( pthread_equal( pthread_self(), server->thread_id ) != 0  ) {
#ifdef TINIA_IPC_LOG_TRACE
        server->logger_f( server->logger_d, 2, who, "Invoked from mainloop thread, has lock and signaling." );
#endif
        // --- we're the mainloop thread -------------------------------------------

        // --- signal notification condition (linked to transaction lock) ------
        rc = pthread_cond_broadcast( &server->shmem_header_ptr->notification_event );
        if( rc != 0 ) {
            server->logger_f( server->logger_d, 0, who,
                              "pthread_cond_broadcast( notification_event ) failed: %s",
                              ipc_msg_strerror_wrap(rc, errnobuf, sizeof(errnobuf) ) );
            ret = -2;
        }

    }
    else {
        // --- try to lock transaction lock ------------------------------------
        rc = pthread_mutex_trylock( &server->shmem_header_ptr->transaction_lock );
        if( rc == EBUSY ) {
#ifdef TINIA_IPC_LOG_TRACE
            server->logger_f( server->logger_d, 2, who, "Invoked from non-mainloop thread, lock busy, deferring" );
#endif
            // someone is interacting with the server, defer signaling until
            // main thread can handle it.
            if( ipc_msg_set_deferred_notification_event( server ) != 0 ) {
                ret = -2;
            }
        }
        else if (rc != 0 ) {
            server->logger_f( server->logger_d, 0, who,
                              "pthread_mutex_timedlock( transaction_lock ) failed: %s",
                              ipc_msg_strerror_wrap(rc, errnobuf, sizeof(errnobuf) ) );
            ret = -2;  
        }
        else {
#ifdef TINIA_IPC_LOG_TRACE
            server->logger_f( server->logger_d, 2, who,
                              "Invoked from non-mainloop thread, got lock, signaling" );
#endif

            // --- signal notification condition (linked to transaction lock) --
            rc = pthread_cond_broadcast( &server->shmem_header_ptr->notification_event );
            if( rc != 0 ) {
                server->logger_f( server->logger_d, 0, who,
                                  "pthread_cond_broadcast( notification_event ) failed: %s",
                                  ipc_msg_strerror_wrap(rc, errnobuf, sizeof(errnobuf) ) );
                ret = -2;
            }
            
            // --- unlock transaction lock -------------------------------------
            rc = pthread_mutex_unlock( &server->shmem_header_ptr->transaction_lock );
            if( rc != 0 ) {
                server->logger_f( server->logger_d, 0, who,
                                  "pthread_mutex_unlock( transaction_lock ) failed: %s",
                                  ipc_msg_strerror_wrap(rc, errnobuf, sizeof(errnobuf) ) );
                ret = -2;
            }
        }
    }
    return ret;
}
