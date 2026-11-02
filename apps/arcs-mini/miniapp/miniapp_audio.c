#include "miniapp_audio.h"

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include "alarm_ring.h"
#include "voice_cloud.h"
#include "voice_intent_mgr.h"
#include "voice_player_comm.h"
#include "voice_player/voice_player_tts.h"

typedef enum {
    AUDIO_IDLE,
    AUDIO_LOADING,
    AUDIO_PLAYING,
    AUDIO_PAUSED,
    AUDIO_ERROR
} audio_state_t;

static TaskHandle_t s_task;
static SemaphoreHandle_t s_operation;
/* This token also lets app_player abort an obsolete URL while preparing it. */
static volatile uint32_t s_generation = 1;
static char s_url[MINIAPP_AUDIO_MAX_URL_BYTES + 1];
static bool s_requested, s_paused, s_finished, s_prepared;
static audio_state_t s_state;
/* Worker-owned, protected by s_operation during lifecycle cleanup. */
static uint32_t s_playing_generation;
static bool s_started;
static TickType_t s_quiet_since;
static bool s_quiet;

static void audio_set_state(uint32_t generation, audio_state_t state)
{
    taskENTER_CRITICAL();
    if (generation == s_generation && s_requested && !s_finished) {
        s_state = state;
    }
    taskEXIT_CRITICAL();
}

static bool audio_blocked(void)
{
    return voice_cloud_is_session_active() || voice_cloud_is_uploading_audio() || voice_intent_top() > INTENT_MUSIC ||
           voice_player_tts_is_active() || alarm_ring_is_active();
}

/* With the critical section held, invalidate a not-yet-playing request. The
 * player checks this token before PREPARED can turn on the PA/start decoding.
 * Keep the URL and user pause preference: resume retries this URL, while stale
 * completion/error callbacks from the cancelled prepare cannot finish it. */
static void audio_suspend_loading(void)
{
    if (s_requested && !s_finished && !s_prepared && s_state == AUDIO_LOADING) {
        if (++s_generation == 0) {
            ++s_generation;
        }
        s_state = AUDIO_PAUSED;
    }
}

void miniapp_audio_poll(void)
{
    if (!audio_blocked()) {
        return;
    }
    taskENTER_CRITICAL();
    audio_suspend_loading();
    taskEXIT_CRITICAL();
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

/* Called only by the worker, with s_operation held. No Lua or player calls under
 * the critical section: player callbacks may execute on a different task. */
static void audio_step(void)
{
    char url[sizeof(s_url)];
    taskENTER_CRITICAL();
    uint32_t generation = s_generation;
    bool requested = s_requested, paused = s_paused, finished = s_finished;
    memcpy(url, s_url, sizeof(url));
    taskEXIT_CRITICAL();

    if (s_playing_generation != generation) {
        if (s_started) {
            /* Natural completion has already released the decoder. Stopping
             * it again reports a spurious core error during app exit. */
            app_player_state_t state = app_player_get_state(music_player);
            if (state != APP_PLAYER_STATE_IDLE && state != APP_PLAYER_STATE_STOPPED) {
                (void)app_player_stop(music_player);
            }
        }
        s_started = false;
        s_playing_generation = generation;
        s_quiet = false;
    }
    if (!requested || finished) {
        return;
    }

    bool blocked = audio_blocked();
    app_player_state_t state = app_player_get_state(music_player);
    if (paused || blocked) {
        s_quiet = false;
        if (s_started && (state == APP_PLAYER_STATE_PLAYING || state == APP_PLAYER_STATE_PREPARING ||
                          state == APP_PLAYER_STATE_PREPARED)) {
            if (app_player_pause(music_player) != APP_PLAYER_OK) {
                taskENTER_CRITICAL();
                if (generation == s_generation && s_requested && !s_finished) {
                    s_state = AUDIO_ERROR;
                    s_finished = true;
                }
                taskEXIT_CRITICAL();
                return;
            }
        }
        audio_set_state(generation, AUDIO_PAUSED);
        return;
    }
    /* Do not resume in the short gap between successive continuous sessions. */
    if (!s_quiet) {
        s_quiet = true;
        s_quiet_since = xTaskGetTickCount();
    }
    if ((TickType_t)(xTaskGetTickCount() - s_quiet_since) < pdMS_TO_TICKS(40)) {
        return;
    }

    /* Completion or a replacement may arrive while checking player state. */
    taskENTER_CRITICAL();
    bool current = generation == s_generation && s_requested && !s_finished && !s_paused;
    bool prepared = s_prepared;
    taskEXIT_CRITICAL();
    if (!current) {
        return;
    }

    int result = APP_PLAYER_OK;
    if (!s_started || (!prepared && (state == APP_PLAYER_STATE_IDLE || state == APP_PLAYER_STATE_STOPPED))) {
        app_player_play_opt_t options = {
            .url = url,
            .request_tag = generation,
            .cancel_token = &s_generation,
        };
        s_started = true;
        audio_set_state(generation, AUDIO_LOADING);
        /* A pause can arrive after the snapshot but before play_ex is entered. */
        taskENTER_CRITICAL();
        if (s_paused) {
            audio_suspend_loading();
        }
        taskEXIT_CRITICAL();
        result = app_player_play_ex(music_player, &options);
    } else if (state == APP_PLAYER_STATE_PAUSED || state == APP_PLAYER_STATE_PREPARED) {
        result = app_player_resume(music_player);
    }
    if (result != APP_PLAYER_OK) {
        taskENTER_CRITICAL();
        if (generation == s_generation && s_requested) {
            s_finished = true;
            s_state = AUDIO_ERROR;
        }
        taskEXIT_CRITICAL();
    }
}

static void audio_task(void *unused)
{
    (void)unused;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
        xSemaphoreTake(s_operation, portMAX_DELAY);
        audio_step();
        xSemaphoreGive(s_operation);
    }
}

bool miniapp_audio_play(const char *url)
{
    if (!music_player || !url || strlen(url) > MINIAPP_AUDIO_MAX_URL_BYTES) {
        goto failed;
    }
    if (!s_task) {
        s_operation = xSemaphoreCreateMutex();
        if (!s_operation) {
            goto failed;
        }
        if (xTaskCreate(audio_task, "miniapp.audio", 2048, NULL, 4, &s_task) != pdPASS) {
            vSemaphoreDelete(s_operation);
            s_operation = NULL;
            goto failed;
        }
    }
    taskENTER_CRITICAL();
    if (++s_generation == 0) {
        ++s_generation;
    }
    strcpy(s_url, url);
    s_requested = true;
    s_paused = s_finished = s_prepared = false;
    s_state = AUDIO_LOADING;
    taskEXIT_CRITICAL();
    xTaskNotifyGive(s_task);
    return true;
failed:
    taskENTER_CRITICAL();
    s_state = AUDIO_ERROR;
    taskEXIT_CRITICAL();
    return false;
}

void miniapp_audio_pause(bool paused)
{
    taskENTER_CRITICAL();
    s_paused = paused;
    if (paused) {
        audio_suspend_loading();
    }
    taskEXIT_CRITICAL();
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void miniapp_audio_stop(void)
{
    taskENTER_CRITICAL();
    if (++s_generation == 0) {
        ++s_generation;
    }
    s_requested = s_paused = s_finished = false;
    s_state = AUDIO_IDLE;
    taskEXIT_CRITICAL();
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

void miniapp_audio_release(void)
{
    miniapp_audio_stop();
    if (!s_task) {
        return;
    }
    /* Cancel without the worker lock to unblock an in-flight prepare. */
    (void)app_player_cancel_pending(music_player);
    xSemaphoreTake(s_operation, portMAX_DELAY);
    audio_step();
    xSemaphoreGive(s_operation);
}

const char *miniapp_audio_state(void)
{
    static const char *const names[] = {"idle", "loading", "playing", "paused", "error"};
    bool interrupted, requested, finished;

    taskENTER_CRITICAL();
    audio_state_t state = s_state;
    requested = s_requested;
    finished = s_finished;
    taskEXIT_CRITICAL();
    interrupted = requested && !finished && audio_blocked();
    if (interrupted) {
        return "interrupted";
    }
    return names[state];
}

bool miniapp_audio_event(app_player_t *player, app_player_event_t event)
{
    uint32_t generation = app_player_get_callback_request_tag(player);
    if (!generation) {
        return false; /* Normal system playlist. */
    }
    taskENTER_CRITICAL();
    if (generation == s_generation && s_requested && !s_finished) {
        switch (event) {
        case APP_PLAYER_EVENT_PREPARED:
            s_prepared = true;
            break;
        case APP_PLAYER_EVENT_PLAYING:
            s_prepared = true;
            s_state = AUDIO_PLAYING;
            break;
        case APP_PLAYER_EVENT_PAUSED:
            s_state = AUDIO_PAUSED;
            break;
        case APP_PLAYER_EVENT_COMPLETED:
            s_state = AUDIO_IDLE;
            s_finished = true;
            break;
        case APP_PLAYER_EVENT_STOPPED:
            s_state = AUDIO_IDLE;
            s_finished = true;
            break;
        case APP_PLAYER_EVENT_ERROR:
            s_state = AUDIO_ERROR;
            s_finished = true;
            break;
        default:
            break;
        }
    }
    taskEXIT_CRITICAL();
    return true;
}
