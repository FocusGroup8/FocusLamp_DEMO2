#ifndef AUDIO_DRIVER_CONFIG_H
#define AUDIO_DRIVER_CONFIG_H

#include "esp_err.h"
#include "pin_config.h"

#define AUDIO_DRIVER_ENABLE 1

#if (AUDIO_DRIVER_ENABLE == 1)

/* Audio driver hardware configuration (uses pin_config.h definitions) */
#define AUDIO_DRIVER_SAMPLE_RATE       48000
#define AUDIO_DRIVER_I2S_NUM           1
#define AUDIO_DRIVER_BCLK_GPIO         AUD_BCLK_GPIO   /* GPIO32 */
#define AUDIO_DRIVER_WS_GPIO           AUD_LRC_GPIO    /* GPIO33 */
#define AUDIO_DRIVER_DOUT_GPIO         AUD_SD_GPIO     /* GPIO30 */
#define AUDIO_DRIVER_DIN_GPIO          AUD_DIN_GPIO    /* GPIO31 */
#define AUDIO_DRIVER_DEFAULT_VOLUME    (0.50f)
#define AUDIO_DRIVER_DEFAULT_GAIN      (1.0f)
#define AUDIO_DRIVER_NOISE_THRESHOLD   1000
#define AUDIO_DRIVER_ENABLE_NOISE_REDUCTION 0

#endif /* AUDIO_DRIVER_ENABLE */

#endif /* AUDIO_DRIVER_CONFIG_H */
