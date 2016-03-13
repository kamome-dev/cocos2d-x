/****************************************************************************
 Copyright (c) 2015 Chukong Technologies Inc.

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

#include "network/CCDownloaderImpl.h"

#include <curl/curl.h>

#include "platform/CCFileUtils.h"
#include "deprecated/CCString.h"

USING_NS_CC;
using namespace cocos2d::network;

static const long LOW_SPEED_LIMIT = 1;
static const long LOW_SPEED_TIME = 5;
static const int DEFAULT_TIMEOUT = 5;
static const int MAX_REDIRS = 2;
static const int MAX_WAIT_MSECS = 30*1000; /* Wait max. 30 seconds */

static const int HTTP_CODE_SUPPORT_RESUME = 206;
static const char* TEMP_EXT = ".temp";

static size_t _fileWriteFunc(void *ptr, size_t size, size_t nmemb, void* userdata)
{
    DownloadUnit *unit = (DownloadUnit*)userdata;
    DownloaderImpl* this_ = (DownloaderImpl*)unit->_reserved;
    int ret = this_->getWriterCallback()(ptr, size, nmemb, unit);
    return ret;
}
static size_t _fileWriteFuncForAdapter(void *ptr, size_t size, size_t nmemb, void* userdata)
{
    return nmemb;
}
static int _downloadProgressFunc(void* userdata, double totalToDownload, double nowDownloaded, double totalToUpLoad, double nowUpLoaded)
{
    DownloadUnit *downloadUnit = (DownloadUnit*)userdata;
    DownloaderImpl* this_ = (DownloaderImpl*)downloadUnit->_reserved;

    this_->getProgressCallback()(downloadUnit, totalToDownload, nowDownloaded);

    // must return 0, otherwise download will get cancelled
    return 0;
}

DownloaderImpl::DownloaderImpl()
: IDownloaderImpl()
, _curlHandle(nullptr)
, _lastErrCode(CURLE_OK)
, _connectionTimeout(DEFAULT_TIMEOUT)
, _initialized(false)
{
}
DownloaderImpl::~DownloaderImpl()
{
    if (_curlHandle)
        curl_easy_cleanup(_curlHandle);
}

bool DownloaderImpl::init()
{
    if (!_initialized) {
        _curlHandle = curl_easy_init();
        curl_easy_setopt(_curlHandle, CURLOPT_SSL_VERIFYPEER, 0L);

        _initialized = true;
    }

    return _initialized;
}

std::string DownloaderImpl::getStrError() const
{
    return curl_easy_strerror((CURLcode)_lastErrCode);
}

int DownloaderImpl::performDownload(DownloadUnit* unit,
                                    const WriterCallback& writerCallback,
                                    const ProgressCallback& progressCallback
                                    )
{
    CC_ASSERT(_initialized && "must be initialized");

    // for callbacks
    unit->_reserved = this;

    curl_easy_setopt(_curlHandle, CURLOPT_URL, unit->srcUrl.c_str());
	curl_easy_setopt(_curlHandle, CURLOPT_SSL_VERIFYPEER, 0L);

    // Download pacakge
    curl_easy_setopt(_curlHandle, CURLOPT_WRITEFUNCTION, _fileWriteFunc);
    curl_easy_setopt(_curlHandle, CURLOPT_WRITEDATA, unit);

    curl_easy_setopt(_curlHandle, CURLOPT_NOPROGRESS, false);
    curl_easy_setopt(_curlHandle, CURLOPT_PROGRESSFUNCTION, _downloadProgressFunc);
    curl_easy_setopt(_curlHandle, CURLOPT_PROGRESSDATA, unit);

    curl_easy_setopt(_curlHandle, CURLOPT_FAILONERROR, true);
    if (_connectionTimeout)
        curl_easy_setopt(_curlHandle, CURLOPT_CONNECTTIMEOUT, _connectionTimeout);
    curl_easy_setopt(_curlHandle, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(_curlHandle, CURLOPT_LOW_SPEED_LIMIT, LOW_SPEED_LIMIT);
    curl_easy_setopt(_curlHandle, CURLOPT_LOW_SPEED_TIME, LOW_SPEED_TIME);

    _writerCallback = writerCallback;
    _progressCallback = progressCallback;
    _lastErrCode = curl_easy_perform(_curlHandle);
    return _lastErrCode;
}

int DownloaderImpl::performBatchDownload(const DownloadUnits& units,
                                         const WriterCallback& batchWriterCallback,
                                         const ProgressCallback& batchProgressCallback,
                                         const ErrorCallback& errorCallback)
{
    CC_ASSERT(_initialized && "must be initialized");

    if (units.size() == 0)
        return -1;

    CURLM* multi_handle = curl_multi_init();
    int still_running = 0;

    bool supportResume = supportsResume(units.cbegin()->second.srcUrl);
    auto fileUtils = FileUtils::getInstance();

    _writerCallback = batchWriterCallback;
    _progressCallback = batchProgressCallback;

    std::vector<CURL*> curls;
    curls.reserve(units.size());

    for (const auto& unitEntry: units)
    {
        const auto& unit = unitEntry.second;

        // HACK: Needed for callbacks. "this" + "unit" are needed
        unit._reserved = this;

        if (unit.fp != NULL)
        {
            CURL* curl = curl_easy_init();

            curl_easy_setopt(curl, CURLOPT_URL, unit.srcUrl.c_str());
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, _fileWriteFunc);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &unit);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, false);
            curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, _downloadProgressFunc);
            curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, &unit);
            curl_easy_setopt(curl, CURLOPT_FAILONERROR, true);
            if (_connectionTimeout)
                curl_easy_setopt(_curlHandle, CURLOPT_CONNECTTIMEOUT, _connectionTimeout);
            curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, LOW_SPEED_LIMIT);
            curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, LOW_SPEED_TIME);
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, true);
            curl_easy_setopt(curl, CURLOPT_MAXREDIRS, MAX_REDIRS);

            // Resuming download support
            if (supportResume && unit.resumeDownload)
            {
                // Check already downloaded size for current download unit
                long size = fileUtils->getFileSize(unit.storagePath + TEMP_EXT);
                if (size != -1)
                {
                    curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE, size);
                }
            }

            CURLMcode code = curl_multi_add_handle(multi_handle, curl);
            if (code != CURLM_OK)
            {
                errorCallback(StringUtils::format("Unable to add curl handler for %s: [curl error]%s", unit.customId.c_str(), curl_multi_strerror(code)),
                              code,
                              unit.customId);
                curl_easy_cleanup(curl);
            }
            else
            {
                curls.push_back(curl);
            }
        }
    }

    // Query multi perform
    CURLMcode curlm_code = CURLM_CALL_MULTI_PERFORM;
    while(CURLM_CALL_MULTI_PERFORM == curlm_code) {
        curlm_code = curl_multi_perform(multi_handle, &still_running);
    }
    if (curlm_code != CURLM_OK) {
        errorCallback(StringUtils::format("Unable to continue the download process: [curl error]%s", curl_multi_strerror(curlm_code)),
                      curlm_code,
                      "");
    }
    else
    {
        bool failed = false;
        while (still_running > 0 && !failed)
        {
            // set a suitable timeout to play around with
            struct timeval select_tv;
            long curl_timeo = -1;
            select_tv.tv_sec = 1;
            select_tv.tv_usec = 0;

            curl_multi_timeout(multi_handle, &curl_timeo);
            if(curl_timeo >= 0) {
                select_tv.tv_sec = curl_timeo / 1000;
                if(select_tv.tv_sec > 1)
                    select_tv.tv_sec = 1;
                else
                    select_tv.tv_usec = (curl_timeo % 1000) * 1000;
            }

            int rc;
            fd_set fdread;
            fd_set fdwrite;
            fd_set fdexcep;
            int maxfd = -1;
            FD_ZERO(&fdread);
            FD_ZERO(&fdwrite);
            FD_ZERO(&fdexcep);
            // FIXME: when jenkins migrate to ubuntu, we should remove this hack code
#if (CC_TARGET_PLATFORM == CC_PLATFORM_LINUX)
            curl_multi_fdset(multi_handle, &fdread, &fdwrite, &fdexcep, &maxfd);
            rc = select(maxfd + 1, &fdread, &fdwrite, &fdexcep, &select_tv);
#else
            rc = curl_multi_wait(multi_handle,nullptr, 0, MAX_WAIT_MSECS, &maxfd);
#endif

            switch(rc)
            {
                case -1:
                    failed = true;
                    break;
                case 0:
                default:
                    curlm_code = CURLM_CALL_MULTI_PERFORM;
                    while(CURLM_CALL_MULTI_PERFORM == curlm_code) {
                        curlm_code = curl_multi_perform(multi_handle, &still_running);
                    }
                    if (curlm_code != CURLM_OK) {
                        errorCallback(StringUtils::format("Unable to continue the download process: [curl error]%s", curl_multi_strerror(curlm_code)),
                                      curlm_code,
                                      "");
                    }
                    break;
            }
        }
    }

    // Clean up and close files
    for (auto& curl: curls)
    {
#if COCOS2D_DEBUG >= 1        //失敗してないかチェック。
#if 0
        //最終的にはmd5をチェックすべき？（速度の面の心配がある）
        long s_long;
        char* s_pchar;
        curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&s_long );
        long s_code = s_long;
        if( s_code!=200 ) {
			#define CInfo_long( stat ) if(curl_easy_getinfo(curl,stat,&s_long )==CURLE_OK){CCLOG( "  " #stat ":%ld",s_long );}
			#define CInfo_string( stat ) if(curl_easy_getinfo(curl,stat,&s_pchar )==CURLE_OK && s_pchar!=nullptr ){CCLOG( "  " #stat ":%s",s_pchar );}
            CCLOG( "FAIL:%ld",s_code );
			CInfo_string( CURLINFO_EFFECTIVE_URL );
//			CInfo_long( CURLINFO_HTTP_CONNECTCODE );
//			CInfo_long( CURLINFO_FILETIME );
//			CInfo_long( CURLINFO_TOTAL_TIME );
//			CInfo_long( CURLINFO_NAMELOOKUP_TIME );
//			CInfo_long( CURLINFO_CONNECT_TIME );
//			CInfo_long( CURLINFO_APPCONNECT_TIME );
//			CInfo_long( CURLINFO_PRETRANSFER_TIME );
//			CInfo_long( CURLINFO_STARTTRANSFER_TIME );
//			CInfo_long( CURLINFO_REDIRECT_TIME );
//			CInfo_long( CURLINFO_REDIRECT_COUNT );
//			CInfo_long( CURLINFO_REDIRECT_COUNT );
//			CInfo_string( CURLINFO_REDIRECT_URL );
//			CInfo_long( CURLINFO_SIZE_UPLOAD );
//			CInfo_long( CURLINFO_SIZE_DOWNLOAD );
//			CInfo_long( CURLINFO_SPEED_DOWNLOAD );
//			CInfo_long( CURLINFO_SPEED_UPLOAD );
//			CInfo_long( CURLINFO_HEADER_SIZE );
//			CInfo_long( CURLINFO_REQUEST_SIZE );
//			CInfo_long( CURLINFO_SSL_VERIFYRESULT );
//			CInfo_long( CURLINFO_SSL_ENGINES );
//			CInfo_long( CURLINFO_CONTENT_LENGTH_DOWNLOAD );
//			CInfo_long( CURLINFO_CONTENT_LENGTH_UPLOAD );
//			CInfo_long( CURLINFO_CONTENT_TYPE );
//			CInfo_string( CURLINFO_PRIVATE );
//			CInfo_long( CURLINFO_HTTPAUTH_AVAIL );
//			CInfo_long( CURLINFO_PROXYAUTH_AVAIL );
//			CInfo_long( CURLINFO_OS_ERRNO );
//			CInfo_long( CURLINFO_NUM_CONNECTS );
//			CInfo_string( CURLINFO_PRIMARY_IP );
//			CInfo_long( CURLINFO_PRIMARY_PORT );
//			CInfo_string( CURLINFO_LOCAL_IP );
//			CInfo_long( CURLINFO_LOCAL_PORT );
//			CInfo_string( CURLINFO_COOKIELIST );
//            CInfo_long( CURLINFO_LASTSOCKET );
//			CInfo_string( CURLINFO_FTP_ENTRY_PATH );
        }
#endif
#endif
        curl_multi_remove_handle(multi_handle, curl);
        curl_easy_cleanup(curl);
    }
    curl_multi_cleanup(multi_handle);

    return 0;
}


int DownloaderImpl::getHeader(const std::string& url, HeaderInfo* headerInfo)
{

    void *curlHandle = curl_easy_init();
    CC_ASSERT(headerInfo && "headerInfo must not be null");
    
    curl_easy_setopt(curlHandle, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curlHandle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curlHandle, CURLOPT_HEADER, 1);
    curl_easy_setopt(curlHandle, CURLOPT_NOBODY, 1);
    curl_easy_setopt(curlHandle, CURLOPT_NOSIGNAL, 1);
    // in win32 platform, if not set the writeFunction, it will return CURLE_WRITE_ERROR
    curl_easy_setopt(curlHandle, CURLOPT_WRITEFUNCTION, _fileWriteFuncForAdapter);
    if ((_lastErrCode=curl_easy_perform(curlHandle)) == CURLE_OK)
    {
        char *effectiveUrl;
        char *contentType;
        curl_easy_getinfo(curlHandle, CURLINFO_EFFECTIVE_URL, &effectiveUrl);
        curl_easy_getinfo(curlHandle, CURLINFO_CONTENT_TYPE, &contentType);
        curl_easy_getinfo(curlHandle, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &headerInfo->contentSize);
        curl_easy_getinfo(curlHandle, CURLINFO_RESPONSE_CODE, &headerInfo->responseCode);

        if (contentType == nullptr || headerInfo->contentSize == -1 || headerInfo->responseCode >= 400)
        {
            headerInfo->valid = false;
        }
        else
        {
            headerInfo->url = effectiveUrl;
            headerInfo->contentType = contentType;
            headerInfo->valid = true;
        }

        curl_easy_cleanup(curlHandle);
        return 0;
    }

    curl_easy_cleanup(curlHandle);
    return -1;
}

bool DownloaderImpl::supportsResume(const std::string& url)
{
    CC_ASSERT(_initialized && "must be initialized");

    HeaderInfo headerInfo;
    // Make a resume request

    curl_easy_setopt(_curlHandle, CURLOPT_RESUME_FROM_LARGE, 0);
    if (getHeader(url, &headerInfo) == 0 && headerInfo.valid)
    {
        return (headerInfo.responseCode == HTTP_CODE_SUPPORT_RESUME);
    }
    return false;
}

void DownloaderImpl::setConnectionTimeout(int connectionTimeout)
{
    _connectionTimeout = connectionTimeout;
}
