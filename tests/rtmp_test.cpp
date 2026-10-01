#include <string.h>

#include <string>
#include <vector>

#include "coco/coco_api.h"
#include "coco/common/error.hpp"
#include "coco/net/layer7/rtmp/coco_rtmp.hpp"
#include "coco/net/layer7/rtmp/rtmp_amf0.hpp"
#include "coco/net/layer7/rtmp/rtmp_chunk.hpp"
#include "coco/net/layer7/rtmp/rtmp_handshake.hpp"
#include "coco/server/coco_rtmp_server.hpp"
#include "coco/utils/io.hpp"
#include "test_util.hpp"

using namespace coco;

namespace {

class Bytes : public IoReader {
 public:
    explicit Bytes(std::string s, bool one) : s_(s), one_(one) {}

    int Read(void* buf, size_t size, ssize_t* nread) override {
        if (i_ >= s_.size()) {
            *nread = 0;
            return ERROR_SOCKET_CLOSED;
        }
        size_t n = one_ ? 1 : size;
        if (n > s_.size() - i_) {
            n = s_.size() - i_;
        }
        memcpy(buf, s_.data() + i_, n);
        i_ += n;
        *nread = (ssize_t)n;
        return COCO_SUCCESS;
    }

 private:
    std::string s_;
    size_t i_ = 0;
    bool one_ = false;
};

bool SameMessage(const RtmpMessage& a, const RtmpMessage& b) {
    return a.type == b.type && a.timestamp == b.timestamp && a.stream_id == b.stream_id &&
           a.payload == b.payload;
}

void ReadBack(const std::string& bytes, const std::vector<RtmpMessage>& want, bool one_byte) {
    Bytes in(bytes, one_byte);
    RtmpChunkReader reader(&in);
    for (size_t i = 0; i < want.size(); i++) {
        RtmpMessage got;
        CHECK_EQ(reader.ReadMessage(&got, nullptr), COCO_SUCCESS);
        CHECK(got.csid == want[i].csid);
        CHECK(SameMessage(got, want[i]));
    }
}

}  // namespace

COTEST(RtmpDigestKnownAnswer) { CHECK(RtmpDigestSelfCheck()); }

COTEST(RtmpAmf0RoundTrip) {
    Amf0Value obj = Amf0Value::Object();
    obj.Put("app", Amf0Value::Str("live"));
    obj.Put("n", Amf0Value::Num(1.5));
    obj.Put("ok", Amf0Value::Bool(true));
    obj.Put("z", Amf0Value::Null());
    Amf0Value nested = Amf0Value::Ecma();
    nested.Put("width", Amf0Value::Num(160));
    obj.Put("meta", nested);

    Amf0Value arr = Amf0Value::Array();
    arr.Push(Amf0Value::Str("a"));
    arr.Push(Amf0Value::Undef());
    arr.Push(Amf0Value::Num(42));

    std::vector<Amf0Value> values;
    values.push_back(Amf0Value::Str("connect"));
    values.push_back(Amf0Value::Num(1));
    values.push_back(obj);
    values.push_back(arr);
    values.push_back(Amf0Value::Str(std::string(70000, 'x')));

    std::string bytes = Amf0Encode(values);
    std::vector<Amf0Value> back;
    CHECK_EQ(Amf0DecodeAll((const uint8_t*)bytes.data(), bytes.size(), &back), COCO_SUCCESS);
    CHECK_EQ(back.size(), values.size());
    CHECK(back[0].str == "connect");
    CHECK(back[1].number == 1);
    CHECK(back[2].GetString("app") == "live");
    CHECK(back[2].GetNumber("n") == 1.5);
    CHECK(back[2].Find("ok")->boolean);
    CHECK(back[2].Find("z")->type == Amf0Value::kNull);
    CHECK(back[2].Find("meta")->GetNumber("width") == 160);
    CHECK_EQ(back[3].items.size(), 3);
    CHECK(back[3].items[0].str == "a");
    CHECK(back[3].items[1].type == Amf0Value::kUndefined);
    CHECK(back[3].items[2].number == 42);
    CHECK_EQ(back[4].str.size(), 70000);
    CHECK(back[4].str[0] == 'x');

    uint8_t bad[] = {0x02, 0x00};
    Amf0Value v;
    size_t used = 0;
    CHECK_EQ(Amf0Decode(bad, sizeof(bad), &v, &used), ERROR_RTMP_AMF);
}

COTEST(RtmpChunksRoundTrip) {
    RtmpChunkWriter writer;
    writer.SetChunkSize(128);
    std::vector<RtmpMessage> want;
    std::string bytes;

    RtmpMessage a;
    a.csid = 4;
    a.stream_id = 1;
    a.type = RTMP_MSG_AUDIO;
    a.timestamp = 0x01000000;
    a.payload.assign(200, 'a');
    want.push_back(a);
    CHECK_EQ(writer.Encode(a.csid, a, &bytes), COCO_SUCCESS);

    RtmpMessage b = a;
    b.timestamp = 0x01000000 + 1000;
    b.payload.assign(200, 'b');
    want.push_back(b);
    CHECK_EQ(writer.Encode(b.csid, b, &bytes), COCO_SUCCESS);

    RtmpMessage c = b;
    c.timestamp = b.timestamp + 0x01000000;
    c.payload.assign(200, 'c');
    want.push_back(c);
    CHECK_EQ(writer.Encode(c.csid, c, &bytes), COCO_SUCCESS);

    RtmpMessage d;
    d.csid = 80;
    d.stream_id = 1;
    d.type = RTMP_MSG_VIDEO;
    d.timestamp = 5;
    d.payload = "vid";
    want.push_back(d);
    CHECK_EQ(writer.Encode(d.csid, d, &bytes), COCO_SUCCESS);

    RtmpMessage e;
    e.csid = 400;
    e.stream_id = 7;
    e.type = RTMP_MSG_ACK;
    e.timestamp = 0;
    want.push_back(e);
    CHECK_EQ(writer.Encode(e.csid, e, &bytes), COCO_SUCCESS);

    ReadBack(bytes, want, false);
    ReadBack(bytes, want, true);

    // A chunk of csid 4, then a whole message on csid 6, then the rest of csid 4.
    RtmpChunkWriter aw;
    aw.SetChunkSize(128);
    std::string ab;
    CHECK_EQ(aw.Encode(a.csid, a, &ab), COCO_SUCCESS);
    // fmt 0, no 2-byte csid, extended timestamp: 1 + 11 + 4 + 128.
    CHECK(ab.size() > 144);
    RtmpChunkWriter bw;
    bw.SetChunkSize(128);
    std::string bb;
    RtmpMessage mid;
    mid.csid = 6;
    mid.stream_id = 1;
    mid.type = RTMP_MSG_VIDEO;
    mid.timestamp = 9;
    mid.payload = "m";
    CHECK_EQ(bw.Encode(mid.csid, mid, &bb), COCO_SUCCESS);
    std::string mixed = ab.substr(0, 144) + bb + ab.substr(144);
    std::vector<RtmpMessage> order;
    order.push_back(mid);
    order.push_back(a);
    ReadBack(mixed, order, false);
}

COTEST(RtmpUrlParse) {
    RtmpUrl u;
    CHECK_EQ(ParseRtmpUrl("rtmp://127.0.0.1/live/cam", &u), COCO_SUCCESS);
    CHECK(!u.tls);
    CHECK(u.host == "127.0.0.1");
    CHECK_EQ(u.port, 1935);
    CHECK(u.app == "live");
    CHECK(u.stream == "cam");
    CHECK(u.tc_url == "rtmp://127.0.0.1/live");

    CHECK_EQ(ParseRtmpUrl("rtmp://host:1936/app/a/b?x=1", &u), COCO_SUCCESS);
    CHECK(u.host == "host");
    CHECK_EQ(u.port, 1936);
    CHECK(u.app == "app");
    CHECK(u.stream == "a/b");
    CHECK(u.tc_url == "rtmp://host:1936/app");

    CHECK_EQ(ParseRtmpUrl("rtmps://[::1]:1935/live/s", &u), COCO_SUCCESS);
    CHECK(u.tls);
    CHECK(u.host == "::1");
    CHECK_EQ(u.port, 1935);
    CHECK(u.tc_url == "rtmps://[::1]/live");

    CHECK_EQ(ParseRtmpUrl("http://127.0.0.1/live", &u), ERROR_RTMP_URL);
    CHECK_EQ(ParseRtmpUrl("rtmp://127.0.0.1", &u), ERROR_RTMP_URL);
    RtmpClient client;
    CHECK_EQ(client.Dial("not a url"), ERROR_RTMP_URL);
    CHECK_EQ(client.Dial("rtmps://127.0.0.1/live/s"), ERROR_RTMP_URL);
}

COTEST(RtmpPublishAndPlay) {
    const int port = 19340;
    std::vector<RtmpMessage> stored;
    RtmpServer server([&](RtmpConn& conn, const RtmpRequest& req) {
        if (req.publish) {
            CHECK(req.app == "live");
            CHECK(req.stream == "cam");
            CHECK(req.publish_type == "live");
            RtmpMessage msg;
            while (stored.size() < 3 && conn.ReadMessage(&msg) == COCO_SUCCESS) {
                if (msg.type == RTMP_MSG_AUDIO || msg.type == RTMP_MSG_VIDEO ||
                    msg.type == RTMP_MSG_DATA_AMF0) {
                    stored.push_back(msg);
                }
            }
            return COCO_SUCCESS;
        }
        CHECK(req.stream == "cam");
        CHECK(req.stream_id != 0);
        for (size_t i = 0; i < stored.size(); i++) {
            RtmpMessage msg = stored[i];
            msg.stream_id = req.stream_id;
            msg.csid = 0;
            int ret = conn.WriteMessage(msg);
            if (ret != COCO_SUCCESS) {
                return ret;
            }
        }
        return COCO_SUCCESS;
    });
    CHECK_EQ(server.Start("127.0.0.1", port), COCO_SUCCESS);

    RtmpClient pub;
    CHECK_EQ(pub.Dial("rtmp://127.0.0.1:19340/live/cam"), COCO_SUCCESS);
    CHECK_EQ(pub.Publish(), COCO_SUCCESS);
    CHECK(pub.StreamId() != 0);

    RtmpMessage audio;
    audio.type = RTMP_MSG_AUDIO;
    audio.timestamp = 0x01000000;
    audio.payload.assign(5000, 'A');
    CHECK_EQ(pub.WriteMessage(audio), COCO_SUCCESS);

    RtmpMessage video;
    video.type = RTMP_MSG_VIDEO;
    video.timestamp = 0x01000000 + 40;
    video.payload.assign(300, 'V');
    CHECK_EQ(pub.WriteMessage(video), COCO_SUCCESS);

    RtmpMessage data;
    data.type = RTMP_MSG_DATA_AMF0;
    data.timestamp = 0;
    std::vector<Amf0Value> meta;
    meta.push_back(Amf0Value::Str("@setDataFrame"));
    meta.push_back(Amf0Value::Str("onMetaData"));
    Amf0Value props = Amf0Value::Ecma();
    props.Put("width", Amf0Value::Num(160));
    meta.push_back(props);
    data.payload = Amf0Encode(meta);
    CHECK_EQ(pub.WriteMessage(data), COCO_SUCCESS);

    CHECK(cotest::WaitUntil([&] { return stored.size() == 3; }, 2000));
    CHECK_EQ(stored.size(), 3);
    CHECK_EQ(stored[0].timestamp, audio.timestamp);
    CHECK(stored[0].payload == audio.payload);
    CHECK_EQ(stored[1].timestamp, video.timestamp);
    CHECK(stored[1].payload == video.payload);
    CHECK(stored[2].payload == data.payload);

    RtmpClient play;
    CHECK_EQ(play.Dial("rtmp://127.0.0.1:19340/live/cam"), COCO_SUCCESS);
    CHECK_EQ(play.Play(), COCO_SUCCESS);
    for (size_t i = 0; i < stored.size(); i++) {
        RtmpMessage msg;
        CHECK_EQ(play.ReadMessage(&msg), COCO_SUCCESS);
        CHECK_EQ(msg.type, stored[i].type);
        CHECK_EQ(msg.timestamp, stored[i].timestamp);
        CHECK(msg.payload == stored[i].payload);
        CHECK_EQ(msg.stream_id, play.StreamId());
    }
}
