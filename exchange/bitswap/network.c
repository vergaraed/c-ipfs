/***
 * This implements the BitswapNetwork. Members of this network can fill requests and
 * smartly handle queues of local and remote requests.
 *
 * For a somewhat accurate diagram of how this may work, @see https://github.com/ipfs/js-ipfs-bitswap
 */

#include "ipfs/exchange/bitswap/network.h"
#include "ipfs/exchange/bitswap/peer_request_queue.h"

/****
 * send a message to a particular peer
 * @param context the BitswapContext
 * @param peer the peer that is the recipient
 * @param message the message to send
 */
int ipfs_bitswap_network_send_message(const struct BitswapContext* context, struct Libp2pPeer* peer, const struct BitswapMessage* message) {
	// get a connection to the peer
	if (peer->connection_type != CONNECTION_TYPE_CONNECTED) {
		libp2p_peer_connect(&context->ipfsNode->identity->private_key, peer, 10);
		if(peer->connection_type != CONNECTION_TYPE_CONNECTED)
			return 0;
	}
	// protobuf the message
	size_t buf_size = ipfs_bitswap_message_protobuf_encode_size(message);
	uint8_t* buf = (uint8_t*) malloc(buf_size + 20);
	if (buf == NULL)
		return 0;
	if (!ipfs_bitswap_message_protobuf_encode(message, &buf[20], buf_size, &buf_size)) {
		free(buf);
		return 0;
	}
	// tack on the protocol header
	memcpy(buf, "/ipfs/bitswap/1.1.0\n", 20);
	buf_size += 20;
	// send it
	int bytes_written = peer->sessionContext->default_stream->write(peer->sessionContext, buf, buf_size);
	if (bytes_written <= 0) {
		free(buf);
		return 0;
	}
	free(buf);
	return 1;
}

/***
 * Handle a raw incoming bitswap message from the network
 * @param node us
 * @param sessionContext the connection context
 * @param bytes the message
 * @param bytes_size the size of the message
 * @returns true(1) on success, false(0) otherwise.
 */
int ipfs_bitswap_network_handle_message(const struct IpfsNode* node, const struct SessionContext* sessionContext, const uint8_t* bytes, size_t bytes_length) {
	struct BitswapContext* bitswapContext = (struct BitswapContext*)node->exchange->exchangeContext;
	// strip off the protocol header
	int start = -1;
	for(int i = 0; i < bytes_length; i++) {
		if (bytes[i] == '\n') {
			start = i+1;
			break;
		}
	}
	if (start == -1)
		return 0;
	// un-protobuf the message
	struct BitswapMessage* message = NULL;
	if (!ipfs_bitswap_message_protobuf_decode(&bytes[start], bytes_length - start, &message))
		return 0;
	// process the message
	// payload - what we want
	for(int i = 0; i < message->payload->total; i++) {
		struct Block* blk = (struct Block*)libp2p_utils_vector_get(message->payload, i);
		node->exchange->HasBlock(node->exchange, blk);
	}
	// wantlist - what they want
	if (message->wantlist != NULL && message->wantlist->entries != NULL && message->wantlist->entries->total > 0) {
		// get the peer
		struct Libp2pPeer* peer = libp2p_peerstore_get_peer(node->peerstore, (unsigned char*)sessionContext->remote_peer_id, strlen(sessionContext->remote_peer_id));
		struct PeerRequestEntry* queueEntry = ipfs_bitswap_peer_request_queue_find_entry(bitswapContext->peerRequestQueue, peer);
		for(int i = 0; i < message->wantlist->entries->total; i++) {
			struct WantlistEntry* entry = (struct WantlistEntry*) libp2p_utils_vector_get(message->wantlist->entries, i);
			// turn the "block" back into a cid
			struct Cid* cid = NULL;
			if (!ipfs_cid_protobuf_decode(entry->block, entry->block_size, &cid)) {
				ipfs_cid_free(cid);
				return 0;
			}
			// add the cid to their queue
			libp2p_utils_vector_add(queueEntry->current->cids, cid);
		}
	}
	return 1;
}