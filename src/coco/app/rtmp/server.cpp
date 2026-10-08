#include "coco/app/rtmp/server.hpp"

#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/log/log.hpp"
#include "coco/net/tcp.hpp"
#include "coco/app/rtmp/codec/command.hpp"
#include "coco/app/rtmp/codec/handshake.hpp"
#include "coco/net/tls/conn.hpp"

namespace coco {

namespace {

int WriteOnStatus(RtmpConn* conn, uint32_t stream_id, const std::string& code,
                  const std::string& desc) {
    return RtmpWriteCommand(conn, stream_id, RtmpOnStatus(code, desc));
}

int WriteStreamBegin(RtmpConn* conn, uint32_t stream_id) {
    return conn->WriteMessage(RtmpStreamBegin(stream_id));
}

}  // namespace

int ServeRtmpConn(StreamConn& conn, const RtmpHandler& handler) {
    if (!handler) {
        return ERROR_RTMP_PROTOCOL;
    }
    int ret = RtmpHandshake(&conn, &conn, false);
    if (ret != COCO_SUCCESS) {
        return ret;
    }
    RtmpConn rtmp(&conn);
    if ((ret = rtmp.WriteWindowAckSize(kRtmpAckWindow)) != COCO_SUCCESS ||
        (ret = rtmp.WritePeerBandwidth(kRtmpAckWindow, 2)) != COCO_SUCCESS ||
        (ret = rtmp.SetChunkSize(kRtmpOutChunkSize)) != COCO_SUCCESS) {
        return ret;
    }

    std::string app, tc_url, flash_ver;
    bool connected = false;
    uint32_t next_sid = 1;
    while (true) {
        RtmpMessage msg;
        if ((ret = rtmp.ReadMessage(&msg)) != COCO_SUCCESS) {
            return ret;
        }
        if (!RtmpIsCommand(msg.type)) {
            continue;
        }
        RtmpCommand cmd;
        if ((ret = RtmpParseCommand(msg, &cmd)) != COCO_SUCCESS) {
            return ret;
        }
        if (cmd.name == "connect") {
            if (connected) {
                continue;
            }
            app = cmd.object.GetString("app");
            size_t aq = app.find('?');
            if (aq != std::string::npos) {
                app.erase(aq);
            }
            while (!app.empty() && app[app.size() - 1] == '/') {
                app.erase(app.size() - 1);
            }
            tc_url = cmd.object.GetString("tcUrl");
            flash_ver = cmd.object.GetString("flashVer");
            double enc = cmd.object.GetNumber("objectEncoding", 0);
            if (enc != 0 && enc != 3) {
                return ERROR_RTMP_PROTOCOL;
            }
            Amf0Value props = Amf0Value::Object();
            props.Put("fmsVer", Amf0Value::Str("FMS/3,0,1,123"));
            props.Put("capabilities", Amf0Value::Num(31));
            props.Put("mode", Amf0Value::Num(1));
            Amf0Value info = Amf0Value::Object();
            info.Put("level", Amf0Value::Str("status"));
            info.Put("code", Amf0Value::Str("NetConnection.Connect.Success"));
            info.Put("description", Amf0Value::Str("Connection succeeded."));
            info.Put("objectEncoding", Amf0Value::Num(0));
            std::vector<Amf0Value> v;
            v.push_back(Amf0Value::Str("_result"));
            v.push_back(Amf0Value::Num(cmd.tx));
            v.push_back(props);
            v.push_back(info);
            if ((ret = RtmpWriteCommand(&rtmp, 0, v)) != COCO_SUCCESS) {
                return ret;
            }
            if ((ret = WriteStreamBegin(&rtmp, 0)) != COCO_SUCCESS) {
                return ret;
            }
            connected = true;
            coco_info("rtmp: connect app=%s tcUrl=%s", app.c_str(), tc_url.c_str());
        } else if (!connected) {
            return ERROR_RTMP_PROTOCOL;
        } else if (cmd.name == "releaseStream" || cmd.name == "FCUnpublish" ||
                   cmd.name == "_checkbw") {
            if ((ret = RtmpWriteCommand(&rtmp, 0, RtmpResult(cmd.tx, Amf0Value::Undef()))) !=
                COCO_SUCCESS) {
                return ret;
            }
        } else if (cmd.name == "FCPublish") {
            Amf0Value info = Amf0Value::Object();
            info.Put("level", Amf0Value::Str("status"));
            info.Put("code", Amf0Value::Str("NetStream.Publish.Start"));
            info.Put("description", Amf0Value::Str(""));
            std::vector<Amf0Value> fc;
            fc.push_back(Amf0Value::Str("onFCPublish"));
            fc.push_back(Amf0Value::Num(0));
            fc.push_back(Amf0Value::Null());
            fc.push_back(info);
            if ((ret = RtmpWriteCommand(&rtmp, 0, fc)) != COCO_SUCCESS) {
                return ret;
            }
        } else if (cmd.name == "createStream") {
            uint32_t sid = next_sid++;
            if ((ret = RtmpWriteCommand(&rtmp, 0, RtmpResult(cmd.tx, Amf0Value::Num(sid)))) !=
                COCO_SUCCESS) {
                return ret;
            }
        } else if (cmd.name == "getStreamLength") {
            if ((ret = RtmpWriteCommand(&rtmp, 0, RtmpResult(cmd.tx, Amf0Value::Num(0)))) != COCO_SUCCESS) {
                return ret;
            }
        } else if (cmd.name == "publish" || cmd.name == "play") {
            RtmpRequest req;
            req.app = app;
            req.tc_url = tc_url;
            req.flash_ver = flash_ver;
            req.publish = cmd.name == "publish";
            req.stream_id = msg.stream_id;
            if (!cmd.args.empty() && cmd.args[0].type == Amf0Value::kString) {
                req.stream = cmd.args[0].str;
            }
            if (req.publish && cmd.args.size() >= 2 && cmd.args[1].type == Amf0Value::kString) {
                req.publish_type = cmd.args[1].str;
            }
            if (req.publish && req.publish_type.empty()) {
                req.publish_type = "live";
            }
            if (req.stream.empty() || req.stream_id == 0) {
                WriteOnStatus(&rtmp, req.stream_id, "NetStream.Publish.BadName", "bad stream");
                return ERROR_RTMP_PROTOCOL;
            }
            if ((ret = WriteStreamBegin(&rtmp, req.stream_id)) != COCO_SUCCESS) {
                return ret;
            }
            if (req.publish) {
                ret = WriteOnStatus(&rtmp, req.stream_id, "NetStream.Publish.Start",
                                    "Start publishing");
            } else {
                if ((ret = WriteOnStatus(&rtmp, req.stream_id, "NetStream.Play.Reset", "Reset")) !=
                    COCO_SUCCESS) {
                    return ret;
                }
                ret = WriteOnStatus(&rtmp, req.stream_id, "NetStream.Play.Start", "Start playback");
            }
            if (ret != COCO_SUCCESS) {
                return ret;
            }
            coco_info("rtmp: %s %s/%s stream_id=%u", req.publish ? "publish" : "play",
                      req.app.c_str(), req.stream.c_str(), req.stream_id);
            return handler(rtmp, req);
        } else if (cmd.name == "closeStream" || cmd.name == "deleteStream") {
            return COCO_SUCCESS;
        }
    }
}

RtmpServer::RtmpServer(RtmpHandler handler) : handler_(handler) {}

RtmpServer::~RtmpServer() { server_.reset(); }

int RtmpServer::ListenAndServe(const std::string& ip, int port) {
    int ret = Start(ip, port);
    if (ret == COCO_SUCCESS) {
        Wait();
    }
    return ret;
}

int RtmpServer::ListenAndServeTLS(const std::string& ip, int port, const std::string& crt_file,
                                  const std::string& key_file) {
    int ret = StartTLS(ip, port, crt_file, key_file);
    if (ret == COCO_SUCCESS) {
        Wait();
    }
    return ret;
}

int RtmpServer::Serve(std::unique_ptr<StreamListener> l) {
    int ret = Start(std::move(l));
    if (ret == COCO_SUCCESS) {
        Wait();
    }
    return ret;
}

int RtmpServer::Start(const std::string& ip, int port) {
    std::unique_ptr<TcpListener> l;
    int ret = ListenTcp(ip, port, &l);
    if (ret != COCO_SUCCESS) {
        coco_error("rtmp: listen on %s:%d failed. ret=%d", ip.c_str(), port, ret);
        return ret;
    }
    return StartOn(std::move(l), "", "");
}

int RtmpServer::StartTLS(const std::string& ip, int port, const std::string& crt_file,
                         const std::string& key_file) {
    std::unique_ptr<TcpListener> l;
    int ret = ListenTcp(ip, port, &l);
    if (ret != COCO_SUCCESS) {
        coco_error("rtmps: listen on %s:%d failed. ret=%d", ip.c_str(), port, ret);
        return ret;
    }
    return StartOn(std::move(l), crt_file, key_file);
}

int RtmpServer::Start(std::unique_ptr<StreamListener> l) { return StartOn(std::move(l), "", ""); }

void RtmpServer::Wait() {
    if (server_) {
        server_->Wait();
    }
}

int RtmpServer::StartOn(std::unique_ptr<StreamListener> l, const std::string& crt_file,
                        const std::string& key_file) {
    if (server_ != nullptr) {
        coco_error("rtmp server already serving");
        return ERROR_THREAD_STARTED;
    }
    RtmpHandler handler = handler_;
    StreamHandler serve = [handler](StreamConn& conn) { return ServeRtmpConn(conn, handler); };
    if (!crt_file.empty() && !key_file.empty()) {
        std::shared_ptr<TlsConfig> cfg;
        int ret = TlsConfig::NewServer(key_file, crt_file, &cfg);
        if (ret != COCO_SUCCESS) {
            coco_error("rtmps: loading %s and %s failed. ret=%d", crt_file.c_str(),
                       key_file.c_str(), ret);
            return ret;
        }
        serve = TlsHandler(cfg, serve);
    }
    server_.reset(new TcpServer(serve));
    return server_->Start(std::move(l));
}

void RtmpServer::Stop() {
    if (server_) {
        server_->Stop();
    }
}

}  // namespace coco
