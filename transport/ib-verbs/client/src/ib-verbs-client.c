/*
  (C) 2006,2007 Z RESEARCH Inc. <http://www.zresearch.com>
  
  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of
  the License, or (at your option) any later version.
    
  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
  GNU General Public License for more details.
    
  You should have received a copy of the GNU General Public
  License along with this program; if not, write to the Free
  Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
  Boston, MA 02110-1301 USA
*/ 

#include "dict.h"
#include "glusterfs.h"
#include "transport.h"
#include "logging.h"
#include "xlator.h"
#include "protocol.h"

#include "ib-verbs.h"


static int32_t 
ib_verbs_client_disconnect (transport_t *this)
{
  ib_verbs_private_t *priv = this->private;

  /* Free everything allocated, registered */
  /* dereg_mr */
  ib_mr_struct_t *temp, *trav = priv->ibv.qp[0].recv_wr_list;
  while (trav) {
    temp = trav;
    if (trav->buf) free (trav->buf);
    if (trav->mr) ibv_dereg_mr (trav->mr);
    trav = trav->next;
    free (temp);
  }
  trav = priv->ibv.qp[1].recv_wr_list;
  while (trav) {
    temp = trav;
    if (trav->buf) free (trav->buf);
    if (trav->mr) ibv_dereg_mr (trav->mr);
    trav = trav->next;
    free (temp);
  }
  trav = priv->ibv.qp[0].send_wr_list;
  while (trav) {
    temp = trav;
    if (trav->buf) free (trav->buf);
    if (trav->mr) ibv_dereg_mr (trav->mr);
    trav = trav->next;
    free (temp);
  }
  trav = priv->ibv.qp[1].send_wr_list;
  while (trav) {
    temp = trav;
    if (trav->buf) free (trav->buf);
    if (trav->mr) ibv_dereg_mr (trav->mr);
    trav = trav->next;
    free (temp);
  }
  /* destroy_qp */
  if (priv->ibv.qp[0].qp) ibv_destroy_qp (priv->ibv.qp[0].qp);
  if (priv->ibv.qp[1].qp) ibv_destroy_qp (priv->ibv.qp[1].qp);

  /* ibv_destroy_cq (priv->ibv.sendcq[0]);
     ibv_destroy_cq (priv->ibv.sendcq[1]);
     ibv_destroy_cq (priv->ibv.recvcq[0]);
     ibv_destroy_cq (priv->ibv.recvcq[1]);
     
     
     ibv_destroy_comp_channel (priv->ibv.send_channel[0]);
     ibv_destroy_comp_channel (priv->ibv.send_channel[1]);
     ibv_destroy_comp_channel (priv->ibv.recv_channel[0]);
     ibv_destroy_comp_channel (priv->ibv.recv_channel[1]);
       
     ibv_dealloc_pd (priv->ibv.pd);
     ibv_close_device (priv->ibv.context);
  */
  priv->connected = 0;
  return 0;
}

static int32_t 
ib_verbs_client_notify (xlator_t *xl, 
			transport_t *trans, 
			int32_t event) 
{
  ib_verbs_private_t *priv = trans->private;
  priv->connected = 0;
  poll_unregister (xl->ctx, priv->sock);
  return 0;
}


static int32_t  
do_handshake (transport_t *this, dict_t *options)
{
  GF_ERROR_IF_NULL (this);
  ib_verbs_private_t *priv = this->private;
  GF_ERROR_IF_NULL (priv);
  
  dict_t *request = get_new_dict ();
  dict_t *reply = get_new_dict ();
  int32_t ret;
  int32_t remote_errno;
  char *remote_subvolume = NULL;
  char *remote_error;

  if (priv->is_debug) {
    FUNCTION_CALLED;
  }
  
  remote_subvolume = data_to_str (dict_get (options,
					    "remote-subvolume"));
  dict_set (request, 
	    "remote-subvolume",
	    data_from_dynstr (strdup (remote_subvolume)));
  
  {
    int32_t dict_len = dict_serialized_length (request);
    char *dict_buf = malloc (dict_len);
    dict_serialize (request, dict_buf);

    gf_block *blk = gf_block_new (424242); /* "random" number */
    blk->type = GF_OP_TYPE_MOP_REQUEST;
    blk->op = GF_MOP_SETVOLUME;
    blk->size = dict_len;
    blk->data = dict_buf;

    int32_t blk_len = gf_block_serialized_length (blk);
    char *blk_buf = malloc (blk_len);
    gf_block_serialize (blk, blk_buf);

    ib_mr_struct_t *mr = priv->ibv.qp[0].send_wr_list;
    priv->ibv.qp[0].send_wr_list = mr->next;
    memcpy (mr->buf, blk_buf, blk_len);
    
    {
      ret = ib_verbs_post_send (this, &priv->ibv.qp[0], mr, blk_len); 
      struct ibv_wc wc;
      struct ibv_cq *ev_cq;
      void *ev_ctx;
      ibv_get_cq_event (priv->ibv.send_channel[0], &ev_cq, &ev_ctx);
      ibv_poll_cq (priv->ibv.sendcq[0], 1, &wc);
      ibv_req_notify_cq (priv->ibv.sendcq[0], 0);
      
      ib_cq_comp_t *ib_cq_comp = (ib_cq_comp_t *)(long)wc.wr_id;
      ib_qp_struct_t *qp = ib_cq_comp->qp;
      ib_mr_struct_t *mr = ib_cq_comp->mr;
      
      if (ib_cq_comp->type == 0) {
	mr->next = qp->send_wr_list;
	qp->send_wr_list = mr;
      } else {
	/* error :O */
	gf_log ("ib-verbs/client", 
		GF_LOG_ERROR, 
		"Error in sending handshake message");
	return -1;
      }
    }

    free (blk_buf);
    free (dict_buf);
    free (blk);
  }

  if (ret == -1) { 
    struct sockaddr_in sin;
    sin.sin_addr.s_addr = priv->addr;
    
    gf_log ("transport: ib-verbs: ",
	    GF_LOG_ERROR,
	    "handshake with %s failed", 
	    inet_ntoa (sin.sin_addr));
    goto ret;
  }

  gf_block *reply_blk = gf_block_unserialize_transport (this);
  if (!reply_blk) {
    gf_log ("transport: ib-verbs: ",
	    GF_LOG_ERROR,
	    "gf_block_unserialize failed during handshake");
    ret = -1;
    goto reply_err;
  }

  if (reply_blk->data)
    dict_unserialize (reply_blk->data, reply_blk->size, &reply);
  else
    goto reply_err;

  if (reply == NULL) {
    gf_log ("transport: ib-verbs: ",
	    GF_LOG_ERROR,
	    "dict_unserialize failed");
    ret = -1;
    goto reply_err;
  }

  ret = data_to_int (dict_get (reply, "RET"));
  remote_errno = data_to_int (dict_get (reply, "ERRNO"));
  remote_error = data_to_str (dict_get (reply, "ERROR")); /* note that its not 'errno' */
  
  if (ret < 0) {
    gf_log ("ib-verbs/client",
	    GF_LOG_ERROR,
	    "SETVOLUME on remote server failed (%s)",
	    remote_error);
    errno = remote_errno;
    goto reply_err;
  }

 reply_err:
  if (reply_blk) {
    if (reply_blk->data)
      free (reply_blk->data);
    free (reply_blk);
  }
		   
 ret:
  dict_destroy (request);
  dict_destroy (reply);
  return ret;
}

static int32_t
ib_verbs_connect (struct transport *this, 
		  dict_t *options)
{
  GF_ERROR_IF_NULL (this);
  
  ib_verbs_private_t *priv = this->private;
  GF_ERROR_IF_NULL (priv);
  
  if (!priv->options)
    priv->options = dict_copy (options, NULL);

  struct sockaddr_in sin;
  struct sockaddr_in sin_src;
  int32_t ret = 0;
  uint16_t try_port = CLIENT_PORT_CIELING;

  if (!priv->connected)
    priv->sock = socket (AF_INET, SOCK_STREAM, 0);

  gf_log ("transport: ib-verbs: ",
	  GF_LOG_DEBUG,
	  "try_connect: socket fd = %d", priv->sock);

  if (priv->sock == -1) {
    gf_log ("transport: ib-verbs: ",
	    GF_LOG_ERROR,
	    "try_connect: socket () - error: %s",
	    strerror (errno));
    return -errno;
  }

  while (try_port) { 
    sin_src.sin_family = AF_INET;
    sin_src.sin_port = htons (try_port); //FIXME: have it a #define or configurable
    sin_src.sin_addr.s_addr = INADDR_ANY;
    
    if ((ret = bind (priv->sock,
		     (struct sockaddr *)&sin_src,
		     sizeof (sin_src))) == 0) {
      gf_log ("transport: ib-verbs: ",
	      GF_LOG_DEBUG,
	      "try_connect: finalized on port `%d'",
	      try_port);
      break;
    }
    
    try_port--;
  }
  
  if (ret != 0) {
      gf_log ("transport: ib-verbs: ",
	      GF_LOG_ERROR,
	      "try_connect: bind loop failed - error: %s",
	      strerror (errno));
      close (priv->sock);
      return -errno;
  }

  sin.sin_family = AF_INET;

  if (dict_get (options, "remote-port")) {
    sin.sin_port = htons (data_to_int (dict_get (options,
						 "remote-port")));
  } else {
    gf_log ("ib-verbs/client",
	    GF_LOG_DEBUG,
	    "try_connect: defaulting remote-port to %d", 
	    GF_DEFAULT_LISTEN_PORT);
    sin.sin_port = htons (GF_DEFAULT_LISTEN_PORT);
  }

  if (dict_get (options, "remote-host")) {
    sin.sin_addr.s_addr = gf_resolve_ip (data_to_str (dict_get (options, 
								"remote-host")));
  } else {
    gf_log ("ib-verbs/client",
	    GF_LOG_DEBUG,
	    "try_connect: error: missing 'option remote-host <hostname>'");
    close (priv->sock);
    return -errno;
  }

  if (connect (priv->sock, (struct sockaddr *)&sin, sizeof (sin)) != 0) {
    gf_log ("transport/ib-verbs",
	    GF_LOG_ERROR,
	    "try_connect: connect () - error: %s",
 	    strerror (errno));
    close (priv->sock);
    return -errno;
  }

  /* Get the ibv options from xl->options */
  priv->ibv.qp[0].send_wr_count = 64;
  priv->ibv.qp[0].recv_wr_count = 64;
  priv->ibv.qp[0].send_wr_size = 131072; //128kB
  priv->ibv.qp[0].recv_wr_size = 131072;
  priv->ibv.qp[1].send_wr_count = 2;
  priv->ibv.qp[1].recv_wr_count = 2;

  data_t *temp =NULL;
  temp = dict_get (this->xl->options, "ibv-send-wr-count");
  if (temp)
    priv->ibv.qp[0].send_wr_count = data_to_int (temp);

  temp = dict_get (this->xl->options, "ibv-recv-wr-count");
  if (temp)
    priv->ibv.qp[0].recv_wr_count = data_to_int (temp);

  temp = dict_get (this->xl->options, "ibv-send-wr-size");
  if (temp)
    priv->ibv.qp[0].send_wr_size = data_to_int (temp);
  temp = dict_get (this->xl->options, "ibv-recv-wr-size");
  if (temp)
    priv->ibv.qp[0].recv_wr_size = data_to_int (temp);
  
  if (ib_verbs_create_qp (priv) < 0) {
    gf_log ("ib-verbs/client", GF_LOG_ERROR, "Couldn't create QP");
    return -1;
  }
  
  char buf[256] = {0,};
  int32_t recv_buf_size[2], send_buf_size[2];

  sprintf (buf, "QP1:LID=%04x:QPN=%06x:PSN=%06x:RECV_BLKSIZE=%08x:SEND_BLKSIZE=%08x\n"
	   "QP2:LID=%04x:QPN=%06x:PSN=%06x:RECV_BLKSIZE=%08x:SEND_BLKSIZE=%08x\n",
	   priv->ibv.qp[0].local_lid,
	   priv->ibv.qp[0].local_qpn,
	   priv->ibv.qp[0].local_psn,
	   priv->ibv.qp[0].recv_wr_size,
	   priv->ibv.qp[0].send_wr_size,
	   priv->ibv.qp[1].local_lid,
	   priv->ibv.qp[1].local_qpn,
	   priv->ibv.qp[1].local_psn,
	   priv->ibv.qp[1].recv_wr_size,
	   priv->ibv.qp[1].send_wr_size);

  write (priv->sock, buf, sizeof buf);

  buf[0] = '\0';
  read (priv->sock, buf, sizeof buf);
  if (strncmp (buf, "QP1:LID", 7)) {
    gf_log ("ib-verbs-client",
	    GF_LOG_CRITICAL,
	    "Server's transport type is different");
    return -1;
  }
  sscanf (buf, "QP1:LID=%04x:QPN=%06x:PSN=%06x:RECV_BLKSIZE=%08x:SEND_BLKSIZE=%08x\n"
	  "QP2:LID=%04x:QPN=%06x:PSN=%06x:RECV_BLKSIZE=%08x:SEND_BLKSIZE=%08x\n",
	  &priv->ibv.qp[0].remote_lid,
	  &priv->ibv.qp[0].remote_qpn,
	  &priv->ibv.qp[0].remote_psn,
 	  &send_buf_size[0],
	  &recv_buf_size[0],
	  &priv->ibv.qp[1].remote_lid,
	  &priv->ibv.qp[1].remote_qpn,
	  &priv->ibv.qp[1].remote_psn,
	  &send_buf_size[1],
	  &recv_buf_size[1]);

  if (recv_buf_size[0] < priv->ibv.qp[0].recv_wr_size)
    priv->ibv.qp[0].recv_wr_size = recv_buf_size[0];
  if (recv_buf_size[1] < priv->ibv.qp[1].recv_wr_size)
    priv->ibv.qp[1].recv_wr_size = recv_buf_size[1];
  if (send_buf_size[0] < priv->ibv.qp[0].send_wr_size)
    priv->ibv.qp[0].send_wr_size = send_buf_size[0];
  if (send_buf_size[1] < priv->ibv.qp[1].send_wr_size)
    priv->ibv.qp[1].send_wr_size = send_buf_size[1];
  
  // allocate buffers and MRs
  if (ib_verbs_create_buf_list (&priv->ibv) < 0) {
    gf_log ("ib-verbs/client", 
	    GF_LOG_ERROR, 
	    "Couldn't allocate buffer for QPs");
    return -1;
  }

  /* Keep the read requests in the queue */
  int32_t i;
  ib_mr_struct_t *mr;
  for (i = 0; i < priv->ibv.qp[0].recv_wr_count; i++) {
    mr = priv->ibv.qp[0].recv_wr_list;
    priv->ibv.qp[0].recv_wr_list = mr->next;
    ib_verbs_post_recv (this, &priv->ibv.qp[0], mr);
  }
  
  if (ib_verbs_ibv_connect (priv, 1, IBV_MTU_1024)) {
    gf_log ("ib-verbs/client", 
	    GF_LOG_ERROR, 
	    "Failed to connect with remote QP");
    return -1;
  }
  
  ret = do_handshake (this, options);

  if (ret != 0) {
    gf_log ("transport: ib-verbs: ", 
	    GF_LOG_ERROR, "handshake failed");
    close (priv->sock);
    return ret;
  }

  this->notify = ib_verbs_client_notify; //for server disconnect
  poll_register (this->xl->ctx, priv->sock, this);

  priv->connected = 1;

  return ret;
}

/* TODO : cleanup (no locking required as its called only once */
static int32_t
ib_verbs_client_submit (transport_t *this, char *buf, int32_t len)
{
  ib_verbs_private_t *priv = this->private;

  /* See if the buffer (memory region) is free, then send it */
  int32_t qp_idx = 0;
  if (len <= priv->ibv.qp[0].send_wr_size + 2048) {
    /* General queue is enough for this data */
    qp_idx = IBVERBS_CMD_QP;
  } else {
    /* Go for the next queue, which can be of variable size */
    qp_idx = IBVERBS_MISC_QP;
    
    if (!priv->ibv.qp[1].send_wr_list)
      priv->ibv.qp[1].send_wr_list = calloc (1, sizeof (ib_mr_struct_t));

    if (priv->ibv.qp[1].send_wr_list->buf_size < len) {
      /* Already allocated data buffer is not enough, allocate bigger chunk */
      if (priv->ibv.qp[1].send_wr_list->buf)
	free (priv->ibv.qp[1].send_wr_list->buf);
      priv->ibv.qp[1].send_wr_list->buf = valloc (len + 2048);
      priv->ibv.qp[1].send_wr_list->buf_size = len + 2048;
      memset (priv->ibv.qp[1].send_wr_list->buf, 0, len + 2048);
      priv->ibv.qp[1].send_wr_list->mr = ibv_reg_mr(priv->ibv.pd, 
						    priv->ibv.qp[1].send_wr_list->buf, 
						    len + 2048,
						    IBV_ACCESS_LOCAL_WRITE);
      if (!priv->ibv.qp[1].send_wr_list->mr) {
	gf_log ("transport/ib-verbs", 
		GF_LOG_CRITICAL, 
		"Couldn't allocate MR[0]\n");
	return -1;
      }
    }
    ib_mr_struct_t *temp_mr = priv->ibv.qp[0].send_wr_list;
    priv->ibv.qp[0].send_wr_list = temp_mr->next;

    sprintf (temp_mr->buf, "NeedDataMR:%d\n", len + 4);
    if (ib_verbs_post_send (this, &priv->ibv.qp[0], temp_mr, 20)) {
      gf_log ("ib-verbs/client", GF_LOG_ERROR, "Failed submit");
      return -1;
    }
  } 
  
  ib_mr_struct_t *mr = priv->ibv.qp[qp_idx].send_wr_list;
  memcpy (mr->buf, buf, len);
  priv->ibv.qp[qp_idx].send_wr_list = mr->next;

  if (!priv->connected) {
    int ret = ib_verbs_connect (this, priv->options);
    if (ret == 0) {
      return ib_verbs_post_send (this, &priv->ibv.qp[qp_idx], mr, len);
    }
    else
      return -1;
  }
  
  if (ib_verbs_post_send (this, &priv->ibv.qp[qp_idx], mr, len)) {
    gf_log ("ib-verbs/client", GF_LOG_ERROR, "Failed to submit msg");
    return -1;
  }
  return len;
}

static int32_t
ib_verbs_client_except (transport_t *this)
{
  /* TODO : Check whether this is enough */
  /* Need to free few of the pointers already allocated */
  GF_ERROR_IF_NULL (this);

  ib_verbs_private_t *priv = this->private;
  GF_ERROR_IF_NULL (priv);
  
  gf_log ("ib-verbs/client", GF_LOG_ERROR, "except");

  priv->connected = 0;
  int32_t ret = ib_verbs_connect (this, priv->options);

  return ret;
}

static int32_t 
ib_verbs_client_writev (struct transport *this,
			const struct iovec *vector,
			int32_t count)
{
  int32_t i, len = 0;
  ib_verbs_private_t *priv = this->private;
  const struct iovec *trav = vector;

  if (!priv->connected) {
    int32_t ret = ib_verbs_connect (this, priv->options);
    if (ret)
      return -ENOTCONN;
  }
  for (i = 0; i< count; i++) {
    len += trav[i].iov_len;
  }

  /* See if the buffer (memory region) is free, then send it */
  int32_t qp_idx = 0;
  ib_mr_struct_t *mr;
  if (len <= priv->ibv.qp[0].send_wr_size + 2048) {
    qp_idx = IBVERBS_CMD_QP;
    while (1) {
      pthread_mutex_lock (&priv->write_mutex);
      mr = priv->ibv.qp[0].send_wr_list;
      if (mr)
	priv->ibv.qp[0].send_wr_list = mr->next;
      pthread_mutex_unlock (&priv->write_mutex);
      if (!mr) {
	ib_verbs_send_cq_notify (this->xl, this, POLLIN);
      } else {
	break;
      }
    }
  } else {
    qp_idx = IBVERBS_MISC_QP;
    while (1) {
      pthread_mutex_lock (&priv->write_mutex);
      mr = priv->ibv.qp[1].send_wr_list;
      if (mr)
	priv->ibv.qp[1].send_wr_list = mr->next;
      pthread_mutex_unlock (&priv->write_mutex);
      if (!mr) {
	ib_verbs_send_cq_notify1 (this->xl, this, POLLIN);
      } else {
	break;
      }
    }

    if (mr->buf_size < len) {
      /* Already allocated data buffer is not enough, allocate bigger chunk */
      if (mr->buf) {
	free (mr->buf);
	ibv_dereg_mr (mr->mr);
      }

      mr->buf = valloc (len + 2048);
      mr->buf_size = len + 2048;
      memset (mr->buf, 0, len + 2048);

      mr->mr = ibv_reg_mr(priv->ibv.pd, 
			  mr->buf, 
			  len + 2048,
			  IBV_ACCESS_LOCAL_WRITE);
      if (!mr->mr) {
	gf_log ("transport/ib-verbs", 
		GF_LOG_CRITICAL, 
		"Couldn't allocate MR\n");
	return -1;
      }
    }

    pthread_mutex_lock (&priv->write_mutex);
    ib_mr_struct_t *temp_mr = priv->ibv.qp[0].send_wr_list;
    priv->ibv.qp[0].send_wr_list = temp_mr->next;
    pthread_mutex_unlock (&priv->write_mutex);

    sprintf (temp_mr->buf, 
	     "NeedDataMR:%d\n", len + 4);
    if (ib_verbs_post_send (this, &priv->ibv.qp[0], temp_mr, 20) < 0) {
      gf_log ("ib-verbs-writev", 
	      GF_LOG_CRITICAL, 
	      "Failed to send meta buffer");
      return -EINTR;
    }
  }  
  
  len = 0;
  for (i = 0; i< count; i++) {
    memcpy (mr->buf + len, trav[i].iov_base, trav[i].iov_len);
    len += trav[i].iov_len;
  }

  if (ib_verbs_post_send (this, &priv->ibv.qp[qp_idx], mr, len) < 0) {
    gf_log ("ib-verbs-writev", 
	    GF_LOG_CRITICAL, 
	    "Failed to send buffer");
    return -EINTR;
  }

  return 0;
}

struct transport_ops transport_ops = {
  .recieve = ib_verbs_recieve,
  .submit = ib_verbs_client_submit,
  .readv = ib_verbs_readv,
  .writev = ib_verbs_client_writev,

  .disconnect = ib_verbs_client_disconnect,
  .except = ib_verbs_client_except,
};

int32_t 
gf_transport_init (struct transport *this,
		   dict_t *options,
		   int32_t (*notify) (xlator_t *xl, transport_t *trans, int32_t event))
{
  transport_t *recv_trans = calloc (1, sizeof (transport_t));
  transport_t *send_trans = calloc (1, sizeof (transport_t));
  ib_verbs_private_t *priv = calloc (1, sizeof (ib_verbs_private_t));
  this->private = priv;
  priv->notify = notify;

  /* Initialize the driver specific parameters */
  if (ib_verbs_ibv_init (&priv->ibv)) {
    gf_log ("ib-verbs/client", 
	    GF_LOG_ERROR, 
	    "Failed to initialize IB Device");
    return -1;
  }

  pthread_mutex_init (&priv->read_mutex, NULL);
  pthread_mutex_init (&priv->write_mutex, NULL);

  memcpy (recv_trans, this, sizeof (transport_t));
  recv_trans->notify = ib_verbs_recv_cq_notify;
  recv_trans->xl = this->xl;

  memcpy (send_trans, this, sizeof (transport_t));
  send_trans->notify = ib_verbs_send_cq_notify;
  send_trans->xl = this->xl;

  /* Register Channel fd for getting event notification on CQ */
  poll_register (send_trans->xl,
		 priv->ibv.send_channel[0]->fd,
		 send_trans);
  poll_register (recv_trans->xl,
		 priv->ibv.recv_channel[0]->fd,
		 recv_trans);
  priv->registered = 1;

  int ret = ib_verbs_connect (this, options);
  if (ret != 0) {
    gf_log ("transport: ib-verbs: client: ", 
	    GF_LOG_ERROR, "init failed");
    //    return -1;
  }
  
  return 0;
}

void  
gf_transport_fini (struct transport *this)
{
  /* TODO: proper cleaning */
  ib_verbs_private_t *priv = this->private;

  /* This cleans up all the ib-verbs related pointers */
  ib_verbs_client_disconnect (this);

  dict_destroy (priv->options);
  close (priv->sock);
  free (priv);
  return ;
}
