#pragma once
#include <sstream>
#include "net/layer7/coco_http.hpp"
#include "utils/utils.hpp"

#define WS_CLIENT_TIMEOUT_US (int64_t)(3 * 1000 * 1000LL)
// websocket组合包最大不得超过4MB(防止内存爆炸)
#define MAX_WS_PACKET (4 * 1024 * 1024)

class WebSocketHeader {
 public:
    typedef enum {
        CONTINUATION = 0x0,
        TEXT = 0x1,
        BINARY = 0x2,
        RSV3 = 0x3,
        RSV4 = 0x4,
        RSV5 = 0x5,
        RSV6 = 0x6,
        RSV7 = 0x7,
        CLOSE = 0x8,
        PING = 0x9,
        PONG = 0xA,
        CONTROL_RSVB = 0xB,
        CONTROL_RSVC = 0xC,
        CONTROL_RSVD = 0xD,
        CONTROL_RSVE = 0xE,
        CONTROL_RSVF = 0xF
    } Type;

 public:
    WebSocketHeader() : _mask(4) {
        //获取_mask内部buffer的内存地址，该内存是malloc开辟的，地址为随机
        uint64_t ptr = (uint64_t)(&_mask[0]);
        //根据内存地址设置掩码随机数
        _mask.assign((uint8_t *)(&ptr), (uint8_t *)(&ptr) + 4);
    }
    virtual ~WebSocketHeader() {}
    void Reset() {
        _fin = false;
        _reserved = 0;
        _opcode = CONTINUATION;
        _mask_flag = false;
        _payload_len = 0;
        _mask.clear();
        payload_offset_ = 0;
    }

 public:
    bool _fin;
    uint8_t _reserved;
    Type _opcode;
    bool _mask_flag;
    size_t _payload_len;
    std::vector<uint8_t> _mask;

    size_t payload_offset_ = 0;
};

class WebSocektMessage {
 public:
    WebSocektMessage(){};
    virtual ~WebSocektMessage(){};

    WebSocketHeader::Type _opcode;
    bool is_fragmented = false;
    std::string cache_;
    WebSocketHeader header_;
    bool got_header_ = false;

    std::string data_cache_;
    std::string data_;
};

class WebSocketConn;
typedef std::function<int(WebSocketConn *, std::unique_ptr<WebSocektMessage> msg)>
    WebsocketMessageHandler;

class WebSocketConn : public ConnRoutine {
 public:
    WebSocketConn(void *observer, ConnManager *mgr, StreamConn *conn, HttpMessage *r);
    virtual ~WebSocketConn();
    virtual std::string GetRemoteAddr() { return conn_->RemoteAddr(); };
    int Send(uint8_t *buf, ssize_t len, WebSocketHeader::Type data_type);

 public:
    virtual int DoCycle();
    /**
     * 输入数据以便解包webSocket数据以及处理粘包问题
     * 可能触发onWebSocketDecodeHeader和onWebSocketDecodePayload回调
     * @param data 需要解包的数据，可能是不完整的包或多个包
     * @param len 数据长度
     */
    void decode(uint8_t *data, size_t len);

    /**
     * 编码一个数据包
     * 将触发2次onWebSocketEncodeData回调
     * @param header 数据头
     * @param buffer 负载数据
     */
    std::string Encode(WebSocketHeader &header, uint8_t *buffer, uint32_t size);

    /**
     * 接收到完整的一个webSocket数据包后回调
     * @param header 数据包包头
     */
    void ProcessMessage(std::unique_ptr<WebSocektMessage> msg);

 private:
    // HttpResponseWriter rsp_writer_ = nullptr;
    HttpMessage *http_msg_ = nullptr;
    StreamConn *conn_ = nullptr;
    std::unique_ptr<WebSocektMessage> cur_msg_;

    int _mask_offset = 0;
    void *observer_ = nullptr;
};

class WebSocketClient {
 public:
    WebSocketClient();
    virtual ~WebSocketClient();

    /**
     * Start
     * 目的是替换TcpClient的连接服务器行为，使之先完成WebSocket握手
     * @param host websocket服务器ip或域名
     * @param iPort websocket服务器端口
     * @param timeout_sec 超时时间
     */
    int Start(bool is_wss, const std::string &host, uint16_t port, std::string path,
              uint64_t timeout_us = WS_CLIENT_TIMEOUT_US);

    void SetMessageHandler(WebsocketMessageHandler handler) { message_handler_ = handler; }
    int HandleMessage(std::unique_ptr<WebSocektMessage> msg);
    int Send(uint8_t *buf, ssize_t len, WebSocketHeader::Type data_type = WebSocketHeader::TEXT);

 private:
    std::string sec_websocket_key_;
    ConnManager *manager_;

    HttpClient *http_client_ = nullptr;

    HttpMessage *ws_http_msg_ = nullptr;
    WebSocketConn *conn_ = nullptr;
    WebsocketMessageHandler message_handler_ = nullptr;
};
