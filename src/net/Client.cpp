/* UlordRig
 * Copyright 2010      Jeff Garzik <jgarzik@pobox.com>
 * Copyright 2012-2014 pooler      <pooler@litecoinpool.org>
 * Copyright 2014      Lucas Jones <https://github.com/lucasjones>
 * Copyright 2014-2016 Wolf9466    <https://github.com/OhGodAPet>
 * Copyright 2016      Jay D Dee   <jayddee246@gmail.com>
 * Copyright 2017-2018 XMR-Stak    <https://github.com/fireice-uk>, <https://github.com/psychocrypt>
 * Copyright 2016-2018 XMRig       <https://github.com/xmrig>, <support@xmrig.com>
 * Copyright 2018      UlordRig    <https://github.com/UlordChain/ulordrig>
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <assert.h>
#include <inttypes.h>
#include <iterator>
#include <stdio.h>
#include <string.h>
#include <utility>


#include "interfaces/IClientListener.h"
#include "log/Log.h"
#include "net/Client.h"
#include "net/Url.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"



#ifdef ULORDRIG_PROXY_PROJECT
#   include "proxy/JobResult.h"
#else
#   include "net/JobResult.h"
#endif


#ifdef _MSC_VER
#   define strncasecmp(x,y,z) _strnicmp(x,y,z)
#endif


int64_t Client::m_sequence = 1;


Client::Client(int id, const char *agent, IClientListener *listener) :
    m_ipv6(false),
    m_nicehash(false),
    m_quiet(false),
    m_agent(agent),
    m_listener(listener),
    m_id(id),
    m_retryPause(5000),
    m_failures(0),
    m_recvBufPos(0),
    m_state(UnconnectedState),
    m_expire(0),
    m_jobs(0),
    m_stream(nullptr),
    m_socket(nullptr),
    m_isDormancy(false)
{
    memset(m_ip, 0, sizeof(m_ip));
    memset(&m_hints, 0, sizeof(m_hints));

    m_resolver.data = this;

    m_hints.ai_family   = AF_UNSPEC;
    m_hints.ai_socktype = SOCK_STREAM;
    m_hints.ai_protocol = IPPROTO_TCP;

    m_recvBuf.base = m_buf;
    m_recvBuf.len  = sizeof(m_buf);

#   ifndef ULORDRIG_PROXY_PROJECT
    m_keepAliveTimer.data = this;
    uv_timer_init(uv_default_loop(), &m_keepAliveTimer);
#   endif
}


Client::~Client()
{
    delete m_socket;
}


void Client::connect()
{
    resolve(m_url.host());
}


/**
 * @brief Connect to server.
 *
 * @param url
 */
void Client::connect(const Url *url)
{
    setUrl(url);
    resolve(m_url.host());
}


void Client::deleteLater()
{
    if (!m_listener) {
        return;
    }

    m_listener = nullptr;

    if (!disconnect()) {
        delete this;
    }
}


void Client::setUrl(const Url *url)
{
    if (!url || !url->isValid()) {
        return;
    }

    m_url = url;
}


void Client::tick(uint64_t now)
{
    if (m_expire == 0 || now < m_expire) {
        return;
    }

    if (m_state == ConnectedState) {
        LOG_DEBUG_ERR("[%s:%u] timeout", m_url.host(), m_url.port());
        close();
    }


    if (m_state == ConnectingState) {
        connect();
    }
}


bool Client::disconnect()
{
#   ifndef ULORDRIG_PROXY_PROJECT
    uv_timer_stop(&m_keepAliveTimer);
#   endif

    m_expire   = 0;
    m_failures = -1;

    return close();
}


int64_t Client::submit(const JobResult &result)
{
#   ifdef ULORDRIG_PROXY_PROJECT
    const char *nonce = result.nonce;
    const char *data  = result.result;
#   else
    char nonce[9];
    char data[65];

    Job::toHex(reinterpret_cast<const unsigned char*>(&result.nonce), 4, nonce);
    nonce[8] = '\0';

    Job::toHex(result.result, 32, data);
    data[64] = '\0';
#   endif

    const size_t size = snprintf(m_sendBuf, sizeof(m_sendBuf), "{\"id\":%" PRIu64 ",\"jsonrpc\":\"2.0\",\"method\":\"submit\",\"params\":{\"id\":\"%s\",\"job_id\":\"%s\",\"nonce\":\"%s\",\"result\":\"%s\"}}\n",
                                 m_sequence, m_rpcId.data(), result.jobId.data(), nonce, data);

#   ifdef ULORDRIG_PROXY_PROJECT
    m_results[m_sequence] = SubmitResult(m_sequence, result.diff, result.actualDiff(), result.id);
#   else
    m_results[m_sequence] = SubmitResult(m_sequence, result.diff, result.actualDiff());
#   endif



    return send(size);
}


bool Client::close()
{
    if (m_state == UnconnectedState || m_state == ClosingState) {
        return false;
    }

    if (!m_socket){
        setState(UnconnectedState);
        return false;
    }

    setState(ClosingState);

    uv_stream_t *stream = reinterpret_cast<uv_stream_t*>(m_socket);

    if (uv_is_readable(stream) == 1) {
        uv_read_stop(stream);
    }

    if (uv_is_writable(stream) == 1) {
        const int rc = uv_shutdown(new uv_shutdown_t, stream, [](uv_shutdown_t* req, int status) {
            if (uv_is_closing(reinterpret_cast<uv_handle_t*>(req->handle)) == 0) {
                uv_close(reinterpret_cast<uv_handle_t*>(req->handle), Client::onClose);
            }

            delete req;
        });
        
        assert(rc == 0);

        if (rc != 0) {
            onClose();
        }
    }
    else {
        uv_close(reinterpret_cast<uv_handle_t*>(m_socket), Client::onClose);
    }

    return true;
}


bool Client::isCriticalError(const char *message)
{
    if (!message) {
        return false;
    }

    if (strncasecmp(message, "Unauthenticated", 15) == 0) {
        return true;
    }

    if (strncasecmp(message, "your IP is banned", 17) == 0) {
        return true;
    }

    if (strncasecmp(message, "IP Address currently banned", 27) == 0) {
        return true;
    }

    return false;
}


bool Client::parseJob(const rapidjson::Value &params, int *code)
{
    if (!params.IsObject()) {
        *code = 2;
        return false;
    }

#   ifdef ULORDRIG_PROXY_PROJECT
    Job job(m_id, m_url.variant());
    job.setClientId(m_rpcId);
    job.setCoin(m_url.coin());
#   else
    Job job(m_id, m_nicehash, m_url.algo(), m_url.variant());
#   endif

    if (!job.setId(params["job_id"].GetString())) {
        *code = 3;
        return false;
    }

    if (!job.setBlob(params["blob"].GetString())) {
        *code = 4;
        return false;
    }

    if (!job.setTarget(params["target"].GetString())) {
        *code = 5;
        return false;
    }

    if (params.HasMember("coin")) {
        job.setCoin(params["coin"].GetString());
    }

    if (params.HasMember("variant")) {
        job.setVariant(params["variant"].GetInt());
    }

    if (m_job != job) {
        m_jobs++;
        m_job = std::move(job);
        return true;
    }

    if (m_jobs == 0) { 
        return false;
    }

    if (!m_quiet) {
        LOG_WARN("[%s:%u] duplicate job received, reconnect", m_url.host(), m_url.port());
    }

    close();
    return false;
}


bool Client::parseLogin(const rapidjson::Value &result, int *code)
{
    if (!m_rpcId.setId(result["id"].GetString())) {
        *code = 1;
        return false;
    }

#   ifndef ULORDRIG_PROXY_PROJECT
    m_nicehash = m_url.isNicehash();
#   endif

    if (result.HasMember("extensions")) {
        parseExtensions(result["extensions"]);
    }

    const bool rc = parseJob(result["job"], code);
    m_jobs = 0;

    return rc;
}


int Client::resolve(const char *host)
{
    setState(HostLookupState);

    m_expire     = 0;
    m_recvBufPos = 0;

    if (m_failures == -1) {
        m_failures = 0;
    }
    const int r = uv_getaddrinfo(uv_default_loop(), &m_resolver, Client::onResolved, host, nullptr, &m_hints);
    if (r) {
        if (!m_quiet) {
            LOG_ERR("[%s:%u] getaddrinfo error: \"%s\"", host, m_url.port(), uv_strerror(r));
        }
        return 1;
    }

    return 0;
}


int64_t Client::send(size_t size)
{
    LOG_DEBUG("[%s:%u] send (%d bytes): \"%s\"", m_url.host(), m_url.port(), size, m_sendBuf);
    if (state() != ConnectedState || !uv_is_writable(m_stream)) {
        LOG_DEBUG_ERR("[%s:%u] send failed, invalid state: %d", m_url.host(), m_url.port(), m_state);
        return -1;
    }

    uv_buf_t buf = uv_buf_init(m_sendBuf, (unsigned int) size);

    if (uv_try_write(m_stream, &buf, 1) < 0) {
        close();
        return -1;
    }

    m_expire = uv_now(uv_default_loop()) + kResponseTimeout;
    return m_sequence++;
}


void Client::connect(const std::vector<addrinfo*> &ipv4, const std::vector<addrinfo*> &ipv6)
{
    addrinfo *addr = nullptr;
    m_ipv6         = ipv4.empty() && !ipv6.empty();

    if (m_ipv6) {
        addr = ipv6[ipv6.size() == 1 ? 0 : rand() % ipv6.size()];
        uv_ip6_name(reinterpret_cast<sockaddr_in6*>(addr->ai_addr), m_ip, 45);
    }
    else {
        addr = ipv4[ipv4.size() == 1 ? 0 : rand() % ipv4.size()];
        uv_ip4_name(reinterpret_cast<sockaddr_in*>(addr->ai_addr), m_ip, 16);
    }

    connect(addr->ai_addr);
}


void Client::connect(sockaddr *addr)
{
    setState(ConnectingState);

    reinterpret_cast<struct sockaddr_in*>(addr)->sin_port = htons(m_url.port());
    delete m_socket;

    uv_connect_t *req = new uv_connect_t;
    req->data = this;

    m_socket = new uv_tcp_t;
    m_socket->data = this;

    uv_tcp_init(uv_default_loop(), m_socket);
    uv_tcp_nodelay(m_socket, 1);

#   ifndef WIN32
    uv_tcp_keepalive(m_socket, 1, 60);
#   endif

    uv_tcp_connect(req, m_socket, reinterpret_cast<const sockaddr*>(addr), Client::onConnect);
}


void Client::login()
{
    m_results.clear();

    rapidjson::Document doc;
    doc.SetObject();

    auto &allocator = doc.GetAllocator();

    doc.AddMember("id",      1,       allocator);
    doc.AddMember("jsonrpc", "2.0",   allocator);
    doc.AddMember("method",  "login", allocator);

    rapidjson::Value params(rapidjson::kObjectType);
    params.AddMember("login", rapidjson::StringRef(m_url.user()),     allocator);
    params.AddMember("pass",  rapidjson::StringRef(m_url.password()), allocator);
    params.AddMember("agent", rapidjson::StringRef(m_agent),          allocator);

    doc.AddMember("params", params, allocator);

    rapidjson::StringBuffer buffer(0, 512);
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept(writer);

    const size_t size = buffer.GetSize();
    if (size > (sizeof(m_buf) - 2)) {
        return;
    }

    memcpy(m_sendBuf, buffer.GetString(), size);
    m_sendBuf[size]     = '\n';
    m_sendBuf[size + 1] = '\0';

    send(size + 1);
}


void Client::onClose()
{
    delete m_socket;

    m_stream = nullptr;
    m_socket = nullptr;
    setState(UnconnectedState);

    reconnect();
}


void Client::parse(char *line, size_t len)
{
    startTimeout();

    line[len - 1] = '\0';

    LOG_DEBUG("[%s:%u] received (%d bytes): \"%s\"", m_url.host(), m_url.port(), len, line);

    if (len < 32 || line[0] != '{') {
        if (!m_quiet) {
            LOG_ERR("[%s:%u] JSON decode failed", m_url.host(), m_url.port());
        }

        return;
    }

    rapidjson::Document doc;
    if (doc.ParseInsitu(line).HasParseError()) {
        if (!m_quiet) {
            LOG_ERR("[%s:%u] JSON decode failed: \"%s\"", m_url.host(), m_url.port(), rapidjson::GetParseError_En(doc.GetParseError()));
        }

        return;
    }

    if (!doc.IsObject()) {
        return;
    }

    const rapidjson::Value &id = doc["id"];
    if (id.IsInt64()) {
        parseResponse(id.GetInt64(), doc["result"], doc["error"]);
    }
    else {
        parseNotification(doc["method"].GetString(), doc["params"], doc["error"]);
    }
}


void Client::parseExtensions(const rapidjson::Value &value)
{
    if (!value.IsArray()) {
        return;
    }

    for (const rapidjson::Value &ext : value.GetArray()) {
        if (!ext.IsString()) {
            continue;
        }

        if (strcmp(ext.GetString(), "nicehash") == 0) {
            m_nicehash = true;
        }
    }
}


void Client::parseNotification(const char *method, const rapidjson::Value &params, const rapidjson::Value &error)
{
    if (error.IsObject()) {
        if (!m_quiet) {
            LOG_ERR("[%s:%u] error: \"%s\", code: %d", m_url.host(), m_url.port(), error["message"].GetString(), error["code"].GetInt());
        }
        return;
    }

    if (!method) {
        return;
    }

    if (strcmp(method, "job") == 0) {
        int code = -1;
        if (parseJob(params, &code)) {
            m_listener->onJobReceived(this, m_job);
        }

        return;
    }

    LOG_WARN("[%s:%u] unsupported method: \"%s\"", m_url.host(), m_url.port(), method);
}


void Client::parseResponse(int64_t id, const rapidjson::Value &result, const rapidjson::Value &error)
{
    if (error.IsObject()) {
        const char *message = error["message"].GetString();

        auto it = m_results.find(id);
        if (it != m_results.end()) {
            it->second.done();
            m_listener->onResultAccepted(this, it->second, message);
            m_results.erase(it);
        }
        else if (!m_quiet) {
            LOG_ERR("[%s:%u] error: \"%s\", code: %d", m_url.host(), m_url.port(), message, error["code"].GetInt());
        }

        if (id == 1 || isCriticalError(message)) {
            close();
        }

        return;
    }

    if (!result.IsObject()) {
        return;
    }

    if (id == 1) {
        int code = -1;
        if (!parseLogin(result, &code)) {
            if (!m_quiet) {
                LOG_ERR("[%s:%u] login error code: %d", m_url.host(), m_url.port(), code);
            }

            close();
            return;
        }

        m_failures = 0;
        m_listener->onLoginSuccess(this);
        m_listener->onJobReceived(this, m_job);
        return;
    }

    auto it = m_results.find(id);
    if (it != m_results.end()) {
        it->second.done();
        m_listener->onResultAccepted(this, it->second, nullptr);
        m_results.erase(it);
    }
}


void Client::ping()
{
    send(snprintf(m_sendBuf, sizeof(m_sendBuf), "{\"id\":%" PRId64 ",\"jsonrpc\":\"2.0\",\"method\":\"keepalived\",\"params\":{\"id\":\"%s\"}}\n", m_sequence, m_rpcId.data()));
}

void Client::setDormancy(bool ifDormancy){
    m_isDormancy = ifDormancy;
}

void Client::reconnect()
{

    if (m_isDormancy){
        return;
    }

    if (!m_listener) {
        delete this;

        return;
    }

    setState(ConnectingState);

#   ifndef ULORDRIG_PROXY_PROJECT
    if (m_url.isKeepAlive()) {
        uv_timer_stop(&m_keepAliveTimer);
    }
#   endif
    if (m_failures == -1) {
        return m_listener->onClose(this, -1);
    }

    m_failures++;
    m_listener->onClose(this, (int) m_failures);

    m_expire = uv_now(uv_default_loop()) + m_retryPause;
}


void Client::setState(SocketState state)
{
    LOG_DEBUG("[%s:%u] state: %d", m_url.host(), m_url.port(), state);

    if (m_state == state) {
        return;
    }

    m_state = state;
}


void Client::startTimeout()
{
    m_expire = 0;

#   ifndef ULORDRIG_PROXY_PROJECT
    if (!m_url.isKeepAlive()) {
        return;
    }

    uv_timer_start(&m_keepAliveTimer, [](uv_timer_t *handle) { getClient(handle->data)->ping(); }, kKeepAliveTimeout, 0);
#   endif
}


void Client::onAllocBuffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    auto client = getClient(handle->data);
    if (!client) {
        return;
    }

    buf->base = &client->m_recvBuf.base[client->m_recvBufPos];
    buf->len  = client->m_recvBuf.len - client->m_recvBufPos;
}


void Client::onClose(uv_handle_t *handle)
{
    auto client = getClient(handle->data);
    if (!client) {
        return;
    }
    client->onClose();
}


void Client::onConnect(uv_connect_t *req, int status)
{
    auto client = getClient(req->data);
    if (!client) {
        return;
    }

    if (status < 0) {
        if (!client->m_quiet) {
            LOG_ERR("[%s:%u] connect error: \"%s\"", client->m_url.host(), client->m_url.port(), uv_strerror(status));
        }

        delete req;
        client->close();
        return;
    }

    client->m_stream = static_cast<uv_stream_t*>(req->handle);
    client->m_stream->data = req->data;
    client->setState(ConnectedState);

    uv_read_start(client->m_stream, Client::onAllocBuffer, Client::onRead);
    delete req;

    client->login();
}


void Client::onRead(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    auto client = getClient(stream->data);
    if (!client) {
        return;
    }

    if (nread < 0) {
        if (nread != UV_EOF && !client->m_quiet) {
            LOG_ERR("[%s:%u] read error: \"%s\"", client->m_url.host(), client->m_url.port(), uv_strerror((int) nread));
        }

        client->close();
        return;
    }

    if ((size_t) nread > (sizeof(m_buf) - 8 - client->m_recvBufPos)) {
        client->close();
        return;
    }

    client->m_recvBufPos += nread;

    char* end;
    char* start = client->m_recvBuf.base;
    size_t remaining = client->m_recvBufPos;

    while ((end = static_cast<char*>(memchr(start, '\n', remaining))) != nullptr) {
        end++;
        size_t len = end - start;
        client->parse(start, len);

        remaining -= len;
        start = end;
    }

    if (remaining == 0) {
        client->m_recvBufPos = 0;
        return;
    }

    if (start == client->m_recvBuf.base) {
        return;
    }

    memcpy(client->m_recvBuf.base, start, remaining);
    client->m_recvBufPos = remaining;
}


void Client::onResolved(uv_getaddrinfo_t *req, int status, struct addrinfo *res)
{

    auto client = getClient(req->data);
    if (!client) {
        return;
    }

    if (status < 0) {
        if (!client->m_quiet) {
            LOG_ERR("[%s:%u] DNS error: \"%s\"", client->m_url.host(), client->m_url.port(), uv_strerror(status));
        }

        return client->reconnect();
    }

    addrinfo *ptr = res;
    std::vector<addrinfo*> ipv4;
    std::vector<addrinfo*> ipv6;

    while (ptr != nullptr) {
        if (ptr->ai_family == AF_INET) {
            ipv4.push_back(ptr);
        }

        if (ptr->ai_family == AF_INET6) {
            ipv6.push_back(ptr);
        }

        ptr = ptr->ai_next;
    }

    if (ipv4.empty() && ipv6.empty()) {
        if (!client->m_quiet) {
            LOG_ERR("[%s:%u] DNS error: \"No IPv4 (A) or IPv6 (AAAA) records found\"", client->m_url.host(), client->m_url.port());
        }

        uv_freeaddrinfo(res);
        return client->reconnect();
    }

    client->connect(ipv4, ipv6);
    uv_freeaddrinfo(res);
}
