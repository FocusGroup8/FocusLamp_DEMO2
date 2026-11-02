/* The actual head WebSocket handlers and raw framing are tested below with
 * bounded fake sockets. Assertions also reject recursive mutex acquisition
 * and any I/O performed inside a FreeRTOS spinlock. No hardware/network. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../../head/components/websocket_manager/src/websocket_manager.c"

static int64_t now_us;
static _Thread_local unsigned tracking_depth, camera_depth, critical_depth;
static CRITICAL_SECTION tracking_mutex, camera_mutex;
static HANDLE send_entered, send_release, camera_waiting;
static bool block_send;
static void (*before_camera_take)(void);
static uint8_t wire[65536];
static size_t wire_len, send_chunk=3;
static size_t fail_after=SIZE_MAX;
static bool fail_with_budget;
static int last_shutdown=-1, shutdown_count, async_close_count;

int64_t esp_timer_get_time(void) {return now_us;}
void test_ws_log(const char *tag,const char *format,...) {(void)tag;(void)format;}
size_t esp_get_free_heap_size(void) {return 1000000;}
void test_ws_critical_enter(void) {assert(critical_depth==0);++critical_depth;}
void test_ws_critical_exit(void) {assert(critical_depth==1);--critical_depth;}
SemaphoreHandle_t xSemaphoreCreateMutex(void) {assert(0);return NULL;}
int xSemaphoreTake(SemaphoreHandle_t handle,uint32_t ticks)
{
    (void)ticks;assert(handle && !critical_depth);
    if(handle==s_camera_send_mutex){
        assert(!camera_depth && !tracking_depth);
        if(before_camera_take){void (*hook)(void)=before_camera_take;before_camera_take=NULL;hook();}
        if(!TryEnterCriticalSection(&camera_mutex)){
            SetEvent(camera_waiting);EnterCriticalSection(&camera_mutex);
        }
        ++camera_depth;
    }else{
        assert(handle==s_server_mutex && !tracking_depth);
        EnterCriticalSection(&tracking_mutex);++tracking_depth;
    }
    return pdTRUE;
}
int xSemaphoreGive(SemaphoreHandle_t handle)
{
    assert(handle && !critical_depth);
    if(handle==s_camera_send_mutex){assert(camera_depth==1 && !tracking_depth);--camera_depth;LeaveCriticalSection(&camera_mutex);}
    else{assert(handle==s_server_mutex && tracking_depth==1);--tracking_depth;LeaveCriticalSection(&tracking_mutex);}
    return pdTRUE;
}
void vSemaphoreDelete(SemaphoreHandle_t handle) {(void)handle;}
int test_ws_send(int fd,const void *data,size_t length,int flags)
{
    assert(!critical_depth && !tracking_depth && camera_depth==1 && fd==s_camera_client_fd);
    assert(flags==(MSG_NOSIGNAL|MSG_DONTWAIT));
    if(block_send){
        block_send=false;SetEvent(send_entered);
        assert(WaitForSingleObject(send_release,5000)==WAIT_OBJECT_0);
    }
    if(wire_len>=fail_after){errno=fail_with_budget?EAGAIN:ECONNRESET;return -1;}
    size_t n=length<send_chunk?length:send_chunk;
    if(n>fail_after-wire_len)n=fail_after-wire_len;
    assert(wire_len+n<=sizeof(wire));memcpy(wire+wire_len,data,n);wire_len+=n;return (int)n;
}
int test_ws_shutdown(int fd,int how)
{assert(!critical_depth && !tracking_depth && camera_depth==1 && how==SHUT_RDWR);last_shutdown=fd;++shutdown_count;return 0;}
int test_ws_close(int fd) {(void)fd;assert(!critical_depth && !tracking_depth && !camera_depth);return 0;}
int test_ws_setsockopt(int fd,int level,int option,const void *value,size_t size)
{(void)fd;(void)level;(void)option;(void)value;(void)size;assert(!critical_depth);return 0;}
int test_ws_select(int count,fd_set *readable,fd_set *writable,fd_set *error,struct timeval *timeout)
{
    (void)count;(void)readable;(void)writable;(void)error;assert(!critical_depth && !tracking_depth);
    if(fail_with_budget){now_us+=(int64_t)timeout->tv_sec*1000000+timeout->tv_usec;return 0;}
    return 1;
}
int httpd_req_to_sockfd(httpd_req_t *req) {return req->fd;}
int httpd_ws_get_fd_info(httpd_handle_t handle,int fd)
{(void)handle;(void)fd;return HTTPD_WS_CLIENT_WEBSOCKET;}
esp_err_t httpd_ws_recv_frame(httpd_req_t *req,httpd_ws_frame_t *frame,size_t max_len)
{
    assert(!critical_depth && !tracking_depth && !camera_depth);
    if(max_len){assert(max_len>=req->incoming.len);memcpy(frame->payload,req->incoming.payload,req->incoming.len);}
    frame->type=req->incoming.type;frame->len=req->incoming.len;frame->final=req->incoming.final;return ESP_OK;
}
esp_err_t httpd_ws_send_frame_async(httpd_handle_t handle,int fd,const httpd_ws_frame_t *frame)
{(void)handle;(void)frame;assert(!critical_depth && !camera_depth && fd!=s_camera_client_fd);return ESP_OK;}
esp_err_t httpd_resp_send_err(httpd_req_t *req,int code,const char *message)
{(void)req;(void)code;(void)message;assert(!critical_depth);return ESP_OK;}
esp_err_t httpd_resp_sendstr(httpd_req_t *req,const char *text) {(void)req;(void)text;return ESP_OK;}
esp_err_t httpd_resp_set_type(httpd_req_t *req,const char *type) {(void)req;(void)type;return ESP_OK;}
esp_err_t httpd_sess_trigger_close(httpd_handle_t handle,int fd)
{(void)handle;(void)fd;assert(!critical_depth);++async_close_count;return ESP_OK;}
esp_err_t httpd_sess_update_lru_counter(httpd_handle_t handle,int fd) {(void)handle;(void)fd;return ESP_OK;}
esp_err_t httpd_register_uri_handler(httpd_handle_t handle,const httpd_uri_t *uri) {(void)handle;(void)uri;return ESP_OK;}
esp_err_t httpd_start(httpd_handle_t *handle,const httpd_config_t *config) {(void)config;*handle=(void*)1;return ESP_OK;}
esp_err_t httpd_stop(httpd_handle_t handle) {(void)handle;return ESP_OK;}

static void wire_reset(void) {wire_len=0;fail_after=SIZE_MAX;fail_with_budget=false;}
static void reset_server(void)
{
    now_us=0;tracking_depth=camera_depth=critical_depth=0;wire_reset();
    s_server=(void*)1;s_server_mutex=&tracking_mutex;s_camera_send_mutex=&camera_mutex;s_server_running=true;
    s_client_count=0;s_algo_client_fd=-1;s_camera_fail_count=0;
    s_camera_state=(ws_camera_client_state_t){0};camera_client_set_fd(-1);
    last_shutdown=-1;shutdown_count=async_close_count=0;
    block_send=false;before_camera_take=NULL;
    ResetEvent(send_entered);ResetEvent(send_release);ResetEvent(camera_waiting);
}
static esp_err_t accept(int fd,const char *uri)
{
    httpd_req_t req={.handle=(void*)1,.method=HTTP_GET,.fd=fd,.uri=uri};
    return ws_server_handler(&req);
}
static void control(int fd,unsigned opcode,const uint8_t *payload,size_t length)
{
    httpd_req_t req={.handle=(void*)1,.method=HTTP_POST,.fd=fd,.uri="",
        .incoming={.payload=(uint8_t*)payload,.len=length,.type=(uint8_t)opcode,.final=true}};
    assert(ws_server_handler(&req)==ESP_OK);
}
static size_t read_frame(size_t pos,unsigned opcode,const uint8_t **payload,size_t *length)
{
    assert(pos+2<=wire_len && wire[pos++]==(0x80|opcode));
    unsigned code=wire[pos++];assert(!(code&0x80));
    size_t len=code;
    if(code==126){assert(pos+2<=wire_len);len=((size_t)wire[pos]<<8)|wire[pos+1];pos+=2;}
    assert(len<=125 || code==126);assert(pos+len<=wire_len);
    *payload=wire+pos;*length=len;return pos+len;
}
static esp_err_t current_pair(const char *header, int n, const char *jpeg, int size)
{
    ws_camera_send_result_t wire_result;
    return ws_manager_server_send_camera_pair(s_camera_state.session_id, header, n, jpeg, size, &wire_result);
}
static void pair(void)
{
    static const char text[]="{\"type\":\"frame_header\",\"seq\":8}";
    static const char jpeg[]={-1,-40,0x89,0x08,1,2,3,-1,-39};
    assert(current_pair(text,sizeof(text)-1,jpeg,sizeof(jpeg))==ESP_OK);
}
static void pure_lease_tests(void)
{
    ws_camera_lease_t lease;uint8_t payload[8],wrong[8];
    ws_camera_lease_start(&lease,5,0);
    assert(!ws_camera_lease_ping_due(&lease,4999999));
    assert(ws_camera_lease_ping_due(&lease,5000000));
    ws_camera_lease_ping_started(&lease,5000000,payload);memcpy(wrong,payload,8);wrong[7]^=1;
    assert(!ws_camera_lease_ping_due(&lease,15000000)); /* one outstanding Ping */
    assert(!ws_camera_lease_pong(&lease,wrong,8,6000000));
    assert(!ws_camera_lease_pong(&lease,payload,7,6000000));
    assert(!ws_camera_lease_expired(&lease,19999999));
    assert(ws_camera_lease_expired(&lease,20000000));
    assert(!ws_camera_lease_pong(&lease,payload,8,20000000)); /* late does not revive */
    ws_camera_lease_start(&lease,6,0);ws_camera_lease_ping_started(&lease,5000000,wrong);
    assert(!ws_camera_lease_pong(&lease,payload,8,6000000)); /* old session */
    assert(ws_camera_lease_pong(&lease,wrong,8,6000000));
    assert(!ws_camera_lease_pong(&lease,wrong,8,7000000)); /* duplicate */
}
static void actual_handler_tests(void)
{
    reset_server();assert(accept(11,"/camera")==ESP_OK);assert(accept(12,"/algo")==ESP_OK);
    now_us=4999999;pair();const uint8_t *payload;size_t length;
    size_t pos=read_frame(0,1,&payload,&length);pos=read_frame(pos,2,&payload,&length);assert(pos==wire_len);
    now_us=5000000;wire_reset();pair();
    pos=read_frame(0,9,&payload,&length);assert(length==8);uint8_t ping1[8];memcpy(ping1,payload,8);
    pos=read_frame(pos,1,&payload,&length);pos=read_frame(pos,2,&payload,&length);assert(pos==wire_len);
    assert(s_camera_state.heartbeat_pending && s_client_count==2);
    assert(accept(13,"/camera?client=other")==ESP_FAIL && s_camera_client_fd==11);
    now_us=5200000;control(11,HTTPD_WS_TYPE_PONG,ping1,8);
    assert(!s_camera_state.heartbeat_pending && ws_manager_server_get_camera_state().heartbeat_pong_age_ms==0);
    now_us=10000000;wire_reset();pair();pos=read_frame(0,9,&payload,&length);
    uint8_t ping2[8];memcpy(ping2,payload,8);assert(memcmp(ping1,ping2,8));
    now_us=11000000;control(11,HTTPD_WS_TYPE_PONG,ping1,8); /* replay */
    control(12,HTTPD_WS_TYPE_PONG,ping2,8); /* another endpoint cannot renew */
    assert(s_camera_state.heartbeat_pending);
    /* Client Ping must receive a matching Pong but cannot renew our lease. */
    wire_reset();uint8_t browser_ping[]={7,8,9};control(11,HTTPD_WS_TYPE_PING,browser_ping,3);
    pos=read_frame(0,10,&payload,&length);assert(pos==wire_len && length==3 && !memcmp(payload,browser_ping,3));
    assert(s_camera_lease.deadline_us==25000000);
    now_us=25000000;wire_reset();
    /* Expiration also works from admission when no JPEG is being sent. */
    assert(accept(13,"/camera")==ESP_OK);
    assert(last_shutdown==11 && shutdown_count==1 && async_close_count==0);
    assert(s_camera_client_fd==13 && s_algo_client_fd==12 && !wire_len);
    assert(ws_manager_server_get_camera_state().heartbeat_timeouts==1);
    control(11,HTTPD_WS_TYPE_PONG,ping2,8);track_client_remove(11);
    assert(s_camera_client_fd==13 && s_algo_client_fd==12);
    now_us=30000000;pair();ws_camera_lease_payload(&s_camera_lease,ping2);
    now_us=30100000;control(13,HTTPD_WS_TYPE_PONG,ping2,8);
    /* Healthy viewers stay admitted for minutes with normal auto-Pongs. */
    for(int i=1;i<=20;++i){
        now_us=30000000+(int64_t)i*5000000;wire_reset();pair();
        ws_camera_lease_payload(&s_camera_lease,ping2);now_us+=1000;control(13,HTTPD_WS_TYPE_PONG,ping2,8);
        assert(accept(14,"/camera")==ESP_FAIL && s_camera_client_fd==13);
    }
    assert(shutdown_count==1 && s_algo_client_fd==12);
    /* A stalled proxy keeps accepting JPEG bytes, yet no Pong arrives. */
    now_us=135000000;pair();
    for(now_us=136000000;now_us<150000000;now_us+=1000000){wire_reset();pair();}
    now_us=150000000;wire_reset();
    assert(current_pair("{}",2,"jpg",3)==ESP_ERR_NOT_FOUND);
    assert(!wire_len && !s_camera_state.connected && s_algo_client_fd==12 && last_shutdown==13);
}
static void partial_ping_tests(void)
{
    reset_server();assert(accept(11,"/camera")==ESP_OK);assert(accept(12,"/algo")==ESP_OK);
    now_us=5000000;fail_after=1;
    assert(current_pair("{}",2,"jpg",3)==ESP_ERR_NOT_FOUND);
    assert(wire_len==1 && wire[0]==0x89); /* partial Ping: close, never append JPEG */
    assert(last_shutdown==11 && shutdown_count==1 && s_camera_client_fd==-1 && s_algo_client_fd==12);
    assert(!s_camera_state.heartbeat_pending && async_close_count==0);
}
static void full_admission_tests(void)
{
    reset_server();assert(accept(11,"/camera")==ESP_OK);assert(accept(12,"/algo")==ESP_OK);
    assert(accept(13,"/mcp")==ESP_OK && accept(14,"/ws")==ESP_OK);
    assert(s_client_count==WS_MANAGER_SERVER_MAX_CONN);
    now_us=5000000;pair();
    now_us=19999999;assert(accept(15,"/camera")==ESP_FAIL && s_camera_client_fd==11);
    now_us=20000000;assert(accept(15,"/camera")==ESP_OK && s_camera_client_fd==15);
    assert(s_client_count==WS_MANAGER_SERVER_MAX_CONN && s_algo_client_fd==12 && last_shutdown==11);
    /* Old cleanup cannot discard the admitted replacement or another URI. */
    ws_session_close_cb(s_server,11);
    assert(s_camera_client_fd==15 && s_client_count==WS_MANAGER_SERVER_MAX_CONN && s_algo_client_fd==12);
}

static DWORD WINAPI pair_worker(void *arg) {(void)arg;pair();return 0;}
static DWORD WINAPI pong_worker(void *arg)
{(void)arg;uint8_t payload[]={1,2,3};control(11,HTTPD_WS_TYPE_PING,payload,sizeof(payload));return 0;}
static DWORD WINAPI close_worker(void *arg) {(void)arg;ws_session_close_cb(s_server,11);return 0;}
static void join(HANDLE thread) {assert(WaitForSingleObject(thread,5000)==WAIT_OBJECT_0);CloseHandle(thread);}
static HANDLE blocked_pair(void)
{
    reset_server();assert(accept(11,"/camera")==ESP_OK);assert(accept(12,"/algo")==ESP_OK);
    block_send=true;HANDLE worker=CreateThread(NULL,0,pair_worker,NULL,0,NULL);assert(worker);
    assert(WaitForSingleObject(send_entered,5000)==WAIT_OBJECT_0);return worker;
}
static void concurrent_send_tests(void)
{
    HANDLE sender=blocked_pair();
    /* Run real registry operations and ordinary /algo output while the camera
     * thread is suspended inside send(). A global send lock deadlocks here. */
    assert(accept(13,"/mcp")==ESP_OK);
    assert(ws_manager_server_send_text(12,"ok",2)==ESP_OK);
    ws_session_close_cb(s_server,13);
    HANDLE pong=CreateThread(NULL,0,pong_worker,NULL,0,NULL);assert(pong);
    assert(WaitForSingleObject(camera_waiting,5000)==WAIT_OBJECT_0);
    assert(WaitForSingleObject(pong,0)==WAIT_TIMEOUT);
    SetEvent(send_release);join(sender);join(pong);
    const uint8_t *payload;size_t length;
    size_t pos=read_frame(0,1,&payload,&length);pos=read_frame(pos,2,&payload,&length);
    pos=read_frame(pos,10,&payload,&length);assert(pos==wire_len && length==3);
    assert(!async_close_count && !shutdown_count);

    sender=blocked_pair();
    HANDLE closer=CreateThread(NULL,0,close_worker,NULL,0,NULL);assert(closer);
    assert(WaitForSingleObject(camera_waiting,5000)==WAIT_OBJECT_0);
    assert(WaitForSingleObject(closer,0)==WAIT_TIMEOUT);
    SetEvent(send_release);join(sender);join(closer);
    pos=read_frame(0,1,&payload,&length);pos=read_frame(pos,2,&payload,&length);assert(pos==wire_len);
    assert(s_camera_client_fd==-1 && s_algo_client_fd==12);
}
static void reuse_same_fd(void)
{
    ws_session_close_cb(s_server,11);assert(accept(11,"/camera")==ESP_OK);
}
static void delayed_writer_tests(void)
{
    reset_server();assert(accept(11,"/camera")==ESP_OK);
    before_camera_take=reuse_same_fd;
    assert(ws_manager_server_send_text(11,"old",3)==ESP_ERR_NOT_FOUND);
    assert(s_camera_client_fd==11 && s_camera_state.session_id==2 && !wire_len && !shutdown_count);
    before_camera_take=reuse_same_fd;
    assert(ws_manager_server_send_binary(11,"old",3)==ESP_ERR_NOT_FOUND);
    assert(s_camera_state.session_id==3 && !wire_len && !shutdown_count);
    assert(accept(12,"/algo")==ESP_OK);
    before_camera_take=reuse_same_fd;
    assert(ws_manager_server_broadcast_text("old",3)==ESP_ERR_NOT_FOUND);
    assert(s_camera_state.session_id==4 && !wire_len && !shutdown_count);
    uint32_t target = s_camera_state.session_id;
    before_camera_take=reuse_same_fd;
    ws_camera_send_result_t result;
    assert(ws_manager_server_send_camera_pair(target, "{}", 2, "jpg", 3, &result) == ESP_ERR_NOT_FOUND);
    assert(s_camera_state.session_id == target + 1 && !wire_len && !shutdown_count && !result.wire_bytes);
    ws_send_stats_t window;
    assert(ws_manager_server_take_send_stats(&window) == ESP_OK);
    pair(); /* replacement still works */
    assert(ws_manager_server_take_send_stats(&window) == ESP_OK);
    assert(window.total_sent == 1 && window.jpeg_bytes == 9 && !window.total_failed);
    assert(ws_manager_server_take_send_stats(&window) == ESP_OK);
    assert(!window.total_sent && !window.jpeg_bytes);
}
static void budget_failure_tests(void)
{
    const size_t failures[]={0,1,4,7};
    for(size_t i=0;i<sizeof(failures)/sizeof(failures[0]);++i){
        reset_server();assert(accept(11,"/camera")==ESP_OK);assert(accept(12,"/algo")==ESP_OK);
        fail_after=failures[i];fail_with_budget=true;
        uint32_t before=atomic_load(&s_camera_budget_timeouts_total);
        esp_err_t result=current_pair("{}",2,"jpg",3);
        assert(now_us==1500000 && wire_len==failures[i]);
        assert(atomic_load(&s_camera_budget_timeouts_total)==before+1);
        assert(s_algo_client_fd==12 && !async_close_count);
        if(failures[i]==0){
            assert(result==ESP_ERR_TIMEOUT && !shutdown_count && s_camera_client_fd==11);
            wire_reset();pair(); /* zero progress leaves framing untouched */
        }else{
            assert(result==ESP_FAIL && shutdown_count==1 && last_shutdown==11 && s_camera_client_fd==-1);
            size_t stopped=wire_len;
            assert(current_pair("{}",2,"jpg",3)==ESP_ERR_NOT_FOUND && wire_len==stopped);
        }
    }
}
int main(void)
{
    InitializeCriticalSection(&tracking_mutex);InitializeCriticalSection(&camera_mutex);
    send_entered=CreateEvent(NULL,TRUE,FALSE,NULL);send_release=CreateEvent(NULL,TRUE,FALSE,NULL);
    camera_waiting=CreateEvent(NULL,TRUE,FALSE,NULL);assert(send_entered && send_release && camera_waiting);
    pure_lease_tests();actual_handler_tests();partial_ping_tests();full_admission_tests();
    concurrent_send_tests();delayed_writer_tests();budget_failure_tests();
    assert(!tracking_depth && !camera_depth && !critical_depth);
    puts("PASS: actual head framing/lease + threaded JPEG/Pong/close serialization, registry and /algo progress during blocked send, reused-fd delayed writers rejected, 1500ms zero/partial progress budget failures, no tracking-lock camera I/O");
    CloseHandle(send_entered);CloseHandle(send_release);CloseHandle(camera_waiting);
    DeleteCriticalSection(&tracking_mutex);DeleteCriticalSection(&camera_mutex);
}
