#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <rdma/rdma_cma.h>

#define TEST_NZ(x) do { if ( (x)) die("error: " #x " failed (returned non-zero)." ); } while (0)
#define TEST_Z(x)  do { if (!(x)) die("error: " #x " failed (returned zero/null)."); } while (0)

const int BUFFER_SIZE = 1024;

struct context {
  struct ibv_context *ctx;
  struct ibv_pd *pd;
  struct ibv_cq *cq;
  struct ibv_comp_channel *comp_channel;

  pthread_t cq_poller_thread;
};

struct connection {
  struct ibv_qp *qp;

  struct ibv_mr *recv_mr;
  struct ibv_mr *send_mr;

  char *recv_region;
  char *send_region;
};

static void die(const char *reason);

static void build_context(struct ibv_context *verbs);
static void build_qp_attr(struct ibv_qp_init_attr *qp_attr);
static void * poll_cq(void *);
static void post_receives(struct connection *conn);
static void register_memory(struct connection *conn);

static void on_completion(struct ibv_wc *wc);
static int on_connect_request(struct rdma_cm_id *id);
static int on_connection(void *context);
static int on_disconnect(struct rdma_cm_id *id);
static int on_event(struct rdma_cm_event *event);

static struct context *s_ctx = NULL;

int main(int argc, char **argv)
{
#if _USE_IPV6
  struct sockaddr_in6 addr;
#else
  struct sockaddr_in addr;
#endif
  struct rdma_cm_event *event = NULL;
  struct rdma_cm_id *listener = NULL;
  struct rdma_event_channel *ec = NULL;
  uint16_t port = 0;

  memset(&addr, 0, sizeof(addr));
#if _USE_IPV6
  addr.sin6_family = AF_INET6;
#else
  addr.sin_family = AF_INET;
#endif

  TEST_Z(ec = rdma_create_event_channel());//当有事件发生时，通知应用程序的通道
  TEST_NZ(rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP));//等价socket，identify
  TEST_NZ(rdma_bind_addr(listener, (struct sockaddr *)&addr));//绑定本地地址和端口
  TEST_NZ(rdma_listen(listener, 10)); /* backlog=10 is arbitrary */ //开始listen

  port = ntohs(rdma_get_src_port(listener)); //获取系统分配的端口号

  printf("listening on port %d.\n", port);//输出提示

  while (rdma_get_cm_event(ec, &event) == 0) {//从event_channel获取一个事件，阻塞调用
    struct rdma_cm_event event_copy;

    memcpy(&event_copy, event, sizeof(*event));  //将event内容copy出来
    rdma_ack_cm_event(event); //对于每个event都要ack，对应rdma_get_cm_event，否则内存leak

    if (on_event(&event_copy)) //处理事件内容
      break;
  }

  rdma_destroy_id(listener);  //销毁identify
  rdma_destroy_event_channel(ec); //销毁通信通道

  return 0;
}

void die(const char *reason)
{
  fprintf(stderr, "%s\n", reason);
  exit(EXIT_FAILURE);
}

void build_context(struct ibv_context *verbs)
{
  if (s_ctx) {   //如果已有context
    if (s_ctx->ctx != verbs)
      die("cannot handle events in more than one context.");   //如果本地上下文环境和要处理的上下文环境不一致

    return;
  }

  s_ctx = (struct context *)malloc(sizeof(struct context));

  s_ctx->ctx = verbs;

  TEST_Z(s_ctx->pd = ibv_alloc_pd(s_ctx->ctx)); //创建保护域，在内存与队列建立关联关系，防止未授权的访问
  TEST_Z(s_ctx->comp_channel = ibv_create_comp_channel(s_ctx->ctx));//当有事件完成时，通过此事件通道通知应用
  TEST_Z(s_ctx->cq = ibv_create_cq(s_ctx->ctx, 10, NULL, s_ctx->comp_channel, 0)); /* cqe=10 is arbitrary *///创建对应事件通道的完成队列
  TEST_NZ(ibv_req_notify_cq(s_ctx->cq, 0));   //为下一个放入cq的请求添加通知，能被ibv_get_cq_event获取

  TEST_NZ(pthread_create(&s_ctx->cq_poller_thread, NULL, poll_cq, NULL));  //创建新线程执行poll_cq函数
}

void build_qp_attr(struct ibv_qp_init_attr *qp_attr)
{
  memset(qp_attr, 0, sizeof(*qp_attr));

  qp_attr->send_cq = s_ctx->cq;
  qp_attr->recv_cq = s_ctx->cq;
  qp_attr->qp_type = IBV_QPT_RC;

  qp_attr->cap.max_send_wr = 10;
  qp_attr->cap.max_recv_wr = 10;
  qp_attr->cap.max_send_sge = 1;
  qp_attr->cap.max_recv_sge = 1;
}

void * poll_cq(void *ctx)
{
  struct ibv_cq *cq;
  struct ibv_wc wc;

  while (1) {
    TEST_NZ(ibv_get_cq_event(s_ctx->comp_channel, &cq, &ctx));  //从cq中读出请求，阻塞
    ibv_ack_cq_events(cq, 1);   //accept该请求
    TEST_NZ(ibv_req_notify_cq(cq, 0));  //将下一个cq请求加上通知

    while (ibv_poll_cq(cq, 1, &wc))  //读取work completion到wc，一次读取一个直到cq为空
      on_completion(&wc);     //处理cq的wc
  }

  return NULL;
}

void post_receives(struct connection *conn)
{
  struct ibv_recv_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  wr.wr_id = (uintptr_t)conn;
  wr.next = NULL;
  wr.sg_list = &sge;
  wr.num_sge = 1;

  sge.addr = (uintptr_t)conn->recv_region;
  sge.length = BUFFER_SIZE;
  sge.lkey = conn->recv_mr->lkey;

  TEST_NZ(ibv_post_recv(conn->qp, &wr, &bad_wr));
}

void register_memory(struct connection *conn)
{
  conn->send_region = malloc(BUFFER_SIZE);
  conn->recv_region = malloc(BUFFER_SIZE);

  TEST_Z(conn->send_mr = ibv_reg_mr( //注册内存区域，RDMA使用的内存
    s_ctx->pd,
    conn->send_region,
    BUFFER_SIZE,
    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE));

  TEST_Z(conn->recv_mr = ibv_reg_mr(
    s_ctx->pd,
    conn->recv_region,
    BUFFER_SIZE,
    IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE));
}

void on_completion(struct ibv_wc *wc)
{
  if (wc->status != IBV_WC_SUCCESS)   //如果状态不是成功，则报错
    die("on_completion: status is not IBV_WC_SUCCESS.");

  if (wc->opcode & IBV_WC_RECV) {    //如果是recv请求，输入接受的信息
    struct connection *conn = (struct connection *)(uintptr_t)wc->wr_id;

    printf("received message: %s\n", conn->recv_region);

  } else if (wc->opcode == IBV_WC_SEND) {   //否则是send请求，输出提示
    printf("send completed successfully.\n");
  }
}

int on_connect_request(struct rdma_cm_id *id)
{
  struct ibv_qp_init_attr qp_attr;
  struct rdma_conn_param cm_params;
  struct connection *conn;

  printf("received connection request.\n");

  build_context(id->verbs);   //构建上下文环境
  build_qp_attr(&qp_attr);   //队列对初始化

  TEST_NZ(rdma_create_qp(id, s_ctx->pd, &qp_attr));  //创建接收队列和发送队列

  id->context = conn = (struct connection *)malloc(sizeof(struct connection));   //分配连接数据结构空间
  conn->qp = id->qp;

  register_memory(conn);   //为本地连接注册空间
  post_receives(conn);     //发出接受连接的信息

  memset(&cm_params, 0, sizeof(cm_params));
  TEST_NZ(rdma_accept(id, &cm_params)); //准备好接收client请求

  return 0;
}

int on_connection(void *context)
{
  struct connection *conn = (struct connection *)context;
  struct ibv_send_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;

  snprintf(conn->send_region, BUFFER_SIZE, "message from passive/server side with pid %d", getpid());

  printf("connected. posting send...\n");

  memset(&wr, 0, sizeof(wr));

  wr.opcode = IBV_WR_SEND;   //通信模式是send/recv
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;

  sge.addr = (uintptr_t)conn->send_region;
  sge.length = BUFFER_SIZE;
  sge.lkey = conn->send_mr->lkey;

  TEST_NZ(ibv_post_send(conn->qp, &wr, &bad_wr));  //server将sge内容发送到远端qp中

  return 0;
}

int on_disconnect(struct rdma_cm_id *id)
{
  struct connection *conn = (struct connection *)id->context;

  printf("peer disconnected.\n");

  rdma_destroy_qp(id);

  ibv_dereg_mr(conn->send_mr);
  ibv_dereg_mr(conn->recv_mr);

  free(conn->send_region);
  free(conn->recv_region);

  free(conn);

  rdma_destroy_id(id);

  return 0;
}

int on_event(struct rdma_cm_event *event)
{
  int r = 0;

  if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST)  //如果为连接请求
    r = on_connect_request(event->id);   //处理连接请求   id属于client or？
  else if (event->event == RDMA_CM_EVENT_ESTABLISHED)
    r = on_connection(event->id->context);    //建立连接
  else if (event->event == RDMA_CM_EVENT_DISCONNECTED)  
    r = on_disconnect(event->id);  
  else
    die("on_event: unknown event.");

  return r;
}

