/****************************************************************************
 Copyright (c) 2014-2015 Chukong Technologies Inc.

 http://www.cocos2d-x.org

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 ****************************************************************************/
#if CC_TARGET_PLATFORM == CC_PLATFORM_ANDROID
#include "AudioEngine-inl.h"

#include <unistd.h>
// for native asset manager
#include <sys/types.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>

#include <unordered_map>
#include "platform/android/jni/JniHelper.h"
#include <android/log.h>
#include <jni.h>
#include "audio/include/AudioEngine.h"
#include "base/CCDirector.h"
#include "base/CCScheduler.h"
#include "platform/android/CCFileUtils-android.h"

using namespace cocos2d;
using namespace cocos2d::experimental;

#define  LOG_TAG    "cocos2d-x debug info"
#define  ALOGV(...)  __android_log_print(ANDROID_LOG_VERBOSE, LOG_TAG, __VA_ARGS__)
#define  ALOGD(...)  __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define  ALOGE(...)  __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define DELAY_TIME_TO_REMOVE (0.5f)
#define SCHEDULE_INVERVAL (0.03f)

#define SL_BREAK_IF_FAILED(r, ...) \
    if (r != SL_RESULT_SUCCESS) {\
        ALOGE(__VA_ARGS__); \
        break; \
    }

#define SL_RETURN_IF_FAILED(r, ...) \
    if (r != SL_RESULT_SUCCESS) {\
        ALOGE(__VA_ARGS__); \
        return; \
    }

#define SL_DESTROY_OBJ(OBJ)    \
    if ((OBJ) != nullptr) { \
        (*(OBJ))->Destroy(OBJ); \
        (OBJ) = nullptr; \
    }


/* used to detect errors likely to have occurred when the OpenSL ES framework fails to open
 * a resource, for instance because a file URI is invalid, or an HTTP server doesn't respond.
 */
#define PREFETCHEVENT_ERROR_CANDIDATE (SL_PREFETCHEVENT_STATUSCHANGE | SL_PREFETCHEVENT_FILLLEVELCHANGE)

// static
void AudioPlayer::playOverEvent(SLPlayItf caller, void* context, SLuint32 playEvent)
{
    if (context && playEvent == SL_PLAYEVENT_HEADATEND)
    {
        AudioPlayer* player = (AudioPlayer*)context;
        ALOGD("SL_PLAYEVENT_HEADATEND, audioId:%d", (int) player->_audioID);
        //fix issue#8965:AudioEngine can't looping audio on Android 2.3.x
        if (player->_loop)
        {
            (*(player->_fdPlayerPlay))->SetPlayState(player->_fdPlayerPlay, SL_PLAYSTATE_PLAYING);
        }
        else
        {
            player->_playOver = true;
        }
    }
}

// static
void AudioPlayer::prefetchCallback(SLPrefetchStatusItf caller, void* context, SLuint32 event)
{
    AudioPlayer* self = (AudioPlayer*) context;
    SLpermille level = 0;
    SLresult result;
    result = (*caller)->GetFillLevel(caller, &level);
    SL_RETURN_IF_FAILED(result, "GetFillLevel failed");

    SLuint32 status;
    //ALOGV("PrefetchEventCallback: received event %u", event);
    result = (*caller)->GetPrefetchStatus(caller, &status);

    SL_RETURN_IF_FAILED(result, "GetPrefetchStatus failed");

    if ((PREFETCHEVENT_ERROR_CANDIDATE == (event & PREFETCHEVENT_ERROR_CANDIDATE))
        && (level == 0) && (status == SL_PREFETCHSTATUS_UNDERFLOW))
    {
        ALOGV("PrefetchEventCallback: Error while prefetching data, exiting");
        self->_prefetchError = true;
    }
}

AudioPlayer::AudioPlayer()
    : _playOver(false)
    , _loop(false)
    , _fdPlayerPlay(nullptr)
    , _fdPlayerObject(nullptr)
    , _fdPlayerSeek(nullptr)
    , _fdPlayerVolume(nullptr)
    , _prefetchItf(nullptr)
    , _duration(AudioEngine::TIME_UNKNOWN)
    , _deltaTimeAfterPlay(0.0f)
    , _audioID(AudioEngine::INVALID_AUDIO_ID)
    , _assetFd(0)
    , _delayTimeToRemove(-1.f)
    , _prefetchError(false)
    , _finishCallback(nullptr)
{

}

AudioPlayer::~AudioPlayer()
{
    if (_fdPlayerObject)
    {
        (*_fdPlayerObject)->Destroy(_fdPlayerObject);
        _fdPlayerObject = nullptr;
        _fdPlayerPlay = nullptr;
        _fdPlayerVolume = nullptr;
        _fdPlayerSeek = nullptr;
        _prefetchItf = nullptr;
    }
    if(_assetFd > 0)
    {
        close(_assetFd);
        _assetFd = 0;
    }
}

bool AudioPlayer::init(SLEngineItf engineEngine, SLObjectItf outputMixObject,const std::string& fileFullPath, float volume, bool loop)
{
    bool ret = false;

    do
    {
        SLDataSource audioSrc;

        SLDataLocator_AndroidFD loc_fd;
        SLDataLocator_URI loc_uri;

        SLDataFormat_MIME format_mime = {SL_DATAFORMAT_MIME, NULL, SL_CONTAINERTYPE_UNSPECIFIED};
        audioSrc.pFormat = &format_mime;

        if (fileFullPath[0] != '/'){
            std::string relativePath = "";

            size_t position = fileFullPath.find("assets/");
            if (0 == position) {
                // "assets/" is at the beginning of the path and we don't want it
                relativePath += fileFullPath.substr(strlen("assets/"));
            } else {
                relativePath += fileFullPath;
            }

            auto asset = AAssetManager_open(cocos2d::FileUtilsAndroid::getAssetManager(), relativePath.c_str(), AASSET_MODE_UNKNOWN);

            // open asset as file descriptor
            off_t start, length;
            _assetFd = AAsset_openFileDescriptor(asset, &start, &length);
            if (_assetFd <= 0){
                AAsset_close(asset);
                break;
            }
            AAsset_close(asset);

            // configure audio source
            loc_fd = {SL_DATALOCATOR_ANDROIDFD, _assetFd, start, length};

            audioSrc.pLocator = &loc_fd;
        }
        else{
            loc_uri = {SL_DATALOCATOR_URI , (SLchar*)fileFullPath.c_str()};
            audioSrc.pLocator = &loc_uri;
        }

        // configure audio sink
        SLDataLocator_OutputMix loc_outmix = {SL_DATALOCATOR_OUTPUTMIX, outputMixObject};
        SLDataSink audioSnk = {&loc_outmix, NULL};

        // create audio player
        const SLInterfaceID ids[3] = {SL_IID_SEEK, SL_IID_PREFETCHSTATUS, SL_IID_VOLUME};
        const SLboolean req[3] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
        auto result = (*engineEngine)->CreateAudioPlayer(engineEngine, &_fdPlayerObject, &audioSrc, &audioSnk, 3, ids, req);
        if(SL_RESULT_SUCCESS != result){ ERRORLOG("create audio player fail"); break; }

        // realize the player
        result = (*_fdPlayerObject)->Realize(_fdPlayerObject, SL_BOOLEAN_FALSE);
        if(SL_RESULT_SUCCESS != result){ ERRORLOG("realize the player fail"); break; }

        // get the play interface
        result = (*_fdPlayerObject)->GetInterface(_fdPlayerObject, SL_IID_PLAY, &_fdPlayerPlay);
        if(SL_RESULT_SUCCESS != result){ ERRORLOG("get the play interface fail"); break; }

        // get the seek interface
        result = (*_fdPlayerObject)->GetInterface(_fdPlayerObject, SL_IID_SEEK, &_fdPlayerSeek);
        if(SL_RESULT_SUCCESS != result){ ERRORLOG("get the seek interface fail"); break; }

        // get the volume interface
        result = (*_fdPlayerObject)->GetInterface(_fdPlayerObject, SL_IID_VOLUME, &_fdPlayerVolume);
        if(SL_RESULT_SUCCESS != result){ ERRORLOG("get the volume interface fail"); break; }

        /* Get the prefetch status interface which was explicitly requested */
        result = (*_fdPlayerObject)->GetInterface(_fdPlayerObject, SL_IID_PREFETCHSTATUS, (void *) &_prefetchItf);
        if (SL_RESULT_SUCCESS != result){ ERRORLOG("GetInterface SL_IID_PREFETCHSTATUS failed"); break; }

        _loop = loop;
        if (loop){
            (*_fdPlayerSeek)->SetLoop(_fdPlayerSeek, SL_BOOLEAN_TRUE, 0, SL_TIME_UNKNOWN);
        }

        int dbVolume = 2000 * log10(volume);
        if(dbVolume < SL_MILLIBEL_MIN){
            dbVolume = SL_MILLIBEL_MIN;
        }
        (*_fdPlayerVolume)->SetVolumeLevel(_fdPlayerVolume, dbVolume);

        /* ------------------------------------------------------ */
        /* Initialize the callback for prefetch errors, if we can't open the resource to decode */
        result = (*_prefetchItf)->RegisterCallback(_prefetchItf,
                                                  AudioPlayer::prefetchCallback,
                                                  this);
        SL_BREAK_IF_FAILED(result, "prefetchItf RegisterCallback failed");

        result = (*_prefetchItf)->SetCallbackEventsMask(_prefetchItf, PREFETCHEVENT_ERROR_CANDIDATE);
        SL_BREAK_IF_FAILED(result, "prefetchItf SetCallbackEventsMask failed");

        result = (*_fdPlayerPlay)->SetPlayState(_fdPlayerPlay, SL_PLAYSTATE_PLAYING);
        if(SL_RESULT_SUCCESS != result){ ERRORLOG("SetPlayState fail"); break; }

        SLuint32 prefetchStatus = SL_PREFETCHSTATUS_UNDERFLOW;
        SLuint32 timeOutIndex = 1000; // time out prefetching after 2s
        while ((prefetchStatus != SL_PREFETCHSTATUS_SUFFICIENTDATA) && (timeOutIndex > 0) &&
               !_prefetchError)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            (*_prefetchItf)->GetPrefetchStatus(_prefetchItf, &prefetchStatus);
            timeOutIndex--;
        }

        // ALOGV("timeOutIndex: %d", (int)timeOutIndex);
        if (timeOutIndex == 0 || _prefetchError)
        {
            ALOGE("Failure to prefetch data in time, exiting");
            SL_BREAK_IF_FAILED(SL_RESULT_CONTENT_NOT_FOUND, "Failure to prefetch data in time");
        }

        ret = true;
    } while (0);

    if (!ret)
    {
        SL_DESTROY_OBJ(_fdPlayerObject);
    }

    return ret;
}

//====================================================
AudioEngineImpl::AudioEngineImpl()
    : currentAudioID(0)
    , _engineObject(nullptr)
    , _engineEngine(nullptr)
    , _outputMixObject(nullptr)
    , _lazyInitLoop(true)
{

}

AudioEngineImpl::~AudioEngineImpl()
{
    if (_outputMixObject)
    {
        (*_outputMixObject)->Destroy(_outputMixObject);
    }
    if (_engineObject)
    {
        (*_engineObject)->Destroy(_engineObject);
    }
}

bool AudioEngineImpl::init()
{
    bool ret = false;
    do{
        // create engine
        auto result = slCreateEngine(&_engineObject, 0, nullptr, 0, nullptr, nullptr);
        if(SL_RESULT_SUCCESS != result){ ERRORLOG("create opensl engine fail"); break; }

        // realize the engine
        result = (*_engineObject)->Realize(_engineObject, SL_BOOLEAN_FALSE);
        if(SL_RESULT_SUCCESS != result){ ERRORLOG("realize the engine fail"); break; }

        // get the engine interface, which is needed in order to create other objects
        result = (*_engineObject)->GetInterface(_engineObject, SL_IID_ENGINE, &_engineEngine);
        if(SL_RESULT_SUCCESS != result){ ERRORLOG("get the engine interface fail"); break; }

        // create output mix
        const SLInterfaceID outputMixIIDs[] = {};
        const SLboolean outputMixReqs[] = {};
        result = (*_engineEngine)->CreateOutputMix(_engineEngine, &_outputMixObject, 0, outputMixIIDs, outputMixReqs);
        if(SL_RESULT_SUCCESS != result){ ERRORLOG("create output mix fail"); break; }

        // realize the output mix
        result = (*_outputMixObject)->Realize(_outputMixObject, SL_BOOLEAN_FALSE);
        if(SL_RESULT_SUCCESS != result){ ERRORLOG("realize the output mix fail"); break; }

        ret = true;
    }while (false);

    return ret;
}

int AudioEngineImpl::play2d(const std::string &filePath ,bool loop ,float volume)
{
    auto audioId = AudioEngine::INVALID_AUDIO_ID;

    do
    {
        if (_engineEngine == nullptr)
            break;

        auto& player = _audioPlayers[currentAudioID];
        auto fullPath = FileUtils::getInstance()->fullPathForFilename(filePath);
        auto initPlayer = player.init(_engineEngine, _outputMixObject, fullPath, volume, loop);
        if (!initPlayer){
            _audioPlayers.erase(currentAudioID);
            log("%s,%d message:create player for %s fail", __func__, __LINE__, filePath.c_str());
            break;
        }


        SLresult r;
        r = (*(player._fdPlayerPlay))->RegisterCallback(player._fdPlayerPlay, AudioPlayer::playOverEvent, (void*)&player);
        SL_BREAK_IF_FAILED(r, "RegisterCallback error, path: %s", fullPath.c_str());

        r = (*(player._fdPlayerPlay))->SetCallbackEventsMask(player._fdPlayerPlay, SL_PLAYEVENT_HEADATEND);
        SL_BREAK_IF_FAILED(r, "SetCallbackEventsMask error, path: %s", fullPath.c_str());

        audioId = currentAudioID++;
        player._audioID = audioId;
        AudioEngine::_audioIDInfoMap[audioId].state = AudioEngine::AudioState::PLAYING;

        if (_lazyInitLoop) {
            _lazyInitLoop = false;

            auto scheduler = Director::getInstance()->getScheduler();
            scheduler->schedule(schedule_selector(AudioEngineImpl::update), this, SCHEDULE_INVERVAL, false);
        }
        // log("play2d, audioId:%d, path: %s", audioId, fullPath.c_str());
    } while (0);

    return audioId;
}

void AudioEngineImpl::update(float dt)
{
    AudioPlayer* player = nullptr;

    auto itend = _audioPlayers.end();
    for (auto iter = _audioPlayers.begin(); iter != itend; )
    {
        player = &(iter->second);

        if (!player->_loop)
        {
            if (dt < SCHEDULE_INVERVAL * 2)
            {
                float duration = getDuration(player->_audioID);
                if (duration > 0)
                {
                    player->_deltaTimeAfterPlay += dt;
                    // ALOGV("audioId:%d, duration: %f, dt: %f, _deltaTimeAfterPlay: %f", player->_audioID, duration, dt, player->_deltaTimeAfterPlay);
                    if (player->_deltaTimeAfterPlay > (duration + 1.0f))
                    {
                        ALOGV("play time is longer than audio duration, set play over flag, audioId: %d", player->_audioID);
                        player->_playOver = true;
                    }
                }
            }
            else
            {
                ALOGV("dt is too large, ignore to add it to _deltaTimeAfterPlay");
            }
        }

        if (player->_delayTimeToRemove > 0.f)
        {
            player->_delayTimeToRemove -= dt;
            if (player->_delayTimeToRemove < 0.f)
            {
                iter = _audioPlayers.erase(iter);
                continue;
            }
        }
        else if (player->_playOver)
        {
            log("_playOver, audioId:%d", player->_audioID);
            if (player->_finishCallback)
                player->_finishCallback(player->_audioID, *AudioEngine::_audioIDInfoMap[player->_audioID].filePath);

            AudioEngine::remove(player->_audioID);
            iter = _audioPlayers.erase(iter);
            continue;
        }

        ++iter;
    }

    if(_audioPlayers.empty()){
        _lazyInitLoop = true;

        auto scheduler = Director::getInstance()->getScheduler();
        scheduler->unschedule(schedule_selector(AudioEngineImpl::update), this);
    }
}

void AudioEngineImpl::setVolume(int audioID,float volume)
{
    auto& player = _audioPlayers[audioID];
    int dbVolume = 2000 * log10(volume);
    if(dbVolume < SL_MILLIBEL_MIN){
        dbVolume = SL_MILLIBEL_MIN;
    }
    auto result = (*player._fdPlayerVolume)->SetVolumeLevel(player._fdPlayerVolume, dbVolume);
    if(SL_RESULT_SUCCESS != result){
        log("%s error:%u",__func__, result);
    }
}

void AudioEngineImpl::setLoop(int audioID, bool loop)
{
    auto& player = _audioPlayers[audioID];
    player._loop = loop;
    SLboolean loopEnabled = SL_BOOLEAN_TRUE;
    if (!loop){
        loopEnabled = SL_BOOLEAN_FALSE;
    }
    (*player._fdPlayerSeek)->SetLoop(player._fdPlayerSeek, loopEnabled, 0, SL_TIME_UNKNOWN);
}

void AudioEngineImpl::pause(int audioID)
{
    auto& player = _audioPlayers[audioID];
    auto result = (*player._fdPlayerPlay)->SetPlayState(player._fdPlayerPlay, SL_PLAYSTATE_PAUSED);
    if(SL_RESULT_SUCCESS != result){
        log("%s error:%u",__func__, result);
    }
}

void AudioEngineImpl::resume(int audioID)
{
    auto& player = _audioPlayers[audioID];
    auto result = (*player._fdPlayerPlay)->SetPlayState(player._fdPlayerPlay, SL_PLAYSTATE_PLAYING);
    if(SL_RESULT_SUCCESS != result){
        log("%s error:%u",__func__, result);
    }
}

void AudioEngineImpl::stop(int audioID)
{
    log("AudioEngineImpl::stop, audioId: %d", audioID);
    auto& player = _audioPlayers[audioID];
    auto result = (*player._fdPlayerPlay)->SetPlayState(player._fdPlayerPlay, SL_PLAYSTATE_STOPPED);
    if(SL_RESULT_SUCCESS != result){
        log("%s error:%u",__func__, result);
    }

    /*If destroy openSL object immediately,it may cause dead lock.
     *It's a system issue.For more information:
     *    https://github.com/cocos2d/cocos2d-x/issues/11697
     *    https://groups.google.com/forum/#!msg/android-ndk/zANdS2n2cQI/AT6q1F3nNGIJ
     */
    player._delayTimeToRemove = DELAY_TIME_TO_REMOVE;
    //_audioPlayers.erase(audioID);
}

void AudioEngineImpl::stopAll()
{
    log("AudioEngineImpl::stopAll...");
    auto itEnd = _audioPlayers.end();
    for (auto it = _audioPlayers.begin(); it != itEnd; ++it)
    {
        (*it->second._fdPlayerPlay)->SetPlayState(it->second._fdPlayerPlay, SL_PLAYSTATE_STOPPED);
        if (it->second._delayTimeToRemove < 0.f)
        {
            //If destroy openSL object immediately,it may cause dead lock.
            it->second._delayTimeToRemove = DELAY_TIME_TO_REMOVE;
        }
    }
}

float AudioEngineImpl::getDuration(int audioID)
{
    auto& player = _audioPlayers[audioID];
    if (player._duration > 0)
        return player._duration;

    SLmillisecond duration;
    auto result = (*player._fdPlayerPlay)->GetDuration(player._fdPlayerPlay, &duration);
    if (duration == SL_TIME_UNKNOWN){
        return AudioEngine::TIME_UNKNOWN;
    }
    else{
        player._duration = duration / 1000.0;

        if (player._duration <= 0)
        {
            return AudioEngine::TIME_UNKNOWN;
        }

        return player._duration;
    }
}

float AudioEngineImpl::getCurrentTime(int audioID)
{
    SLmillisecond currPos;
    auto& player = _audioPlayers[audioID];
    (*player._fdPlayerPlay)->GetPosition(player._fdPlayerPlay, &currPos);
    return currPos / 1000.0f;
}

bool AudioEngineImpl::setCurrentTime(int audioID, float time)
{
    auto& player = _audioPlayers[audioID];
    SLmillisecond pos = 1000 * time;
    auto result = (*player._fdPlayerSeek)->SetPosition(player._fdPlayerSeek, pos, SL_SEEKMODE_ACCURATE);
    if(SL_RESULT_SUCCESS != result){
        return false;
    }
    return true;
}

void AudioEngineImpl::setFinishCallback(int audioID, const std::function<void (int, const std::string &)> &callback)
{
    _audioPlayers[audioID]._finishCallback = callback;
}

void AudioEngineImpl::preload(const std::string& filePath, std::function<void(bool)> callback)
{
    log("Preload isn't supported on Android!");
    if (callback)
    {
        callback(true);
    }
}

#endif

