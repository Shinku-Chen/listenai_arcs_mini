#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include "sys_init.h"
#include "lisa_log.h"
#include "ebus/ebus.h"
#include "voice_msg.h"
#include "sysheap.h"

#define TAG "voice.msg"

static ebus_chn_t *voice_default_chn = NULL;

struct voice_invoke {
    voice_invoke_worker_t worker;
    uint32_t data_len;
    uint8_t data[0];
};

struct voice_invoke_sync {
    voice_invoke_sync_worker_t worker;
    uint32_t data_len;
    uint32_t rsp_len;
    SemaphoreHandle_t done_sem;
    SemaphoreHandle_t release_sem;
    struct voice_invoke_rsp *rsp;
    void *worker_data;
    struct voice_invoke_sync *context;
};

static SemaphoreHandle_t voice_invoke_sync_lock = NULL;

static void voice_invoke_sumbit_msg_received(void *unused, uint32_t evt, void *data, uint32_t len, void *user_data)
{
    struct voice_invoke *invoke = (struct voice_invoke *)data;
    if (invoke == NULL) {
        return;
    }

    LOGI("voice invoke, worker: %p, data: %p, len: %d", invoke->worker, invoke->data, invoke->data_len);

    if (invoke->worker) {
        invoke->worker(invoke->data, invoke->data_len);
    }
}

static void voice_invoke_sync_sumbit_msg_received(void *unused, uint32_t evt, void *data, uint32_t len, void *user_data)
{
    struct voice_invoke_sync *message = (struct voice_invoke_sync *)data;
    if (message == NULL || message->context == NULL) {
        return;
    }
    struct voice_invoke_sync *invoke = message->context;

    LOGI("voice invoke, worker: %p, data: %p, len: %d", message->worker, message->worker_data, message->data_len);

    if (message->worker) {
        message->worker(message->worker_data, message->data_len, message->rsp);
    }

    xSemaphoreGive(invoke->done_sem);
    xSemaphoreTake(invoke->release_sem, portMAX_DELAY);
    vSemaphoreDelete(invoke->done_sem);
    vSemaphoreDelete(invoke->release_sem);
    psram_free(invoke);
}

int voice_invoke(voice_invoke_worker_t worker, void *data, uint32_t len)
{
    if (worker == NULL) {
        return -1;
    }

    struct voice_invoke *invoke = psram_malloc(sizeof(*invoke) + len);
    if (invoke == NULL) {
        return -1;
    }

    invoke->worker = worker;
    invoke->data_len = len;

    if (len) {
        memcpy(invoke->data, data, len);
    }

    /* The bus copies the payload before returning, including for async
     * delivery. The caller still owns this temporary staging buffer. */
    int ret = voice_msg_pub(VOICE_MSG_INVOKE_SUBMIT, invoke, sizeof(*invoke) + len);
    psram_free(invoke);
    return ret;
}

int voice_invoke_sync_ex(voice_invoke_sync_worker_t worker, void *data, uint32_t len, struct voice_invoke_rsp *rsp,
                         uint32_t rsp_len, uint32_t timeout)
{
    if (worker == NULL || rsp == NULL || rsp_len < sizeof(struct voice_invoke_rsp) || (len && data == NULL)) {
        return -1;
    }

    if (rsp_len > UINT32_MAX - (sizeof(uint32_t) - 1U)) {
        return -1;
    }
    const uint32_t rsp_storage_len = (rsp_len + sizeof(uint32_t) - 1U) & ~(sizeof(uint32_t) - 1U);
    if (rsp_storage_len > UINT32_MAX - sizeof(struct voice_invoke_sync) ||
        len > UINT32_MAX - sizeof(struct voice_invoke_sync) - rsp_storage_len) {
        return -1;
    }

    struct voice_invoke_sync *invoke_sync = psram_malloc(sizeof(*invoke_sync) + rsp_storage_len + len);
    if (invoke_sync == NULL) {
        return -1;
    }

    invoke_sync->worker = worker;
    invoke_sync->data_len = len;
    invoke_sync->rsp_len = rsp_len;
    invoke_sync->done_sem = xSemaphoreCreateBinary();
    invoke_sync->release_sem = xSemaphoreCreateBinary();
    invoke_sync->rsp = (struct voice_invoke_rsp *)(invoke_sync + 1);
    invoke_sync->worker_data = (uint8_t *)invoke_sync->rsp + rsp_storage_len;
    invoke_sync->context = invoke_sync;

    if (invoke_sync->done_sem == NULL || invoke_sync->release_sem == NULL) {
        if (invoke_sync->done_sem) {
            vSemaphoreDelete(invoke_sync->done_sem);
        }
        if (invoke_sync->release_sem) {
            vSemaphoreDelete(invoke_sync->release_sem);
        }
        psram_free(invoke_sync);
        return -1;
    }

    memcpy(invoke_sync->rsp, rsp, rsp_len);

    if (len) {
        memcpy(invoke_sync->worker_data, data, len);
    }

    BaseType_t r = xSemaphoreTake(voice_invoke_sync_lock, timeout);
    if (r != pdPASS) {
        vSemaphoreDelete(invoke_sync->done_sem);
        vSemaphoreDelete(invoke_sync->release_sem);
        psram_free(invoke_sync);
        return -1;
    }

    int pub_ret = voice_msg_pub(VOICE_MSG_INVOKE_SYNC_SUBMIT, invoke_sync, sizeof(*invoke_sync));
    if (pub_ret == 0) {
        r = xSemaphoreTake(invoke_sync->done_sem, timeout);
    } else {
        r = pdFAIL;
    }

    if (pub_ret != 0) {
        vSemaphoreDelete(invoke_sync->done_sem);
        vSemaphoreDelete(invoke_sync->release_sem);
        psram_free(invoke_sync);
    } else {
        if (r == pdPASS) {
            memcpy(rsp, invoke_sync->rsp, rsp_len);
        }
        /* The callback owns the context until it consumes this release. */
        xSemaphoreGive(invoke_sync->release_sem);
    }

    xSemaphoreGive(voice_invoke_sync_lock);

    return r == pdPASS ? 0 : -1;
}

int voice_invoke_sync(voice_invoke_sync_worker_t worker, void *data, uint32_t len, struct voice_invoke_rsp *rsp,
                      uint32_t timeout)
{
    return voice_invoke_sync_ex(worker, data, len, rsp, sizeof(*rsp), timeout);
}

int voice_msg_init(void)
{
    int r;
    ebus_handle_t *bus;
    ebus_chn_t *chn;

    r = ebus_init();
    LOGI("voice_msg_init, ebus_init ret:%d", r);
    assert(r == 0);

    bus = ebus_create_async("voice.ebus", 128, 32 * 1024, 6);
    LOGI("voice_msg_init, ebus_create ret:%p", bus);
    assert(bus);

    voice_default_chn = ebus_chn_create_attach(bus, "voice.ebus.default");
    LOGI("voice_msg_init, ebus_chn_create_attach ret:%p", voice_default_chn);
    assert(voice_default_chn);

    voice_invoke_sync_lock = xSemaphoreCreateMutex();
    assert(voice_invoke_sync_lock);

    voice_msg_sub(VOICE_MSG_INVOKE_SUBMIT, voice_invoke_sumbit_msg_received, NULL);
    voice_msg_sub(VOICE_MSG_INVOKE_SYNC_SUBMIT, voice_invoke_sync_sumbit_msg_received, NULL);

    return 0;
}

int voice_msg_pub(uint32_t evt, void *data, uint32_t len)
{
    ebus_chn_t *chn;
    int r;

    chn = voice_default_chn;
    if (chn == NULL) {
        LOGE("voice msg chn is null, evt:%d", evt);
        return -1;
    }

    r = ebus_message_pub_async(chn, evt, data, len);
    if (r != 0) {
        LOGE("voice msg pub failed, evt:%d", evt);
        return -1;
    }

    return 0;
}

int voice_msg_sub(uint32_t evt, voice_msg_cb_t cb, void *user_data)
{
    ebus_chn_t *chn;
    int r;

    chn = voice_default_chn;
    if (chn == NULL) {
        LOGE("voice msg chn is null, evt:%d", evt);
        return -1;
    }

    r = ebus_message_subscribe(chn, EBUS_SUBSCRIBER_TYPE_ASYNC, evt, (ebus_chn_cb_t)cb, user_data);
    if (r != 0) {
        LOGE("voice msg sub failed, evt:%d", evt);
        return -1;
    }

    return 0;
}

int voice_msg_unsub(uint32_t evt, voice_msg_cb_t cb)
{
    ebus_chn_t *chn;
    int r;

    chn = voice_default_chn;
    if (chn == NULL) {
        LOGE("voice msg chn is null, evt:%d", evt);
        return -1;
    }

    r = ebus_message_unsubscribe(chn, (ebus_chn_cb_t)cb);
    if (r != 0) {
        LOGE("voice msg unsub failed, evt:%d", evt);
        return -1;
    }

    return 0;
}

SYS_INIT(voice_msg_init, SYS_INIT_LEVEL_PRE_APPLICATION, 30);
