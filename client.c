#define _GNU_SOURCE
#include <infiniband/verbs.h>
#include <linux/types.h>
//#include "config.h"
#include <assert.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <malloc.h>
#include <getopt.h>
#include <arpa/inet.h>
#include <time.h>
#include <inttypes.h>

//#include "pingpong.h"

#define EAGER_PROTOCOL_LIMIT (1 << 12) /* 4KB limit */
#define MAX_TEST_SIZE (10 * EAGER_PROTOCOL_LIMIT)
#define TEST_LOCATION "~/www/"
#define NUM_SOCKETS 1
typedef int bool;
#define true 1
#define false 0

static int page_size;
static int use_ts;
static int portNum;
static char* servername;

enum packet_type {
    EAGER_GET_REQUEST,
    EAGER_GET_RESPONSE,
    EAGER_SET_REQUEST,
    EAGER_SET_RESPONSE,

    RENDEZVOUS_GET_REQUEST,
    RENDEZVOUS_GET_RESPONSE,
    RENDEZVOUS_SET_REQUEST,
    RENDEZVOUS_SET_RESPONSE,

#ifdef EX4
    FIND,
    LOCATION,
#endif
};

struct packet {
    enum packet_type type; /* What kind of packet/protocol is this */
    union {
        /* The actual packet type will determine which struct will be used: */

        struct {
            /* TODO */
            unsigned keyLen;
            char key[0]; //key will be an array of size len
            
        } eager_get_request;
		
        struct {
            unsigned valueLen;
            char value[0];
        } eager_get_response;

        /* EAGER PROTOCOL PACKETS */
        struct {
#ifdef EX4
            unsigned value_length; /* value is binary, so needs to have length! */
#endif
            unsigned keyLen;
            unsigned valueLen;
            char key_and_value[0]; /* null terminator between key and value */
        } eager_set_request;

        struct {
            /* TODO check what server responds to eager set req*/
            //unsigned keyLen;
            //unsigned valueLen;
            //char key_and_value[0];
        } eager_set_response;

        /* RENDEZVOUS PROTOCOL PACKETS */
        struct {
            int keyLen;
            char key[0];
        } rndv_get_request;

        struct {
            uint64_t remote_address;
            uint32_t rkey;
            int valueLen;
        } rndv_get_response;

        struct {
            int keyLen;
            int valueLen;
            char key[0];
        } rndv_set_request;

        struct {
            uint64_t remote_address;
            uint32_t rkey;
        } rndv_set_response;

		/* TODO - maybe there are more packet types? */
					
#ifdef EX4
        struct {
            unsigned num_of_servers;
            char key[0];
        } find;

        struct {
            unsigned selected_server;
        } location;
#endif
    };
};

struct pingpong_context {
	struct ibv_context	*context; //the connections context (channels live inside the context)
	struct ibv_comp_channel *channel;
	struct ibv_pd		*pd;
	struct ibv_mr		*mr; //this is the memory region we work with
	struct ibv_dm		*dm;
	union {
		struct ibv_cq		*cq;
		struct ibv_cq_ex	*cq_ex;
	} cq_s;
	struct ibv_qp		*qp[NUM_SOCKETS]; //this is the queue pair array we work with
	char			*buf;
	int			 size;
	int			 send_flags;
	int			 rx_depth;
	int			 pending;
	struct ibv_port_attr     portinfo;
	uint64_t		 completion_timestamp_mask;
};

struct pingpong_dest {
	int lid;
	int qpn;
	int psn;
	union ibv_gid gid;
};

struct kv_handle
{
    struct pingpong_context * ctx;//context
    int entryLen;
    int * keyLen;
    int * valueLen;
    char ** keys;
    char ** values;
};

enum ibv_mtu pp_mtu_to_enum(int mtu)
{
	switch (mtu) {
	case 256:  return IBV_MTU_256;
	case 512:  return IBV_MTU_512;
	case 1024: return IBV_MTU_1024;
	case 2048: return IBV_MTU_2048;
	case 4096: return IBV_MTU_4096;
	default:   return 0;
	}
}

enum {
	PINGPONG_RECV_WRID = 1,
	PINGPONG_SEND_WRID = 2,
};

void wire_gid_to_gid(const char *wgid, union ibv_gid *gid)
{
	char tmp[9];
	__be32 v32;
	int i;
	uint32_t tmp_gid[4];

	for (tmp[8] = 0, i = 0; i < 4; ++i) {
		memcpy(tmp, wgid + i * 8, 8);
		sscanf(tmp, "%x", &v32);
		tmp_gid[i] = be32toh(v32);
	}
	memcpy(gid, tmp_gid, sizeof(*gid));
}

void gid_to_wire_gid(const union ibv_gid *gid, char wgid[])
{
	uint32_t tmp_gid[4];
	int i;

	memcpy(tmp_gid, gid, sizeof(tmp_gid));
	for (i = 0; i < 4; ++i)
		sprintf(&wgid[i * 8], "%08x", htobe32(tmp_gid[i]));
}

int pp_get_port_info(struct ibv_context *context, int port,
		     struct ibv_port_attr *attr)
{
	return ibv_query_port(context, port, attr);
}

static struct ibv_cq *pp_cq(struct pingpong_context *ctx)
{
	return use_ts ? ibv_cq_ex_to_cq(ctx->cq_s.cq_ex) :
		ctx->cq_s.cq;
}
//connect qp_num_to_connect in context to this pingpong dest.
static int pp_connect_ctx(struct pingpong_context *ctx, int port, int my_psn,
			  enum ibv_mtu mtu, int sl,
			  struct pingpong_dest *dest, int sgid_idx,int qp_num_to_connect)
{
	struct ibv_qp_attr attr = {
		.qp_state		= IBV_QPS_RTR,
		.path_mtu		= mtu,
		.dest_qp_num		= dest->qpn,
		.rq_psn			= dest->psn,
		.max_dest_rd_atomic	= 1,
		.min_rnr_timer		= 12,
		.ah_attr		= {
			.is_global	= 0,
			.dlid		= dest->lid,
			.sl		= sl,
			.src_path_bits	= 0,
			.port_num	= port
		}
	};

	if (dest->gid.global.interface_id) {
		attr.ah_attr.is_global = 1;
		attr.ah_attr.grh.hop_limit = 1;
		attr.ah_attr.grh.dgid = dest->gid;
		attr.ah_attr.grh.sgid_index = sgid_idx;
	}
	if (ibv_modify_qp(((*ctx).qp[qp_num_to_connect]), &attr,
			  IBV_QP_STATE              |
			  IBV_QP_AV                 |
			  IBV_QP_PATH_MTU           |
			  IBV_QP_DEST_QPN           |
			  IBV_QP_RQ_PSN             |
			  IBV_QP_MAX_DEST_RD_ATOMIC |
			  IBV_QP_MIN_RNR_TIMER)) {
		fprintf(stderr, "Failed to modify QP to RTR\n");
		return 1;
	}

	attr.qp_state	    = IBV_QPS_RTS;
	attr.timeout	    = 14;// 0 is infinite wait
	attr.retry_cnt	    = 7;
	attr.rnr_retry	    = 7;
	attr.sq_psn	    = my_psn;
	attr.max_rd_atomic  = 1;
	if (ibv_modify_qp(((*ctx).qp[qp_num_to_connect]), &attr,
			  IBV_QP_STATE              |
			  IBV_QP_TIMEOUT            |
			  IBV_QP_RETRY_CNT          |
			  IBV_QP_RNR_RETRY          |
			  IBV_QP_SQ_PSN             |
			  IBV_QP_MAX_QP_RD_ATOMIC)) {
		fprintf(stderr, "Failed to modify QP to RTS\n");
		return 1;
	}

	return 0;
}

static struct pingpong_dest *pp_client_exch_dest(const char *servername, int port,
						 const struct pingpong_dest *my_dest)
{
	struct addrinfo *res, *t;
	struct addrinfo hints = {
		.ai_family   = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	char *service;
	char msg[sizeof "0000:000000:000000:00000000000000000000000000000000"];
	int n;
	int sockfd = -1;
	struct pingpong_dest *rem_dest;
  struct pingpong_dest dest;
	char gid[33];

	if (asprintf(&service, "%d", port) < 0)
		return NULL;
	n = getaddrinfo(servername, service, &hints, &res);

	if (n < 0) {
		fprintf(stderr, "%s for %s:%d\n", gai_strerror(n), servername, port);
		free(service);
		return NULL;
	}

	for (t = res; t; t = t->ai_next) {
		sockfd = socket(t->ai_family, t->ai_socktype, t->ai_protocol);
		if (sockfd >= 0) {
            //sleep(1);
            
			if (!connect(sockfd, t->ai_addr, t->ai_addrlen))
            {
               
				break;
            }
			close(sockfd);
			sockfd = -1;
		}
	}
	freeaddrinfo(res);
	free(service);
  
	if (sockfd < 0) {
		fprintf(stderr, "Couldn't connect to %s:%d\n", servername, port);
		return NULL;
	}
    rem_dest = malloc((sizeof dest) * NUM_SOCKETS);
        if (!rem_dest)
            goto out;
    for (int i = 0; i < NUM_SOCKETS; i = i+1)
    {//
        gid_to_wire_gid(&((my_dest[i]).gid), gid);
       sprintf(msg, "%04x:%06x:%06x:%s", my_dest[i].lid, my_dest[i].qpn,
                                my_dest[i].psn, gid);
       if (write(sockfd, msg, sizeof msg) != sizeof msg) {
            fprintf(stderr, "Couldn't send local address\n");
            goto out;
       }

        if (read(sockfd, msg, sizeof msg) != sizeof msg ||
            write(sockfd, "done", sizeof "done") != sizeof "done") {
            perror("client read/write");
            fprintf(stderr, "Couldn't read/write remote address\n");
            goto out;
        }
//
        

        sscanf(msg, "%x:%x:%x:%s", &rem_dest[i].lid, &rem_dest[i].qpn,
                            &rem_dest[i].psn, gid);
        wire_gid_to_gid(gid, &rem_dest[i].gid);
    }
out:
	close(sockfd);
	return rem_dest;
}

static struct pingpong_context *pp_init_ctx(struct ibv_device *ib_dev, int size,
					    int rx_depth, int port,
					    int use_event)
{
    
	struct pingpong_context *ctx;
	int access_flags = IBV_ACCESS_LOCAL_WRITE;

	ctx = calloc(1, sizeof *ctx);
	if (!ctx)
		return NULL;

	ctx->size       = size;
	ctx->send_flags = IBV_SEND_SIGNALED;
	ctx->rx_depth   = rx_depth;

	ctx->buf = memalign(page_size, size);
	if (!ctx->buf) {
		fprintf(stderr, "Couldn't allocate work buf.\n");
		goto clean_ctx;
	}

	memset(ctx->buf, 0x7b, size);

	ctx->context = ibv_open_device(ib_dev);
	if (!ctx->context) {
		fprintf(stderr, "Couldn't get context for %s\n",
			ibv_get_device_name(ib_dev));
		goto clean_buffer;
	}

	if (use_event) {
		ctx->channel = ibv_create_comp_channel(ctx->context);
		if (!ctx->channel) {
			fprintf(stderr, "Couldn't create completion channel\n");
			goto clean_device;
		}
	} else
		ctx->channel = NULL;

	ctx->pd = ibv_alloc_pd(ctx->context);
	if (!ctx->pd) {
		fprintf(stderr, "Couldn't allocate PD\n");
		goto clean_comp_channel;
	}


	ctx->mr = ibv_reg_mr(ctx->pd, ctx->buf, size, access_flags);

	if (!ctx->mr) {
		fprintf(stderr, "Couldn't register MR\n");
		goto clean_dm;
	}

		ctx->cq_s.cq = ibv_create_cq(ctx->context, rx_depth + 1, NULL,
					     ctx->channel, 0);
	

	if (!pp_cq(ctx)) {
		fprintf(stderr, "Couldn't create CQ\n");
		goto clean_mr;
	}
    for (int i = 0 ; i < NUM_SOCKETS; i = i+1) //create NUM_SOCKETS QP's
	{
		struct ibv_qp_attr attr;
		struct ibv_qp_init_attr init_attr = {
			.send_cq = pp_cq(ctx),
			.recv_cq = pp_cq(ctx),
			.cap     = {
				.max_send_wr  = 1,
				.max_recv_wr  = rx_depth,
				.max_send_sge = 1,
				.max_recv_sge = 1
			},
			.qp_type = IBV_QPT_RC
		};

		((*ctx).qp[i]) = ibv_create_qp(ctx->pd, &init_attr);////////////
		if (!((*ctx).qp[i]))  {
			fprintf(stderr, "Couldn't create QP\n");
			goto clean_cq;
		}

		ibv_query_qp(((*ctx).qp[i]), &attr, IBV_QP_CAP, &init_attr);
		if (init_attr.cap.max_inline_data >= size) {
			ctx->send_flags |= IBV_SEND_INLINE;
		}
	

	
		struct ibv_qp_attr attr2 = {
			.qp_state        = IBV_QPS_INIT,
			.pkey_index      = 0,
			.port_num        = port,
			.qp_access_flags = 0
		};

		if (ibv_modify_qp((*ctx).qp[i], &attr2,
				  IBV_QP_STATE              |
				  IBV_QP_PKEY_INDEX         |
				  IBV_QP_PORT               |
				  IBV_QP_ACCESS_FLAGS)) {
			fprintf(stderr, "Failed to modify QP to INIT\n");
			goto clean_qp;
		}
	}

	return ctx;

clean_qp:
    for (int k = 0 ; k < NUM_SOCKETS; k = k+1)
        ibv_destroy_qp((*ctx).qp[k]);

clean_cq:
	ibv_destroy_cq(pp_cq(ctx));

clean_mr:
	ibv_dereg_mr(ctx->mr);

clean_dm:
	if (ctx->dm)

clean_pd:
	ibv_dealloc_pd(ctx->pd);

clean_comp_channel:
	if (ctx->channel)
		ibv_destroy_comp_channel(ctx->channel);

clean_device:
	ibv_close_device(ctx->context);

clean_buffer:
	free(ctx->buf);

clean_ctx:
	free(ctx);

	return NULL;
}


static int pp_close_ctx(struct pingpong_context *ctx)
{
    for (int k = 0 ; k < NUM_SOCKETS; k = k+1)
        if ( ibv_destroy_qp((*ctx).qp[k])) {
            fprintf(stderr, "Couldn't destroy QP\n");
            return 1;
        }

	if (ibv_destroy_cq(pp_cq(ctx))) {
		fprintf(stderr, "Couldn't destroy CQ\n");
		return 1;
	}

	if (ibv_dereg_mr(ctx->mr)) {
		fprintf(stderr, "Couldn't deregister MR\n");
		return 1;
	}

	if (ctx->dm) {
	}

	if (ibv_dealloc_pd(ctx->pd)) {
		fprintf(stderr, "Couldn't deallocate PD\n");
		return 1;
	}

	if (ctx->channel) {
		if (ibv_destroy_comp_channel(ctx->channel)) {
			fprintf(stderr, "Couldn't destroy completion channel\n");
			return 1;
		}
	}

	if (ibv_close_device(ctx->context)) {
		fprintf(stderr, "Couldn't release context\n");
		return 1;
	}

	free(ctx->buf);
	free(ctx);

	return 0;
}

static int pp_post_recv(struct pingpong_context *ctx, int n)
{
    int qp_num = 0;
	struct ibv_sge list = {
		.addr	= (uintptr_t) ctx->buf,
		.length = ctx->size,
		.lkey	= ctx->mr->lkey
	};
	struct ibv_recv_wr wr = {
		.wr_id	    = PINGPONG_RECV_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
	};
	struct ibv_recv_wr *bad_wr;
	int i;

	for (i = 0; i < n; ++i)
		if (ibv_post_recv((*ctx).qp[qp_num], &wr, &bad_wr))
			break;

	return i;
}

static int pp_post_send(struct pingpong_context *ctx, enum ibv_wr_opcode opcode, unsigned size, const char *local_ptr, void *remote_ptr, uint32_t remote_key)
{
	struct ibv_sge list = {
		.addr	= (uintptr_t) (local_ptr ? local_ptr : ctx->buf),
		.length = size,
		.lkey	= ctx->mr->lkey
	};
	struct ibv_send_wr wr = {
		.wr_id	    = PINGPONG_SEND_WRID,
		.sg_list    = &list,
		.num_sge    = 1,
		.opcode     = opcode,
		.send_flags = IBV_SEND_SIGNALED,
		.next       = NULL
	};
	struct ibv_send_wr *bad_wr;
	
	if (remote_ptr) {
		wr.wr.rdma.remote_addr = (uintptr_t) remote_ptr;
		wr.wr.rdma.rkey = remote_key;
	}
    return ibv_post_send((*ctx).qp[0], &wr, &bad_wr);
	//return ibv_post_send(ctx->qp, &wr, &bad_wr);
}

////////////////////
int pp_wait_completions(struct kv_handle *handle, int iters,char ** answerBuffer)
{
    struct pingpong_context* ctx = handle->ctx;
    int rcnt, scnt, num_cq_events, use_event = 0;
	rcnt = scnt = 0;
	while (rcnt + scnt < iters) {
		struct ibv_wc wc[2];
		int ne, i;

		do {
			ne = ibv_poll_cq(pp_cq(ctx), 2, wc);
			if (ne < 0) {
				fprintf(stderr, "poll CQ failed %d\n", ne);
				return 1;
			}

		} while (ne < 1);

		for (i = 0; i < ne; ++i) {
			if (wc[i].status != IBV_WC_SUCCESS) {
				fprintf(stderr, "Failed status %s (%d) for wr_id %d\n",
					ibv_wc_status_str(wc[i].status),
					wc[i].status, (int) wc[i].wr_id);
				return 1;
			}
            struct packet* gotten_packet;
			switch ((int) wc[i].wr_id) {
			case PINGPONG_SEND_WRID:
                printf("msg sent successful\n");
                scnt = scnt + 1;
				break;

			case PINGPONG_RECV_WRID:
				//handle_server_packets_only(handle, (struct packet*)&ctx->buf);
				gotten_packet = (struct packet*)ctx->buf;
                if(gotten_packet->type == EAGER_GET_RESPONSE)
                {
                    *answerBuffer = malloc(gotten_packet->eager_get_response.valueLen * sizeof(char));
                    memcpy(*answerBuffer,gotten_packet->eager_get_response.value,gotten_packet->eager_get_response.valueLen);
                    printf("Answer buffer:\n %s\n",*answerBuffer);
                }
                if(gotten_packet->type = EAGER_SET_RESPONSE)
                {
                    printf("set is done on server, continuing\n");
                }
                pp_post_recv(ctx, 1);
                rcnt = rcnt + 1;
				break;

			default:
				fprintf(stderr, "Completion for unknown wr_id %d\n",
					(int) wc[i].wr_id);
				return 1;
			}
		}
	}
	return 0;
}


int kv_open(struct kv_server_address *server, struct kv_handle *kv_handle)
{
    return 0;//orig_main(server, EAGER_PROTOCOL_LIMIT, g_argc, g_argv, &kv_handle->ctx);
}

int kv_set(struct kv_handle *kv_handle, const char *key, const char *value)
{
    struct pingpong_context *ctx = kv_handle->ctx;
    struct packet *set_packet = (struct packet*)ctx->buf;

    unsigned packet_size = strlen(key) + strlen(value) +2 + sizeof(struct packet);
    if (packet_size < (EAGER_PROTOCOL_LIMIT)) {
        /* Eager protocol - exercise part 1 */
        set_packet->type = EAGER_SET_REQUEST;
        printf("sending eager.\n key = %s\n value = %s\n",key,value);
        set_packet->eager_set_request.keyLen = strlen(key) + 1;
        set_packet->eager_set_request.valueLen = strlen(value) + 1;
        memcpy(set_packet->eager_set_request.key_and_value,key,strlen(key) + 1);
        memcpy(&(set_packet->eager_set_request.key_and_value[strlen(key) + 1]),value,strlen(value) + 1);
        /* TODO (4LOC): fill in the rest of the set_packet */
        printf("send %s\n",set_packet->eager_set_request.key_and_value);
        printf("packet size is %d.\nchar after packet size = %c\nlast char in msg is = %c\n",packet_size,set_packet->eager_set_request.key_and_value[packet_size-sizeof(struct packet)],set_packet->eager_set_request.key_and_value[packet_size-1-sizeof(struct packet)]);
        printf("packet type is %d\n",set_packet->type);
        pp_post_send(ctx, IBV_WR_SEND, packet_size, NULL, NULL, 0); /* Sends the packet to the server */
        printf("packet sent\n");
        return pp_wait_completions(kv_handle, 2,NULL); /* await EAGER_SET_REQUEST completion and EAGER_SET_RESPONSE */
    }

    /* Otherwise, use RENDEZVOUS - exercise part 2 */
    //Flow: send a request to send this big data, recv the rkey to the registered 
    //memory then use RDMA_WRITE
    set_packet->type = RENDEZVOUS_SET_REQUEST;
    printf("randevo\n");
    /* TODO (4LOC): fill in the rest of the set_packet - request peer address & remote key */

    pp_post_recv(ctx, 1); /* Posts a receive-buffer for RENDEZVOUS_SET_RESPONSE */
    pp_post_send(ctx, IBV_WR_SEND, packet_size, NULL, NULL, 0); /* Sends the packet to the server */
    assert(pp_wait_completions(kv_handle, 2,NULL)); /* wait for both to complete */

    assert(set_packet->type == RENDEZVOUS_SET_RESPONSE);
    pp_post_send(ctx, IBV_WR_RDMA_WRITE, packet_size, value, NULL, 0/* TODO (1LOC): replace with remote info for RDMA_WRITE from packet */);
    return pp_wait_completions(kv_handle, 1,NULL); /* wait for both to complete */
}

int kv_get(struct kv_handle *kv_handle, const char *key, char **value)
{
    struct pingpong_context *ctx = kv_handle->ctx;
    struct packet *set_packet = (struct packet*)ctx->buf;

    unsigned packet_size = strlen(key) + sizeof(struct packet);
    if (packet_size < (EAGER_PROTOCOL_LIMIT)) {
        /* Eager protocol - exercise part 1 */
        printf("type is %d\n",EAGER_GET_REQUEST);
        set_packet->type = EAGER_GET_REQUEST;
        printf("sending eager get.\n key = %s\n",key);
        set_packet->eager_get_request.keyLen = strlen(key) + 1;
        
        memcpy(set_packet->eager_get_request.key,key,strlen(key) + 1);
 
        /* TODO (4LOC): fill in the rest of the get_packet */
        printf("send %s\n",set_packet->eager_get_request.key);
        printf("packet size is %d.\nchar after packet size = %c\nlast char in msg is = %c\n",packet_size,set_packet->eager_set_request.key_and_value[packet_size-sizeof(struct packet)],set_packet->eager_set_request.key_and_value[packet_size-1-sizeof(struct packet)]);
        printf("packet type is %d\n",set_packet->type);
        pp_post_send(ctx, IBV_WR_SEND, packet_size, NULL, NULL, 0); /* Sends the packet to the server */
        printf("packet sent\n");
        return pp_wait_completions(kv_handle, 2,value); /* await EAGER_GET_REQUEST completion, and EAGER_GET_RESPONSE answer */
    }

    /* Otherwise, use RENDEZVOUS - exercise part 2 */
    set_packet->type = RENDEZVOUS_SET_REQUEST;
    printf("randevo\n");
    /* TODO (4LOC): fill in the rest of the set_packet - request peer address & remote key */

    pp_post_recv(ctx, 1); /* Posts a receive-buffer for RENDEZVOUS_SET_RESPONSE */
    pp_post_send(ctx, IBV_WR_SEND, packet_size, NULL, NULL, 0); /* Sends the packet to the server */
    assert(pp_wait_completions(kv_handle, 2,value)); /* wait for both to complete */

    assert(set_packet->type == RENDEZVOUS_SET_RESPONSE);
    pp_post_send(ctx, IBV_WR_RDMA_WRITE, packet_size, NULL, NULL, 0);/* TODO (1LOC): replace with remote info for RDMA_WRITE from packet */
    return pp_wait_completions(kv_handle, 1,value); /* wait for both to complete */
}

void kv_release(char *value)
{
    /* TODO (2LOC): free value */
    free(value);
}

int kv_close(struct kv_handle *kv_handle)
{
    return pp_close_ctx(kv_handle->ctx);
}

#ifdef EX3
#define my_open  kv_open
#define set      kv_set
#define get      kv_get
#define release  kv_release
#define my_close kv_close
#endif /* EX3 */





//////////////////////



int main(int argc, char *argv[])
{
    
    struct ibv_device      **dev_list;
	struct ibv_device	*ib_dev;
	struct pingpong_context *context;
	struct pingpong_dest    my_dest[NUM_SOCKETS];
	struct pingpong_dest    *rem_dest;
	struct timeval           timer;
	char                    *ib_devname = NULL;
	//char                    *servername = NULL;
	unsigned int             port = 18515;
	int                      ib_port = 1;
	int             messageSize[NUM_SOCKETS];
    int             numMessages[NUM_SOCKETS];
    page_size = sysconf(_SC_PAGESIZE); //checks the page size used by the system
    for (int i=0 ;i< NUM_SOCKETS; i = i+1)
    {
        messageSize[i] = page_size; //start with page_size messages
        numMessages[i] = 100000; //start with 10000 iters per size of message
    }
	enum ibv_mtu		 mtu = IBV_MTU_1024;
	unsigned int             rx_depth = 5000;
	
	int                      use_event = 0;
	int                      routs[NUM_SOCKETS];
	//int                      rcnt[NUM_SOCKETS];
    int                      sendCount[NUM_SOCKETS];
	int                      num_cq_events;//only one completion queue
	int                      sl = 0;
	int			 gidx = -1;
	char			 gid[33];
	//struct ts_params	 ts;

	srand48(getpid() * time(NULL));

    //////////////
    
    //////////////
    //get input for the server ip and port
    //int portNum;
    int numArgs = 3;
    char* usageMessage = "usage %s Server IP port\n";
	if (argc < numArgs) {
       fprintf(stderr,usageMessage, argv[0]);
       exit(0);
    }
    portNum = atoi(argv[numArgs - 1]);
    port = portNum;
    servername = strdupa(argv[1]);
    
    //get our beloved device
    dev_list = ibv_get_device_list(NULL); //get devices available to this machine
	if (!dev_list) {
		perror("Failed to get IB devices list");
		return 1;
	}
    ib_dev = *dev_list; //chooses the first device by default
    if (!ib_dev) {
        fprintf(stderr, "No IB devices found\n");
        return 1;
    }
    //Create the context for this connection
    //creates context on found device, registers memory of size.
    context = pp_init_ctx(ib_dev, EAGER_PROTOCOL_LIMIT, rx_depth, ib_port, use_event); //use_event (decides if we wait blocking for completion)
    if (!context)
        return 1;
    
    if (pp_get_port_info(context->context, ib_port, &context->portinfo)) { //gets the port status and info (uses ibv_query_port)
            fprintf(stderr, "Couldn't get port info\n");
            return 1;
        }
    for ( int k = 0; k < NUM_SOCKETS ; k = k+1)
    {
        //Prepare to recieve messages. fill the recieve request queue of QP k
        routs[k] = pp_post_recv(context, context->rx_depth); //post rx_depth recieve requests
            if (routs[k] < context->rx_depth) {
                fprintf(stderr, "Couldn't post receive (%d)\n", routs[k]);
                return 1;
            }
        //set my_dest for every QP, getting ready to connect them.
        my_dest[k].lid = context->portinfo.lid; //assigns lid to my dest
        if (context->portinfo.link_layer != IBV_LINK_LAYER_ETHERNET &&
                                !my_dest[k].lid) {
            fprintf(stderr, "Couldn't get local LID\n");
            return 1;
        }
        //set the gid to 0, we are in the same subnet.
        memset(&my_dest[k].gid, 0, sizeof my_dest[k].gid); //zero the gid, we send in the same subnet
        my_dest[k].qpn = ((*context).qp[k])->qp_num; //gets the qp number
        my_dest[k].psn = lrand48() & 0xffffff; //randomizes the packet serial number
        inet_ntop(AF_INET6, &my_dest[k].gid, gid, sizeof gid); //changes gid to text form
        //printf("  local address:  LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
         //      my_dest[k].lid, my_dest[k].qpn, my_dest[k].psn, gid);
    }
    //Get the remote dest for my QPs
    rem_dest = pp_client_exch_dest(servername, port, my_dest); //if youre a client - exchange data with server
    if (!rem_dest)
            return 1; 
    

    for(int k = 0 ; k < NUM_SOCKETS; k = k + 1)
    {
      inet_ntop(AF_INET6, &rem_dest[k].gid, gid, sizeof gid);
      //printf("  remote address: LID 0x%04x, QPN 0x%06x, PSN 0x%06x, GID %s\n",
      //       rem_dest[k].lid, rem_dest[k].qpn, rem_dest[k].psn, gid);
    }      
    //now connect all the QPs to the server
    for( int k = 0 ; k < NUM_SOCKETS; k = k+1)
    {
        if (pp_connect_ctx(context, ib_port, my_dest[k].psn, mtu, sl, &rem_dest[k],
                        gidx,k))
                return 1; //connect to the server

    }
   
    //Do client work
    struct kv_handle * handle = malloc(sizeof(struct kv_handle));
    handle->ctx = context;
    char send_buffer[MAX_TEST_SIZE] = {0};
    char *recv_buffer;
    /* Test small size */
    assert(100 < MAX_TEST_SIZE);
    memset(send_buffer, 'a', 100);
    assert(0 == set(handle, "1", send_buffer));
    printf("set success\n");
    //sleep(1);
    assert(0 == get(handle, "1", &recv_buffer));
    printf("recv buffer: %s\n",recv_buffer);
    assert(0 == strcmp(send_buffer, recv_buffer));
    release(recv_buffer);

    /* Test logic */
    assert(0 == get(handle, "1", &recv_buffer));
    assert(0 == strcmp(send_buffer, recv_buffer));
    release(recv_buffer);
    memset(send_buffer, 'b', 100);
    //sleep(1);
    assert(0 == set(handle, "1", send_buffer));
    memset(send_buffer, 'c', 100);
    //sleep(1);
    assert(0 == set(handle, "22", send_buffer));
    memset(send_buffer, 'b', 100);
    //sleep(1);
    assert(0 == get(handle, "1", &recv_buffer));
    assert(0 == strcmp(send_buffer, recv_buffer));
    release(recv_buffer);

    /* Test large size */
    /*memset(send_buffer, 'a', MAX_TEST_SIZE - 1);
    assert(0 == set(handle, "1", send_buffer));
    assert(0 == set(handle, "333", send_buffer));
    assert(0 == get(handle, "1", &recv_buffer));
    assert(0 == strcmp(send_buffer, recv_buffer));
    release(recv_buffer);*/
    
    ///////////////////////////
    printf("client success@#!@@\n");
    //sleep(10);
    ibv_free_device_list(dev_list);
    free(rem_dest);
    //my_close(handle);
    //free(handle);
    return 0;
}
    
