#pragma once

#include <stdbool.h>
#include "app_player.h"

#define MINIAPP_AUDIO_MAX_URL_BYTES 511u

#ifdef CONFIG_MINIAPP
/* Commands are asynchronous. Only the Lua task submits commands/releases ownership. */
bool miniapp_audio_play(const char *url);
void miniapp_audio_pause(bool paused);
void miniapp_audio_stop(void);
/* Called by the Lua task even while the audio worker is preparing a URL. */
void miniapp_audio_poll(void);
/* Drain playback before leaving/replacing the VM and returning MUSIC to the system. */
void miniapp_audio_release(void);
const char *miniapp_audio_state(void);
/* Consume tagged MUSIC events, including late events from a retired instance. */
bool miniapp_audio_event(app_player_t *player, app_player_event_t event);
#else
static inline bool miniapp_audio_event(app_player_t *p, app_player_event_t e)
{
    (void)p;
    (void)e;
    return false;
}
#endif
