#include <sys/time.h>

#define RADIO_RFD900 1
#define RADIO_BARRETT_HF 2
#define RADIO_CODAN_HF 3
#define RADIO_RF95 4

#define RADIO_ALE_2G (1<<0)
#define RADIO_ALE_3G (1<<1)

// For now we use a fixed link MTU for all radio types for now.
// For ALE 2G we fragment frames.  We can revise this for ALE 3G large message blocks,
// and when we implement a better analog modem down the track.
#define LINK_MTU 200

extern struct sync_state *sync_state;
#define SYNC_SALT_LEN 8

#define SERVALD_STOP "/serval/servald stop"
#define DEFAULT_BROADCAST_ADDRESSES "10.255.255.255","192.168.2.255","192.168.2.1"
#define SEND_HELP_MESSAGE "/serval/servald meshms send message `/serval/servald id self | tail -1` `cat /dos/helpdesk.sid` '"

#define C fprintf(stderr,"%s:%d CHECKPOINT in %s()\n",__FILE__,__LINE__,__FUNCTION__)

// Set packet TX interval details (all in ms)
#define INITIAL_AVG_PACKET_TX_INTERVAL 1000
#define INITIAL_PACKET_TX_INTERVAL_RANDOMNESS 250

/* Ideal number of packets per 4 second period.
   128K air interface with 256 byte (=2K bit) packets = 64 packets per second.
   But we don't want to go that high for a few reasons:
   1. It would crash the FTDI serial drivers on Macs (and I develop on one)
   2. With a simple CSMA protocol, we should aim to keep below 30% channel
      utilisation to minimise colissions.
   3. We are likely to have hidden sender problems.
   4. We don't want to suck too much power.

   So we will aim to keep channel utilisation down around 10%.
   64 * 10% * 4 seconds = 204 packets per 4 seconds.
 */
#define TARGET_TRANSMISSIONS_PER_4SECONDS 26

// BAR consists of:
// 8 bytes : BID prefix
// 8 bytes : version
// 4 bytes : recipient prefix
// 1 byte : size and meshms flag byte
#define BAR_LENGTH (8+8+4+1)

struct segment_list {
  unsigned char *data;
  int start_offset;
  int length;
  struct segment_list *prev,*next;
};

struct partial_bundle {
  // Data from the piece headers for keeping track
  char *bid_prefix;
  long long bundle_version;

  int recent_bytes;
  
  struct segment_list *manifest_segments;
  int manifest_length;

  struct segment_list *body_segments;
  int body_length;
};

struct peer_state {
  char *sid_prefix;
  unsigned char sid_prefix_bin[4];

  // random 32 bit instance ID, used to work out when LBARD has died and restarted
  // on a peer, so that we can restart the sync process.
  unsigned int instance_id;
  
  unsigned char *last_message;
  time_t last_message_time;
  // if last_message_time is more than this many seconds ago, then they aren't
  // considered an active peer, and are excluded from rhizome rank calculations
  // and various other things.
#define PEER_KEEPALIVE_INTERVAL 20
  int last_message_number;

#ifdef SYNC_BY_BAR
  // BARs we have seen from them.
  int bundle_count;
#define MAX_PEER_BUNDLES 100000
  int bundle_count_alloc;
  char **bid_prefixes;
  long long *versions;
  unsigned char *size_bytes;
  unsigned char *insert_failures;
#else

  // Bundle we are currently transfering to this peer
  int tx_bundle;
  int tx_bundle_priority;
  int tx_bundle_manifest_offset;
  int tx_bundle_body_offset;
  // number of http fetch errors for a manifest/payload we tolerate, before
  // discarding this bundle and trying to send the next.
#define MAX_CACHE_ERRORS 5
  int tx_cache_errors;

  /* Bundles we want to send to this peer
     In theory, we can have a relatively short queue, since we intend to rebuild the
     tree periodically. Thus any bundles received will cause the new sync process to
     not insert those in the TX queue, and thus allow the transfer of the next 
     MAX_TXQUEUE_LEN highest priority bundles. */
#define MAX_TXQUEUE_LEN 10
  int tx_queue_len; 
  int tx_queue_bundles[MAX_TXQUEUE_LEN];
  unsigned int tx_queue_priorities[MAX_TXQUEUE_LEN];
  int tx_queue_overflow;
#endif
  // Bundles this peer is transferring.
  // The bundle prioritisation algorithm means that the peer may announce pieces
  // of several bundles interspersed, e.g., a large bundle may be temporarily
  // deferred due to the arrival of a smaller bundle, or the arrival of a peer
  // for whom that peer has bundles with the new peer as the recipient.
  // So we need to carry state for some plurality of bundles being announced.
  // Note that we don't (currently) build bundles from announcements by multiple
  // peers, as a small protection against malicious nodes offering fake bundle
  // pieces that will result in the crypto checksums failing at the end.
#define MAX_BUNDLES_IN_FLIGHT 16
  struct partial_bundle partials[MAX_BUNDLES_IN_FLIGHT];  
};

struct recent_bundle {
  char *bid_prefix;
  long long bundle_version;
  time_t timeout;
};

extern long long start_time;
extern int my_time_stratum;
extern int radio_transmissions_byus;
extern int radio_transmissions_seen;
extern long long last_message_update_time;
extern long long congestion_update_time;
extern int message_update_interval;
extern int message_update_interval_randomness;
extern long long last_message_update_time;
extern long long congestion_update_time;

extern int monitor_mode;

extern char message_buffer[];
extern int message_buffer_size;
extern int message_buffer_length;

struct bundle_record {
  int index; // position in array of bundles
  
  char *service;
  char *bid_hex;
  unsigned char bid_bin[32];
  long long version;
  char *author;
  int originated_here_p;
#ifdef SYNC_BY_BAR
#define TRANSMIT_NOW_TIMEOUT 2
  time_t transmit_now;
  int announce_bar_now;
#else
  sync_key_t sync_key;
#endif
  long long length;
  char *filehash;
  char *sender;
  char *recipient;

  // The last time we announced this bundle in full.
  time_t last_announced_time;
  // The last version of the bundle that we announced.
  long long last_version_of_manifest_announced;
  // The furthest through the file that we have announced during the current
  // attempt at announcing it (which may be interrupted by the presence of bundles
  // with a higher priority).
  long long last_offset_announced;
  // Similarly for the manifest
  long long last_manifest_offset_announced;
  
  long long last_priority;
  int num_peers_that_dont_have_it;
};

// New unified BAR + optional bundle record for BAR tree structure
typedef struct bar_data {
  // BAR fields
  long long bid_prefix_bin;
  long long version;
  long long recipient_sid_prefix_bin;
  int size_byte;
  
} bar_data;

typedef struct bundle_data {
  // Bundle fields, only valid if have_bundle != 0
  // All fields are initialised as fixed-length strings so that we can avoid
  // malloc.
  char service[40];
  char bid_hex[32*2+1];
  char author[32*2+1];
  int originated_here_p;
  char filehash[64*2+1];
  char sender[32*2+1];
  char recipient[32*2+1];
} bundle_data;
  
typedef struct bundle_node {
  // Do we have the bundle?
  // If the node exists, we must have the BAR, because we can make the BAR from a
  // locally stored bundle.  However, a BAR received from a peer can be present,
  // without us having the bundle in Rhizome (yet).
  int have_bundle;
  int have_manifest;
  int have_payload;

  bar_data *bar;
  bundle_data *bundle;
  
  // Priority of bundle based on attributes
  int intrinsic_priority;

  // XOR of all BARs of nodes below this one.
  unsigned char node_xor[BAR_LENGTH];
  
  // Links to other elements in the tree
  struct bundle_node *parent,*left, *right;
} bundle_node;
  
extern unsigned int my_instance_id;

#define MAX_PEERS 1024
extern struct peer_state *peer_records[MAX_PEERS];
extern int peer_count;

#define MAX_BUNDLES 10000
extern struct bundle_record bundles[MAX_BUNDLES];
extern int bundle_count;

extern char *bid_of_cached_bundle;
extern long long cached_version;
// extern int cached_manifest_len;
// extern unsigned char *cached_manifest;
extern int cached_manifest_encoded_len;
extern unsigned char *cached_manifest_encoded;
extern int cached_body_len;
extern unsigned char *cached_body;

extern FILE *debug_file;
extern int debug_radio;
extern int debug_pieces;
extern int debug_announce;
extern int debug_pull;
extern int debug_insert;
extern int debug_radio_rx;
extern int debug_gpio;
extern int debug_insert;
extern int debug_message_pieces;
extern int debug_sync;
extern int debug_sync_keys;
extern int debug_bundlelog;
extern int debug_noprioritisation;
extern int radio_silence_count;
extern int meshms_only;
extern long long min_version;
extern int time_slave;
extern long long start_time;

int saw_piece(char *peer_prefix,int for_me,
	      char *bid_prefix, unsigned char *bid_prefix_bin,
	      long long version,
	      long long piece_offset,int piece_bytes,int is_end_piece,
	      int is_manifest_piece,unsigned char *piece,

	      char *prefix, char *servald_server, char *credential);
int saw_length(char *peer_prefix,char *bid_prefix,long long version,
	       int body_length);
int saw_message(unsigned char *msg,int len,char *my_sid,
		char *prefix, char *servald_server,char *credential);
int load_rhizome_db(int timeout,
		    char *prefix, char *serval_server,
		    char *credential, char **token);
int parse_json_line(char *line,char fields[][8192],int num_fields);
int rhizome_update_bundle(unsigned char *manifest_data,int manifest_length,
			  unsigned char *body_data,int body_length,
			  char *servald_server,char *credential);
int prime_bundle_cache(int bundle_number,char *prefix,
		       char *servald_server, char *credential);
int hex_byte_value(char *hexstring);
int find_highest_priority_bundle();
int find_highest_priority_bar();
int find_peer_by_prefix(char *peer_prefix);
int clear_partial(struct partial_bundle *p);
int dump_partial(struct partial_bundle *p);
int merge_segments(struct segment_list **s);
int free_peer(struct peer_state *p);
int peer_note_bar(struct peer_state *p,
		  char *bid_prefix,long long version, char *recipient_prefix,
		  int size_byte);
int announce_bundle_piece(int bundle_number,int *offset,int mtu,unsigned char *msg,
			  char *prefix,char *servald_server, char *credential,
			  int target_peer);
int update_my_message(int serialfd,
		      unsigned char *my_sid, int mtu,unsigned char *msg_out,
		      char *servald_server,char *credential);
size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream);
int radio_send_message(int serialfd, unsigned char *msg_out,int offset);
int radio_receive_bytes(unsigned char *buffer, int bytes, int monitor_mode);
ssize_t write_all(int fd, const void *buf, size_t len);
int radio_read_bytes(int serialfd, int monitor_mode);
ssize_t read_nonblock(int fd, void *buf, size_t len);

int http_get_simple(char *server_and_port, char *auth_token,
		    char *path, FILE *outfile, int timeout_ms,
		    long long *last_read_time);
int http_post_bundle(char *server_and_port, char *auth_token,
		     char *path,
		     unsigned char *manifest_data, int manifest_length,
		     unsigned char *body_data, int body_length,
		     int timeout_ms);

int generate_progress_string(struct partial_bundle *partial,
			     char *progress,int progress_size);
int show_progress();
int request_wanted_content_from_peers(int *offset,int mtu, unsigned char *msg_out);
int dump_segment_list(struct segment_list *s);

int serial_setup_port_with_speed(int fd,int speed);
int status_dump();
int status_log(char *msg);

long long calculate_bundle_intrinsic_priority(char *bid,
					      long long length,
					      long long version,
					      char *service,
					      char *recipient,
					      int insert_failures);
int bid_to_peer_bundle_index(int peer,char *bid_hex);
int manifest_extract_bid(unsigned char *manifest_data,char *bid_hex);
int we_have_this_bundle_or_newer(char *bid_prefix, long long version);
int register_bundle(char *service,
		    char *bid,
		    char *version,
		    char *author,
		    char *originated_here,
		    long long length,
		    char *filehash,
		    char *sender,
		    char *recipient);
long long size_byte_to_length(unsigned char size_byte);
char *bundle_recipient_if_known(char *bid_prefix);
int rhizome_log(char *service,
		char *bid,
		char *version,
		char *author,
		char *originated_here,
		long long length,
		char *filehash,
		char *sender,
		char *recipient,
		char *message);
int manifest_text_to_binary(unsigned char *text_in, int len_in,
			    unsigned char *bin_out, int *len_out);
int manifest_binary_to_text(unsigned char *bin_in, int len_in,
			    unsigned char *text_out, int *len_out);
int manifest_get_field(unsigned char *manifest, int manifest_len,
		       char *fieldname,
		       char *field_value);
int monitor_log(char *sender_prefix, char *recipient_prefix,char *msg);
int bytes_to_prefix(unsigned char *bytes_in,char *prefix_out);
int saw_timestamp(char *sender_prefix,int stratum, struct timeval *tv);
int http_process(struct sockaddr *cliaddr,
		 char *servald_server,char *credential,
		 char *my_sid_hex,
		 int socket);
int chartohex(int c);
int random_active_peer();
int append_bytes(int *offset,int mtu,unsigned char *msg_out,
		 unsigned char *data,int count);
int sync_tree_receive_message(struct peer_state *p, unsigned char *msg);
int lookup_bundle_by_sync_key(uint8_t bundle_sync_key[KEY_LEN]);
int peer_queue_bundle_tx(struct peer_state *p,struct bundle_record *b, int priority);
int sync_parse_ack(struct peer_state *p,unsigned char *msg);
int http_post_meshms(char *server_and_port, char *auth_token,
		     char *message,char *sender,char *recipient,
		     int timeout_ms);
int sync_setup();
int sync_by_tree_stuff_packet(int *offset,int mtu, unsigned char *msg_out,
			      char *sid_prefix_bin,
			      char *servald_server,char *credential);
int sync_tell_peer_we_have_this_bundle(int peer, int bundle);
int sync_tell_peer_we_have_the_bundle_of_this_partial(int peer, int partial);
int sync_schedule_progress_report(int peer, int partial);
int bundle_calculate_tree_key(sync_key_t *sync_key,
			      uint8_t sync_tree_salt[SYNC_SALT_LEN],
			      char *bid,
			      long long version,
			      long long length,
			      char *filehash);

int urandombytes(unsigned char *buf, size_t len);
int active_peer_count();
int sync_dequeue_bundle(struct peer_state *p,int bundle);
int meshms_parse_command(int argc,char **argv);
int http_list_meshms_conversations(char *server_and_port, char *auth_token,
				   char *participant,int timeout_ms);
int http_list_meshms_messages(char *server_and_port, char *auth_token,
			      char *sender, char *recipient,int timeout_ms);
int http_send_meshms_message(char *server_and_port, char *auth_token,
			     char *sender, char *recipient,char *message,
			     int timeout_ms);
int hextochar(int h);
int peer_queue_list_dump(struct peer_state *p);
int sync_remember_recently_received_bundle(char *bid_prefix, long long version);
int sync_is_bundle_recently_received(char *bid_prefix, long long version);
int sync_tell_peer_we_have_bundle_by_id(int peer,unsigned char *bid_bin,
					long long version);
unsigned char *bid_prefix_hex_to_bin(char *hex);
int progress_report_bundle_receipts();
int progress_log_bundle_receipt(char *bid_prefix, long long version);

int http_get_async(char *server_and_port, char *auth_token,
		   char *path, int timeout_ms);
int http_read_next_line(int sock, char *line, int *len, int maxlen);
int load_rhizome_db_async(char *servald_server,
			  char *credential, char *token);

int lookup_bundle_by_prefix_bin_and_version_exact(unsigned char *prefix, long long version);
int lookup_bundle_by_prefix_bin_and_version_or_older(unsigned char *prefix, long long version);
int lookup_bundle_by_prefix_bin_and_version_or_newer(unsigned char *prefix, long long version);

int radio_set_type(int radio_type);
int radio_set_feature(int bitmask);
int hf_read_configuration(char *filename);
int hf_serviceloop(int fd);
int uhf_serviceloop(int fd);
int radio_get_type();
int uhf_receive_bytes(unsigned char *bytes,int count);
int hf_codan_receive_bytes(unsigned char *bytes,int count);
int hf_barrett_receive_bytes(unsigned char *bytes,int count);
int radio_send_message_codanhf(int serialfd,unsigned char *out, int len);
int radio_send_message_barretthf(int serialfd,unsigned char *out, int len);
int radio_send_message_rf95(int serialfd,unsigned char *out, int len);
int saw_packet(unsigned char *packet_data,int packet_bytes,
	       char *my_sid_hex,char *prefix,
	       char *servald_server,char *credential);
int radio_ready();
int hf_radio_ready();
int hf_radio_pause_for_turnaround();
int hf_radio_send_now();
