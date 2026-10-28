#include "mcp_delayed_probe.h"
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "MCP_PROBE";
typedef struct probe_job probe_job_t;
struct mcp_delayed_probe {
    portMUX_TYPE lock;
    SemaphoreHandle_t idle;
    uint32_t generation;
    bool online, stopping;
    probe_job_t *active;
    mcp_probe_send_t send;
    void *send_context;
};
struct probe_job {
    mcp_delayed_probe_t *owner;
    uint32_t generation, delay_ms;
    char *wire, *id, *session, *question;
    mcp_probe_work_t work;
    int64_t received_us;
};

static char *copy_string(const char *text)
{
    size_t n = strlen(text) + 1;
    char *copy = malloc(n);
    if (copy) memcpy(copy, text, n);
    return copy;
}
static bool job_current(void *context)
{
    probe_job_t *job = context;
    mcp_delayed_probe_t *p = job->owner;
    portENTER_CRITICAL(&p->lock);
    bool valid = p->online && !p->stopping && p->generation == job->generation;
    portEXIT_CRITICAL(&p->lock);
    return valid;
}
static void free_job(probe_job_t *job)
{
    if (!job) return;
    free(job->wire); free(job->id); free(job->session); free(job->question); free(job);
}
static void probe_worker(void *context)
{
    probe_job_t *job = context;
    mcp_delayed_probe_t *p = job->owner;
    int64_t deadline = job->received_us + job->delay_ms * 1000LL;
    while (job_current(job) && esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (job->work && job_current(job)) {
        job->wire=job->work(job->id,job->session,job->question,job_current,job);
    }
    bool current = job_current(job);
    ESP_LOGI(TAG, "worker id=%s epoch=%lu ready_ms=%lld current=%d", job->id,
             (unsigned long)job->generation, (long long)((esp_timer_get_time()-job->received_us)/1000), current);
    if (current && job->wire) {
        esp_err_t ret = p->send(p->send_context, job->wire, job_current, job);
        ESP_LOGI(TAG, "response id=%s epoch=%lu write=%s current_after=%d stack_free=%u (not cloud ACK or TTS)",
                 job->id, (unsigned long)job->generation, esp_err_to_name(ret), job_current(job),
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    } else if (!current) {
        ESP_LOGI(TAG, "discard id=%s epoch=%lu (connection/session changed)", job->id, (unsigned long)job->generation);
    } else {
        ESP_LOGE(TAG,"work failed id=%s epoch=%lu (no response allocated)",job->id,(unsigned long)job->generation);
    }
    portENTER_CRITICAL(&p->lock);
    p->active = NULL;
    portEXIT_CRITICAL(&p->lock);
    free_job(job);
    /* After give, teardown may free p. Never touch p/context after this. */
    xSemaphoreGive(p->idle);
    vTaskDelete(NULL);
}

mcp_delayed_probe_t *mcp_delayed_probe_create(mcp_probe_send_t send, void *context)
{
    if (!send) return NULL;
    mcp_delayed_probe_t *p = malloc(sizeof(*p));
    if (!p) return NULL;
    p->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    p->generation = 0; p->online = false; p->stopping = false; p->active = NULL;
    p->send = send; p->send_context = context;
    p->idle = xSemaphoreCreateBinary();
    if (!p->idle) { free(p); return NULL; }
    xSemaphoreGive(p->idle);
    return p;
}
void mcp_delayed_probe_resume(mcp_delayed_probe_t *p)
{
    if (!p) return;
    portENTER_CRITICAL(&p->lock);
    ++p->generation; p->stopping = false; p->online = false;
    portEXIT_CRITICAL(&p->lock);
}
void mcp_delayed_probe_connection(mcp_delayed_probe_t *p, bool online)
{
    if (!p) return;
    portENTER_CRITICAL(&p->lock);
    ++p->generation; p->online = online && !p->stopping;
    portEXIT_CRITICAL(&p->lock);
}
void mcp_delayed_probe_session_changed(mcp_delayed_probe_t *p)
{
    if (!p) return;
    portENTER_CRITICAL(&p->lock);
    ++p->generation;
    portEXIT_CRITICAL(&p->lock);
}
void mcp_delayed_probe_drain(mcp_delayed_probe_t *p)
{
    if (!p) return;
    portENTER_CRITICAL(&p->lock);
    ++p->generation; p->stopping = true; p->online = false;
    portEXIT_CRITICAL(&p->lock);
    xSemaphoreTake(p->idle, portMAX_DELAY);
    xSemaphoreGive(p->idle);
}
void mcp_delayed_probe_destroy(mcp_delayed_probe_t *p)
{
    if (!p) return;
    mcp_delayed_probe_drain(p);
    vSemaphoreDelete(p->idle);
    free(p);
}
static esp_err_t submit(mcp_delayed_probe_t *p, const char *wire,
                        const char *id, const char *session, uint32_t delay_ms,
                        const char *question, mcp_probe_work_t work)
{
    if (!p || (!wire && !work) || !id || !session || !*session ||
        (wire && strlen(wire)>1536) || strlen(id)>128 || strlen(session)>256 ||
        (work && (!question || !*question || strlen(question)>1000)) ||
        (delay_ms != 0 && delay_ms != 3000)) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(p->idle, 0) != pdTRUE) {
        portENTER_CRITICAL(&p->lock);
        bool duplicate = p->active && p->active->generation == p->generation && !strcmp(p->active->id,id);
        portEXIT_CRITICAL(&p->lock);
        return duplicate ? MCP_PROBE_DUPLICATE : ESP_ERR_INVALID_STATE;
    }
    probe_job_t *job = malloc(sizeof(*job));
    if (!job) { xSemaphoreGive(p->idle); return ESP_ERR_NO_MEM; }
    job->owner=p; job->delay_ms=delay_ms; job->received_us=esp_timer_get_time();
    job->work=work; job->wire=wire ? copy_string(wire) : NULL;
    job->question=question ? copy_string(question) : NULL;
    job->id=copy_string(id); job->session=copy_string(session);
    if ((wire && !job->wire) || (question && !job->question) || !job->id || !job->session) {
        free_job(job); xSemaphoreGive(p->idle); return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&p->lock);
    bool admitted = p->online && !p->stopping;
    job->generation=p->generation;
    if (admitted) p->active=job;
    portEXIT_CRITICAL(&p->lock);
    if (!admitted) { free_job(job); xSemaphoreGive(p->idle); return ESP_ERR_INVALID_STATE; }
    ESP_LOGI(TAG, "request id=%s session=%s epoch=%lu delay_ms=%lu", id, session,
             (unsigned long)job->generation, (unsigned long)delay_ms);
    if (xTaskCreate(probe_worker,"mcp_probe",work ? 8192 : 6144,job,tskIDLE_PRIORITY+2,NULL) != pdPASS) {
        portENTER_CRITICAL(&p->lock); p->active=NULL; portEXIT_CRITICAL(&p->lock);
        free_job(job); xSemaphoreGive(p->idle); return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
esp_err_t mcp_delayed_probe_submit(mcp_delayed_probe_t *p,const char *wire,const char *id,
                                 const char *session,uint32_t delay_ms)
{
    return submit(p,wire,id,session,delay_ms,NULL,NULL);
}
esp_err_t mcp_delayed_probe_submit_work(mcp_delayed_probe_t *p,const char *id,const char *session,
                                      const char *question,mcp_probe_work_t work)
{
    if (!work) return ESP_ERR_INVALID_ARG;
    return submit(p,NULL,id,session,0,question,work);
}
