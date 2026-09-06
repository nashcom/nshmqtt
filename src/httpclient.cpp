// Copyright (c) 2026 Daniel Nashed / NashCom
// SPDX-License-Identifier: Apache-2.0

#include "httpclient.h"

#include <curl/curl.h>

namespace nshmqtt
{

namespace
{

// RAII around a CURL* easy handle -- guarantees curl_easy_cleanup() runs on
// every return path (early error, exception, normal completion) without
// every caller having to remember it.
class CurlHandle
{
public:
    CurlHandle() : handle_(curl_easy_init()) {}
    ~CurlHandle()
    {
        if (handle_ != nullptr)
        {
            curl_easy_cleanup(handle_);
        }
    }
    CurlHandle(const CurlHandle &) = delete;
    CurlHandle &operator=(const CurlHandle &) = delete;

    CURL *get() const
    {
        return handle_;
    }
    explicit operator bool() const
    {
        return handle_ != nullptr;
    }

private:
    CURL *handle_;
};

// Same RAII idea for the header list curl_slist_append() builds up --
// curl_slist_free_all() must run exactly once, regardless of how the
// function returns.
class CurlHeaderList
{
public:
    void append(const std::string &line)
    {
        list_ = curl_slist_append(list_, line.c_str());
    }
    ~CurlHeaderList()
    {
        if (list_ != nullptr)
        {
            curl_slist_free_all(list_);
        }
    }
    CurlHeaderList() = default;
    CurlHeaderList(const CurlHeaderList &) = delete;
    CurlHeaderList &operator=(const CurlHeaderList &) = delete;

    curl_slist *get() const
    {
        return list_;
    }

private:
    curl_slist *list_ = nullptr;
};

} // namespace

HttpResult HttpClient::post(const std::string &url, const std::string &body,
                            const std::vector<std::pair<std::string, std::string>> &headers, int timeout_seconds,
                            bool tls_insecure)
{
    HttpResult result;

    CurlHandle curl;
    if (!curl)
    {
        result.error = "curl_easy_init() failed";
        return result;
    }

    CurlHeaderList header_list;
    header_list.append("Content-Type: application/json");
    for (const auto &h : headers)
    {
        header_list.append(h.first + ": " + h.second);
    }

    char error_buf[CURL_ERROR_SIZE] = {0};

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POST, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, header_list.get());
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds));
    // Required for safe use from a background worker thread: libcurl's own
    // documentation warns that without this, a timeout can be implemented
    // internally with signals that are not thread-safe to deliver to an
    // arbitrary thread.
    curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error_buf);
    // No response body is ever consulted (only the status code), but
    // libcurl writes it to stdout by default if not redirected somewhere
    // -- discard it via the simplest possible no-op write callback instead
    // of pulling in a full response-capture path this code never uses.
    curl_easy_setopt(
        curl.get(), CURLOPT_WRITEFUNCTION,
        +[](char *, size_t size, size_t nmemb, void *) -> size_t { return size * nmemb; });

    if (tls_insecure)
    {
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl.get(), CURLOPT_SSL_VERIFYHOST, 0L);
    }

    CURLcode rc = curl_easy_perform(curl.get());
    if (rc != CURLE_OK)
    {
        result.error = (error_buf[0] != '\0') ? error_buf : curl_easy_strerror(rc);
        return result;
    }

    long status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status);
    result.status_code = status;
    result.ok = (status >= 200 && status < 300);
    if (!result.ok)
    {
        result.error = "unexpected HTTP status " + std::to_string(status);
    }
    return result;
}

} // namespace nshmqtt
