/*
 * Copyright 2022 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
/*
 * Copyright (c) 2022 Qualcomm Innovation Center, Inc. All rights reserved.

    * Redistribution and use in source and binary forms, with or without
      modification, are permitted (subject to the limitations in the
      disclaimer below) provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    * Redistributions in binary form must reproduce the above
      copyright notice, this list of conditions and the following
      disclaimer in the documentation and/or other materials provided
      with the distribution.

    * Neither the name of Qualcomm Innovation Center, Inc. nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

NO EXPRESS OR IMPLIED LICENSES TO ANY PARTY'S PATENT RIGHTS ARE
GRANTED BY THIS LICENSE. THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT
HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR
ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE
GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE
*/

#pragma once

#include <vector>

#include "a2dp_codec_api.h"
#include "audio_aidl_interfaces.h"

namespace bluetooth {
namespace audio {
namespace aidl {
namespace codec {

using ::aidl::android::hardware::bluetooth::audio::ChannelMode;
using ::aidl::android::hardware::bluetooth::audio::CodecConfiguration;
using ::aidl::android::hardware::bluetooth::audio::PcmConfiguration;


extern const CodecConfiguration kInvalidCodecConfiguration;

int32_t A2dpCodecToHalSampleRate(
    const btav_a2dp_codec_config_t& a2dp_codec_config);
int8_t A2dpCodecToHalBitsPerSample(
    const btav_a2dp_codec_config_t& a2dp_codec_config);
ChannelMode A2dpCodecToHalChannelMode(
    const btav_a2dp_codec_config_t& a2dp_codec_config);

bool A2dpSbcToHalConfig(CodecConfiguration* codec_config,
                        A2dpCodecConfig* a2dp_config);
bool A2dpAacToHalConfig(CodecConfiguration* codec_config,
                        A2dpCodecConfig* a2dp_config);
bool A2dpAptxToHalConfig(CodecConfiguration* codec_config,
                         A2dpCodecConfig* a2dp_config);
bool A2dpLdacToHalConfig(CodecConfiguration* codec_config,
                         A2dpCodecConfig* a2dp_config);
bool A2dpAptxAdaptiveToHalConfig(CodecConfiguration* codec_config,
                         A2dpCodecConfig* a2dp_config);
// Savitech Patch - START  Offload
bool A2dpLhdcv5ToHalConfig(CodecConfiguration* codec_config,
                           A2dpCodecConfig* a2dp_config);
// Savitech Patch - END


bool a2dp_is_audio_codec_config_params_changed_aidl(
                       CodecConfiguration* codec_config, A2dpCodecConfig* a2dp_config);
bool a2dp_is_audio_pcm_config_params_changed_aidl(
                       PcmConfiguration* pcm_config, A2dpCodecConfig* a2dp_config);

bool UpdateOffloadingCapabilities(
    const std::vector<btav_a2dp_codec_config_t>& framework_preference);

/***
 * Check whether this codec is supported by the audio HAL and is allowed to use
 * by prefernece of framework / Bluetooth SoC / runtime property.
 ***/
bool IsCodecOffloadingEnabled(const CodecConfiguration& codec_config);

}  // namespace codec
}  // namespace aidl
}  // namespace audio
}  // namespace bluetooth
