#pragma once

typedef struct {
    /** True when all pthread-primitives has been initialized. */
    unsigned int        initialized;

    /** Number of bytes in the header-part of the shared memory. */
    size_t              header_size;

    /** Number of bytes in the payload-part of the shared memory. */
    size_t              payload_size;

    /** Lock for a sequence of operations.
    *
    * Makes sure that only one client may participate in a transaction. The
    * server may never take this lock, only a client. And a client may only
    * take this lock if it has not taken the operation lock.
    */
    pthread_mutex_t     transaction_lock;

    /** Lock for processing an operation.
    *
    * Gets sent back and forth between client and server during a transaction.
    * A client shall never take this lock without having succesfully taken
    * the transaction lock.
    */
    pthread_mutex_t     operation_lock;

    /** An operation is ready for the server to process.
    *
    * Condition is associated with operation_lock.
    */
    pthread_cond_t      server_event;
    int                 server_event_predicate;

    /** An operation is ready for the client to process.
    *
    * Condition is associated with operation_lock.
    */
    pthread_cond_t      client_event;
    int                 client_event_predicate;

    /** Allows clients to long-poll.
    *
    * Condition is associated with transaction_lock.
    */
    pthread_cond_t      notification_event;


    /** True when a server is listening. */
    int                 mainloop_running;

    /** Which part no of the current message is transmitted.
    *
    * Written by producer, verified by consumer.
    */
    int        part;

    /** More parts in current message?
    *
    * Used to notify the receiver that there will be another message part, used
    * by both client and server.
    */
    unsigned int        more;


    /** The number of bytes in the current message part. */
    size_t              bytes;

    enum tinia_ipc_msg_state_t    state;

} tinia_ipc_msg_header_t;

struct tinia_ipc_msg_client_struct {
    char                shmem_name[256];
    tinia_ipc_msg_log_func_t    logger_f;
    void*               logger_d;
    void*               shmem_base;
    size_t              shmem_total_size;
    tinia_ipc_msg_header_t*   shmem_header_ptr;
    size_t              shmem_header_size;
    void*               shmem_payload_ptr;
    size_t              shmem_payload_size;
};

struct tinia_ipc_msg_server_struct {
    pthread_t           thread_id;          ///< Thread id of the thread that initialized and runs the server.
    tinia_ipc_msg_log_func_t    logger_f;
    void*               logger_d;
    char*               shmem_name;
    void*               shmem_base;
    size_t              shmem_total_size;
    tinia_ipc_msg_header_t*   shmem_header_ptr;
    size_t              shmem_header_size;
    void*               shmem_payload_ptr;
    size_t              shmem_payload_size;

    /** True if a thread has failed to notify clients.
    *
    * To avoid leaky notifications (situations where clients may miss updates
    * and wait until timeout unecessary), we require that the transaction lock
    * is held when notifying. However, we can get deadlocks between a client
    * and the thread that invokes notifications w.r.t the transaction lock and
    * the exposed model mutex. To fix this, when failing to grab the
    * transaction lock, we set this flag and defer the notification until the
    * mainloop polls this value.
    */
    volatile int        deferred_notification_event;
    pthread_mutex_t     deferred_notification_lock;
};