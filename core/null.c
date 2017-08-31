#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "libp2p/conn/session.h"
#include "libp2p/net/multistream.h"
#include "libp2p/net/p2pnet.h"
#include "libp2p/net/protocol.h"
#include "libp2p/nodeio/nodeio.h"
#include "libp2p/os/utils.h"
#include "libp2p/record/message.h"
#include "libp2p/routing/dht_protocol.h"
#include "libp2p/secio/secio.h"
#include "libp2p/utils/logger.h"
#include "ipfs/core/daemon.h"
#include "ipfs/core/ipfs_node.h"
#include "ipfs/exchange/bitswap/network.h"
#include "ipfs/journal/journal.h"
#include "ipfs/merkledag/merkledag.h"
#include "ipfs/merkledag/node.h"
#include "ipfs/routing/routing.h"
#include "ipfs/util/thread_pool.h"

#define BUF_SIZE 4096

// this should be set to 5 for normal operation, perhaps higher for debugging purposes
#define DEFAULT_NETWORK_TIMEOUT 5

static int null_shutting_down = 0;

/**
 * We've received a connection. Find out what they want.
 *
 * @param ptr a pointer to a null_connection_params struct
 */
void ipfs_null_connection (void *ptr) {
    struct null_connection_params *connection_param = (struct null_connection_params*) ptr;
    int retVal = 0;

    struct SessionContext* session = libp2p_session_context_new();
    if (session == NULL) {
		libp2p_logger_error("null", "Unable to allocate SessionContext. Out of memory?\n");
		return;
    }

    session->insecure_stream = libp2p_net_multistream_stream_new(connection_param->file_descriptor, connection_param->ip, connection_param->port);
    session->default_stream = session->insecure_stream;
    session->datastore = connection_param->local_node->repo->config->datastore;
    session->filestore = connection_param->local_node->repo->config->filestore;

    libp2p_logger_info("null", "Connection %d, count %d\n", connection_param->file_descriptor, *(connection_param->count));

	if (libp2p_net_multistream_negotiate(session)) {
		// Someone has connected and successfully negotiated multistream. Now talk to them...
		int unsuccessful_max = 30;
		int unsuccessful_counter = 0;
		for(;;) {
			// Wait for them to ask something...
			unsigned char* results = NULL;
			size_t bytes_read = 0;
			if (null_shutting_down) {
				libp2p_logger_debug("null", "%s null shutting down before read.\n", connection_param->local_node->identity->peer->id);
				// this service is shutting down. Ignore the request and exit the loop
				break;
			}
			// see if we have something to read
			retVal = session->default_stream->peek(session);
			if (retVal < 0) { // error
				libp2p_logger_debug("null", "Peer returned %d. Exiting loop\n", retVal);
				retVal = -1;
				break;
			}
			if (retVal == 0) { // nothing to read
				sleep(1);
				unsuccessful_counter++;
				if (unsuccessful_counter >= unsuccessful_max) {
					libp2p_logger_debug("null", "We've tried %d times in the daemon loop. Exiting.\n", unsuccessful_counter);
					retVal = -1;
					break;
				}
				continue;
			}
			if (retVal > 0 && !session->default_stream->read(session, &results, &bytes_read, DEFAULT_NETWORK_TIMEOUT) ) {
				// it said it was ready, but something happened
				libp2p_logger_debug("null", "Peek said there was something there, but there was not. Exiting.\n");
				retVal = -1;
				break;
			}
			if (null_shutting_down) {
				libp2p_logger_debug("null", "%s null shutting down after read.\n", connection_param->local_node->identity->peer->id);
				// this service is shutting down. Ignore the request and exit the loop
				retVal = -1;
				break;
			}

			// We actually got something. Process the request...
			unsuccessful_counter = 0;
			libp2p_logger_debug("null", "Read %lu bytes from a stream tranaction\n", bytes_read);
			retVal = libp2p_protocol_marshal(results, bytes_read, session, connection_param->local_node->protocol_handlers);
			free(results);
			if (retVal == -1) {
				libp2p_logger_debug("null", "protocol_marshal returned error.\n");
				break;
			} else if (retVal == 0) {
				// clean up, but let someone else handle this from now on
				libp2p_logger_debug("null", "protocol_marshal returned 0. The daemon will no longer handle this.\n");
				break;
			} else {
				libp2p_logger_debug("null", "protocol_marshal returned 1. Looping again.\n");
			}
		}
   	} else {
   		libp2p_logger_log("null", LOGLEVEL_DEBUG, "Multistream negotiation failed\n");
   	}

	(*(connection_param->count))--; // update counter.
	if (connection_param->ip != NULL)
		free(connection_param->ip);
	free (connection_param);
	if (retVal != 0) {
		libp2p_logger_debug("null", "%s Freeing session context.\n", connection_param->local_node->identity->peer->id);
		//libp2p_session_context_free(session);
	}
    return;
}

int ipfs_null_do_maintenance(struct IpfsNode* local_node, struct Libp2pPeer* peer) {
	if (peer == NULL) {
		return 0;
	}
	if (peer->is_local) {
		return 1;
	}
	// Is this peer one of our backup partners?
	struct ReplicationPeer* replication_peer = repo_config_get_replication_peer(local_node->repo->config->replication, peer);
	long long announce_secs = local_node->repo->config->replication->announce_minutes * 60;
	// If so, has there been enough time since the last attempt a backup?
	if (replication_peer != NULL) {
		announce_secs -= os_utils_gmtime() - replication_peer->lastConnect;
		libp2p_logger_debug("null", "Checking to see if we should send backup notification to peer %s. Time since last backup: %lld.\n", libp2p_peer_id_to_string(replication_peer->peer), announce_secs);
	}
	// should we attempt to connect if we're not already?
	if (replication_peer != NULL && local_node->repo->config->replication->announce && announce_secs < 0) {
		// try to connect if we aren't already
		if (peer->connection_type != CONNECTION_TYPE_CONNECTED) {
			if (!libp2p_peer_connect(&local_node->identity->private_key, peer, local_node->peerstore, 2)) {
				return 0;
			}
		}
		// attempt a backup, don't forget to reset timer
		libp2p_logger_debug("null", "Attempting a sync of node %s.\n", libp2p_peer_id_to_string(peer));
		ipfs_journal_sync(local_node, replication_peer);
		libp2p_logger_debug("null", "Sync message sent. Maintenance complete for node %s.\n", libp2p_peer_id_to_string(peer));
	} else {
		if (peer->sessionContext != NULL && os_utils_gmtime() - peer->sessionContext->last_comm_epoch > 180) {
			// try a ping, but only if we're connected
			libp2p_logger_debug("null", "Attempting ping of %s.\n", peer->id);
			if (peer->connection_type == CONNECTION_TYPE_CONNECTED && !local_node->routing->Ping(local_node->routing, peer)) {
				libp2p_logger_debug("null", "Attempted ping of %s failed.\n", peer->id);
				peer->connection_type = CONNECTION_TYPE_NOT_CONNECTED;
			}
		}
	}
	return 1;
}


/***
 * Called by the daemon to listen for connections
 * @param ptr a pointer to an IpfsNodeListenParams struct
 * @returns nothing useful.
 */
void* ipfs_null_listen (void *ptr)
{
    int socketfd, s, count = 0;
    threadpool thpool = thpool_init(25);
    struct IpfsNodeListenParams *listen_param;
    struct null_connection_params *connection_param;

    listen_param = (struct IpfsNodeListenParams*) ptr;

    if ((socketfd = socket_listen(socket_tcp4(), &(listen_param->ipv4), &(listen_param->port))) <= 0) {
        libp2p_logger_error("null", "Failed to init null router. Address: %d, Port: %d\n", listen_param->ipv4, listen_param->port);
        return (void*) 2;
    }

    libp2p_logger_error("null", "Ipfs listening on %d\n", listen_param->port);

    // when we have nothing to do, check on the connections to see if we're still connected
    struct Libp2pLinkedList* current_peer_entry = NULL;
    if (listen_param->local_node->peerstore->head_entry != NULL)
    		current_peer_entry = listen_param->local_node->peerstore->head_entry;

    // the main loop, listening for new connections
    for (;;) {
		//libp2p_logger_debug("null", "%s Attempting socket read with fd %d.\n", listen_param->local_node->identity->peer->id, socketfd);
		int numDescriptors = socket_read_select4(socketfd, 2);
		if (null_shutting_down) {
			libp2p_logger_debug("null", "%s null_listen shutting down.\n", listen_param->local_node->identity->peer->id);
			break;
		}
		if (numDescriptors > 0) {
			s = socket_accept4(socketfd, &(listen_param->ipv4), &(listen_param->port));
			if (count >= CONNECTIONS) { // limit reached.
				close (s);
				continue;
			}

			count++;
			connection_param = malloc (sizeof (struct null_connection_params));
			if (connection_param) {
				connection_param->file_descriptor = s;
				connection_param->count = &count;
				connection_param->local_node = listen_param->local_node;
				connection_param->port = listen_param->port;
				connection_param->ip = malloc(INET_ADDRSTRLEN);
				if (inet_ntop(AF_INET, &(listen_param->ipv4), connection_param->ip, INET_ADDRSTRLEN) == NULL) {
					free(connection_param->ip);
					connection_param->ip = NULL;
					connection_param->port = 0;
				}
				// Create pthread for ipfs_null_connection.
				thpool_add_work(thpool, ipfs_null_connection, connection_param);
			}
    		} else {
    			// timeout... do maintenance
    			struct PeerEntry* entry = current_peer_entry->item;
    			ipfs_null_do_maintenance(listen_param->local_node, entry->peer);
    			if (current_peer_entry != NULL)
    				current_peer_entry = current_peer_entry->next;
    			if (current_peer_entry == NULL)
    				current_peer_entry = listen_param->local_node->peerstore->head_entry;
    		}
    }

    thpool_destroy(thpool);

    close(socketfd);

    return (void*) 2;
}

int ipfs_null_shutdown() {
	null_shutting_down = 1;
	return 1;
}
