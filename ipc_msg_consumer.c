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
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <string.h> // memset + strerror
#include <pthread.h>

#include <unistd.h>
// shmem stuff
#include <sys/mman.h>
#include <sys/stat.h>        /* For mode constants */
#include <fcntl.h>           /* For O_* constants */

#include "ipc_msg.h"
#include "ipc_msg_internal.h"


const size_t tinia_ipc_msg_client_t_sizeof = sizeof(share_buf_ipc_msg_consumer_t);



int
tinia_ipc_msg_client_init(share_buf_ipc_msg_consumer_t*   client,
                           const char*               jobid,
                           tinia_ipc_msg_log_func_t  log_f,
                           void*                     log_d  )
{
    static const char* who = "tinia.ipc.msg.client.init";
    char errnobuf[256];
    int rc, ret=0;
    
    client->shmem_name[0] = '\0';
    client->logger_f = log_f;
    client->logger_d = log_d;
    client->shmem_base = MAP_FAILED;
    client->shmem_total_size = 0;
    client->shmem_header_ptr = (tinia_ipc_msg_header_t*)MAP_FAILED;
    client->shmem_header_size = 0;
    client->shmem_payload_ptr = MAP_FAILED;
    client->shmem_payload_size = 0;
    
    if( ipc_msg_shmem_path( client->logger_f,
                            client->logger_d,
                            client->shmem_name,
                            sizeof(client->shmem_name),
                            jobid ) != 0 )
    {
        return -1;
    }

    if( ipc_msg_fake_shmem != 0 ) {
        rc = pthread_mutex_lock( &ipc_msg_fake_shmem_lock );
        if( rc != 0 ) {
            client->logger_f( client->logger_d, 0, who,
                              "pthread_mutex_lock( ipc_msg_fake_shmem_lock ) failed: %s",
                              ipc_msg_strerror_wrap(rc, errnobuf, sizeof(errnobuf) ) );
            ret = -1;
        }
        else {
            if( ipc_msg_fake_shmem_ptr == NULL ) {
                client->logger_f( client->logger_d, 0, who,
                                  "Failed to open fake shared memory '%s'.",
                                  client->shmem_name );
                tinia_ipc_msg_client_release( client );
                ret = -1;;
            }
            else {
                client->shmem_total_size = ipc_msg_fake_shmem_size;
                client->shmem_base = ipc_msg_fake_shmem_ptr;
                ipc_msg_fake_shmem_users++;
            }
            rc = pthread_mutex_unlock( &ipc_msg_fake_shmem_lock );
            if( rc != 0 ) {
                client->logger_f( client->logger_d, 0, who,
                                  "pthread_mutex_unlock( ipc_msg_fake_shmem_lock ) failed: %s",
                                  ipc_msg_strerror_wrap(rc, errnobuf, sizeof(errnobuf) ) );
                ret = -1;
            }
        }
            
    }
    else {
        // --- open shared memory segment ------------------------------------------
        int fd = shm_open( client->shmem_name, O_RDWR, 0600 );
        if( fd < 0 ) {
            client->logger_f( client->logger_d, 0, who,
                              "Failed to open shared memory '%s': %s",
                              client->shmem_name,
                              ipc_msg_strerror_wrap(errno, errnobuf, sizeof(errnobuf) ) );
            ret = -1;
        }
        else {
            
            // --- determine size of shared memory segment -----------------------------
            struct stat fstat_buf;
            if( fstat( fd, &fstat_buf ) != 0 ) {
                client->logger_f( client->logger_d, 0, who,
                                  "Failed to determine size of shared memory '%s': %s",
                                  client->shmem_name,
                                  ipc_msg_strerror_wrap(errno, errnobuf, sizeof(errnobuf) ) );
                ret = -1;
            }
            else {
                client->shmem_total_size = fstat_buf.st_size;
                
                // --- map shared memory segment into process' address space ---------------
                client->shmem_base = mmap( NULL,
                                           fstat_buf.st_size,
                                           PROT_READ | PROT_WRITE,
                                           MAP_SHARED,
                                           fd,
                                           0 );
                if( client->shmem_base == MAP_FAILED ) {
                    client->logger_f( client->logger_d, 0, who,
                                      "Failed to map %ld bytes of shared memory '%s': %s",
                                      fstat_buf.st_size,
                                      client->shmem_name,
                                      ipc_msg_strerror_wrap(errno, errnobuf, sizeof(errnobuf) ) );
                    ret = -1;
                }
            }
            close( fd );
        }
        
    }
    if( ret == 0 ) {
        
        // --- update pointers and do some sanity checks ---------------------------    
        client->shmem_header_ptr = (tinia_ipc_msg_header_t*)client->shmem_base;
        client->shmem_header_size = client->shmem_header_ptr->header_size;
        client->shmem_payload_ptr = (char*)client->shmem_base + client->shmem_header_size;
        client->shmem_payload_size = client->shmem_header_ptr->payload_size;
        
        if( client->shmem_total_size != (client->shmem_header_size + client->shmem_payload_size) ) {
            client->logger_f( client->logger_d, 0, who,
                              "E: %lu != %lu + %lu!\n",
                              client->shmem_total_size,
                              client->shmem_header_size,
                              client->shmem_payload_size );
            ret = -1;
        }
    }
    
    if( ret != 0 ) {
        tinia_ipc_msg_client_release( client );
    }
    return ret;
}

int
tinia_ipc_msg_client_release(share_buf_ipc_msg_consumer_t* client )
{
    static const char* who = "tinia.ipc.msg.client.release";
    char errnobuf[256];
    int ret=0;

    if( ipc_msg_fake_shmem != 0 ) {
        if ( pthread_mutex_lock( &ipc_msg_fake_shmem_lock ) != 0) {
            return -1;
        }

        ipc_msg_fake_shmem_users--;

        if ( pthread_mutex_unlock( &ipc_msg_fake_shmem_lock ) != 0) {
            return -1;
        }
    }
    else {
        if( client->shmem_base != MAP_FAILED ) {
            if( munmap( client->shmem_base, client->shmem_total_size ) != 0 ) {
                client->logger_f( client->logger_d, 0, who,
                                  "Failed to unmap shared memory '%s': %s",
                                  client->shmem_name,
                                  ipc_msg_strerror_wrap(errno, errnobuf, sizeof(errnobuf) ) );
                ret = -1;
            }
        }
    }
    // Note that MAP_FAILED is used to flag shared memory as "unmapped" or "unused", 
    // not necessarily that mapping has really failed.
    client->shmem_name[0] = '\0';
    client->shmem_base = MAP_FAILED;
    client->shmem_total_size = 0;
    client->shmem_header_ptr = (tinia_ipc_msg_header_t*)MAP_FAILED;
    client->shmem_header_size = 0;
    client->shmem_payload_ptr = MAP_FAILED;
    client->shmem_payload_size = 0;
    return ret;    
}

int
ipc_msg_client_wait_server( char* errnobuf,
                            size_t errnobuf_size,
                            struct timespec* timeout,
                            share_buf_ipc_msg_consumer_t* client )
{
    static const char* who = "tinia.ipc.msg.client.wait.server";
    
    client->shmem_header_ptr->client_event_predicate = 0;
    do {
        int rc = pthread_cond_timedwait( &client->shmem_header_ptr->client_event,
                                         &client->shmem_header_ptr->operation_lock,
                                         timeout );
        if( rc != 0 ) {
            client->logger_f( client->logger_d, 0, who,
                              "pthread_cond_timedwait( client_event ): %s",
                              ipc_msg_strerror_wrap(rc, errnobuf, errnobuf_size ) );
            tinia_ipc_msg_dump_backtrace( client->logger_f, client->logger_d );
            
            client->shmem_header_ptr->state = IPC_MSG_STATE_ERROR;
            ipc_msg_client_signal_server( errnobuf, errnobuf_size, client );
            if( rc == ETIMEDOUT ) {
                return -1;
            }
            else {
                return -2;
            }
        }
    } while( client->shmem_header_ptr->client_event_predicate == 0 );
    return 0;
}

int
ipc_msg_client_recv( char*                          errnobuf,
                     size_t                         errnobuf_size,
                     struct timespec*               timeout,
                     share_buf_ipc_msg_consumer_t*        client,
                     tinia_ipc_msg_consumer_func_t  consumer,
                     void*                          consumer_data )
{
    static const char* who = "tinia.ipc.msg.client.recv";
    
    int do_wait_on_notification = 0;
    int ret = 0, part, rc;
 
    enum tinia_ipc_msg_state_t debug = client->shmem_header_ptr->state;

    // --- wait for server to be ready -------------------------------------
    if ((ret = ipc_msg_client_wait_server(errnobuf, errnobuf_size, timeout, client)) != 0) {
        client->logger_f(client->logger_d, 0, who, "server wait fail, part=%d, state=%d",
            part, debug);

        return -1;
    }

    consumer(consumer_data,
        (char*)client->shmem_payload_ptr,
        client->shmem_header_ptr->bytes,
        part);

#ifdef TRACE
    client->logger_f( client->logger_d, 2, who, "Received %d parts from %s.", part+1, client->shmem_name );
#endif

    return 0;
}

