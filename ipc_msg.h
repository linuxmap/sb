#pragma once

// === CALLBACK TYPES ==========================================================
#include "ipc_util.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Minimum size for the buffer that holds message parts.
 *
 * It can be safely assumed that the buffer that is used to transmit message
 * parts between the client and server is of this size, and that messages
 * smaller than this size doesn't need to be split.
 *
 * \note This compile-time constant _might_ change, and thus, assumming it is of
 * at least a given fixed size warrants an assertion.
 */
#define SHARED_BUF_IPC_MSG_PART_MIN_BYTES 4096


typedef struct tinia_ipc_msg_client_struct share_buf_ipc_msg_consumer_t;

extern const size_t tinia_ipc_msg_client_t_sizeof;

/** Initializes a new message client.
 *
 * \param[inout] client       User-supplied buffer that contains the client
 *                            struct, the bytesize of this struct is
 *                            tinia_ipc_msg_client_t_sizeof.
 * \param[in]    destination  Job id of destination server.
 * \param[in]    logger_f     Callback that handles log messages.
 * \param[in]    logger_d     Optional data passed to logger callback.
 *
 * \return 0 on success, a negative value on error.
 */
int
distribute_ipc_msg_consumer_init(share_buf_ipc_msg_consumer_t*   client,
                           const char*               jobid,
                           tinia_ipc_msg_log_func_t  log_f,
                           void*                     log_d );


/** Releases resources of a message client.
 *
 * \param[in] client  Buffer that contains the client struct.
 *
 * \return 0 on success, a negative value on error.
 */
int
distribute_ipc_msg_consumer_release(share_buf_ipc_msg_consumer_t* client );

int
distribute_ipc_msg_consumer_wait(share_buf_ipc_msg_consumer_t* client,int longpoll_timeout);


typedef struct tinia_ipc_msg_server_struct distribute_ipc_msg_producer_t;


/** Create a new server.
 *
 * \param jobid     Id of the server.
 * \param logger_f  Callback used for logging.
 * \param logger_d  Optional data passed to logger callback.
 *
 * \warning This function must be invoked before any additional threads are
 * created, otherwise the signalmask cannot be set properly. 
 *
 * \warning Only one server should be created per process.
 */
distribute_ipc_msg_producer_t*
distribute_ipc_msg_producer_create( const char*       jobid,
                       tinia_ipc_msg_log_func_t  logger_f,
                       void*             logger_d );

/** Clean up and release resources of an existing server.
 *
 * \param[in] server  Pointer to an existing server.
 *
 * \return 0 on success, or a negative value on failure.
 */
int
distribute_ipc_msg_producer_delete(distribute_ipc_msg_producer_t* producer);


int
distribute_msg_producer_send(char* errnobuf,
    size_t errnobuf_size,
    distribute_ipc_msg_producer_t* producer,
    tinia_ipc_msg_output_handler_func_t output_handler,
    void* output_handler_data);

/** Wake all clients waiting for notification.
 *
 * Wakes all the clients that are currently waiting for notification.
 *
 * If invoked in the same thread as the thread running the mainloop, waiting
 * clients are notified immediatly. Otherwise, it first waits for any currently
 * active clients to finish. This behaviour is to avoid that a client misses a
 * notification in the moment between a query was processed and a client starts
 * to wait for notifications.
 *
 * \param[in] producer               Pointer to initialized producer struct.
 *
 * \return 0 on success, or a negative value on failure.
 */
int
distribute_ipc_msg_producer_notify(distribute_ipc_msg_producer_t* producer);

#ifdef __cplusplus
}
#endif
