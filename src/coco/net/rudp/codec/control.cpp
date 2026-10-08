#include "coco/net/rudp/codec/control.hpp"

#include <string.h>

#include <algorithm>

namespace coco {

RudpOptions ClampRudpOptions(const RudpOptions &o) {
    RudpOptions c = o;
    c.interval_us = std::max<int64_t>(c.interval_us, 1000);
    c.link_timeout_us = std::max(c.link_timeout_us, 3 * c.interval_us);
    // A resend at least every third of the link timeout: a segment goes out a few times
    // before the link is given up, and a zero-window probe hears from a live peer in time.
    c.max_rto_us = std::max(std::min(c.max_rto_us, c.link_timeout_us / 3), c.interval_us);
    c.min_rto_us = std::min(std::max(c.min_rto_us, c.interval_us), c.max_rto_us);
    c.initial_rto_us = std::min(std::max(c.initial_rto_us, c.min_rto_us), c.max_rto_us);
    c.send_window = std::min(std::max(c.send_window, 1), kRudpMaxWindow);
    c.recv_window = std::min(std::max(c.recv_window, 1), kRudpMaxWindow);
    c.backlog = std::max(c.backlog, 1);
    return c;
}

RudpControl::RudpControl(const RudpOptions &options, uint32_t conn_id, uint32_t isn, int64_t now)
    : opt_(ClampRudpOptions(options)), client_(true), conn_id_(conn_id), state_(State::kSynSent) {
    Init(isn, now);
    EmitHandshake(now);
}

RudpControl::RudpControl(const RudpOptions &options, const RudpPacket &syn, uint32_t isn,
                         int64_t now)
    : opt_(ClampRudpOptions(options)),
      client_(false),
      conn_id_(syn.conn_id),
      state_(State::kSynRcvd) {
    Init(isn, now);
    peer_isn_ = syn.seq;
    rcv_nxt_ = syn.seq;
    rmt_wnd_ = syn.window;
    EmitHandshake(now);
}

void RudpControl::Init(uint32_t isn, int64_t now) {
    local_isn_ = isn;
    snd_una_ = snd_nxt_ = isn;
    recover_ = isn;
    heard_us_ = now;
    rto_us_ = opt_.initial_rto_us;
    hs_rto_us_ = opt_.initial_rto_us;
    cwnd_ = std::min(kRudpInitialCwnd, opt_.send_window);
    ssthresh_ = opt_.send_window;
    // A power of two divides 2^32, so seq % size stays one slot per sequence number within
    // the window across the wrap as well.
    size_t ring = 1;
    while (ring < (size_t)opt_.recv_window) {
        ring *= 2;
    }
    rcv_ring_.resize(ring);
}

void RudpControl::EmitHandshake(int64_t now) {
    if (++hs_xmit_ == 1) {
        hs_first_sent_us_ = now;
    }
    hs_sent_us_ = now;
    hs_resend_us_ = now + hs_rto_us_;
    if (client_) {
        Out(RudpType::kSyn, local_isn_);
    } else {
        Out(RudpType::kSynAck, local_isn_);
    }
}

void RudpControl::Input(const RudpPacket &p, int64_t now) {
    if (p.conn_id != conn_id_) {
        return;
    }
    if (state_ == State::kClosed) {
        if (p.type != RudpType::kRst) {
            OutRst();
        }
        return;
    }
    if (p.type == RudpType::kRst) {
        OnRst();
        return;
    }

    if (state_ == State::kSynSent) {
        if (p.type != RudpType::kSynAck || p.ack != local_isn_) {
            return;
        }
        heard_us_ = now;
        peer_isn_ = p.seq;
        rcv_nxt_ = p.seq;
        rmt_wnd_ = p.window;
        if (hs_xmit_ == 1) {
            UpdateRtt(now - hs_sent_us_);
        }
        state_ = State::kEstablished;
        OutPureAck();
        return;
    }

    if (state_ == State::kSynRcvd) {
        if (p.type == RudpType::kSyn) {
            if (p.seq == peer_isn_) {
                heard_us_ = now;
                // Karn: an RTT measured over a resent SYN_ACK would be ambiguous.
                ++hs_xmit_;
                Out(RudpType::kSynAck, local_isn_);
            }
            return;
        }
        if (p.type == RudpType::kSynAck || p.ack != local_isn_) {
            return;
        }
        if (hs_xmit_ == 1) {
            UpdateRtt(now - hs_sent_us_);
        }
        state_ = State::kEstablished;
    }

    InputEstablished(p, now);
}

void RudpControl::InputEstablished(const RudpPacket &p, int64_t now) {
    switch (p.type) {
        case RudpType::kSyn:
            // The client did not see our SYN_ACK, though something else of it reached us.
            if (!client_ && p.seq == peer_isn_) {
                heard_us_ = now;
                Out(RudpType::kSynAck, local_isn_);
            }
            return;
        case RudpType::kSynAck:
            // The server did not see the ACK that completed the handshake.
            if (client_ && p.ack == local_isn_) {
                heard_us_ = now;
                OutPureAck();
            }
            return;
        default:
            break;
    }
    if (RudpBefore(snd_nxt_, p.ack)) {
        return;
    }
    heard_us_ = now;
    OnAck(p, now);
    if (p.type == RudpType::kData || p.type == RudpType::kFin) {
        OnData(p);
    }
    Flush(now);
    CheckCloseDone();
}

void RudpControl::OnAck(const RudpPacket &p, int64_t now) {
    int newly = 0;
    bool was_closed = rmt_wnd_ == 0;
    // The segment an ACK names first, while it is still in snd_buf_: it is the one an RTT
    // sample can come from, and usually the cumulative part acknowledges it as well.
    if (p.type == RudpType::kAck && !RudpBefore(p.seq, snd_una_) && RudpBefore(p.seq, snd_nxt_)) {
        Segment &seg = snd_buf_[p.seq - snd_una_];
        if (!seg.acked) {
            seg.acked = true;
            ++newly;
            if (seg.xmit == 1) {
                UpdateRtt(now - seg.sent_us);
            }
            for (Segment &s : snd_buf_) {
                if (!RudpBefore(s.seq, p.seq)) {
                    break;
                }
                if (s.acked || s.fast_resent || ++s.skipped < kRudpFastResend) {
                    continue;
                }
                s.fast_resent = true;
                OnLoss(s, false);
                Transmit(s, now);
                ++stats_.fast_resends;
            }
        }
    }

    if (!RudpBefore(p.ack, snd_una_)) {
        rmt_wnd_ = p.window;
        while (!snd_buf_.empty() && RudpBefore(snd_buf_.front().seq, p.ack)) {
            if (!snd_buf_.front().acked) {
                ++newly;
            }
            snd_buf_.pop_front();
        }
        snd_una_ = p.ack;
    }
    while (!snd_buf_.empty() && snd_buf_.front().acked) {
        snd_buf_.pop_front();
        ++snd_una_;
    }
    // Losses are only ever compared with recover_ within half the sequence space.
    if (RudpBefore(recover_, snd_una_)) {
        recover_ = snd_una_;
    }

    if (newly > 0) {
        OnAcked(newly);
    }
    // The window opened: the probe the receiver had no room for goes again now, not at its
    // backed-off timeout.
    if (was_closed && rmt_wnd_ > 0 && !snd_buf_.empty() && !snd_buf_.front().acked) {
        Transmit(snd_buf_.front(), now);
    }
}

void RudpControl::OnData(const RudpPacket &p) {
    bool fin = p.type == RudpType::kFin;
    if (RudpBefore(p.seq, rcv_nxt_)) {
        ++stats_.duplicates;
        OutAck(p.seq);
        return;
    }
    if (fin_known_ && (RudpBefore(fin_seq_, p.seq) || (p.seq == fin_seq_) != fin)) {
        return;
    }
    uint32_t off = p.seq - rcv_nxt_;
    // A FIN needs no room, so the next one is taken even when the window is closed.
    if (off >= (uint32_t)AdvWindow() && !(fin && off == 0)) {
        OutPureAck();
        return;
    }
    Slot &slot = rcv_ring_[p.seq % rcv_ring_.size()];
    if (slot.used) {
        if (slot.seq != p.seq) {
            return;  // never acknowledge what is not held
        }
        ++stats_.duplicates;
    } else {
        slot.used = true;
        slot.seq = p.seq;
        slot.fin = fin;
        slot.data = p.payload;
        ++stats_.segments_received;
        if (fin) {
            fin_known_ = true;
            fin_seq_ = p.seq;
        }
    }
    while (!peer_fin_) {
        Slot &head = rcv_ring_[rcv_nxt_ % rcv_ring_.size()];
        if (!head.used || head.seq != rcv_nxt_) {
            break;
        }
        // The stream ends at the FIN: what a peer put after it is never delivered.
        peer_fin_ = head.fin;
        if (!head.fin && !close_requested_) {
            rcv_queue_.push_back(std::move(head.data));
        }
        head.used = false;
        head.data.clear();
        ++rcv_nxt_;
    }
    OutAck(p.seq);
}

void RudpControl::OnRst() {
    if (state_ == State::kEstablished && close_requested_ && DataAcked()) {
        End(COCO_SUCCESS);
    } else {
        End(ERROR_RUDP_RESET);
    }
}

void RudpControl::OnAcked(int segments) {
    for (int i = 0; i < segments; ++i) {
        if (cwnd_ < ssthresh_) {
            ++cwnd_;
        } else if (++cwnd_cnt_ >= cwnd_) {
            ++cwnd_;
            cwnd_cnt_ = 0;
        }
    }
    cwnd_ = std::min(cwnd_, opt_.send_window);
}

void RudpControl::OnLoss(const Segment &seg, bool timeout) {
    if (!RudpBefore(seg.seq, recover_)) {
        ++stats_.congestion_events;
        ssthresh_ = std::max((int)snd_buf_.size() / 2, kRudpMinSsthresh);
        recover_ = snd_nxt_;
        cwnd_ = ssthresh_;
        cwnd_cnt_ = 0;
    }
    if (timeout) {
        cwnd_ = 1;
        cwnd_cnt_ = 0;
    }
}

void RudpControl::UpdateRtt(int64_t rtt_us) {
    rtt_us = std::max<int64_t>(rtt_us, 0);
    if (srtt_us_ == 0) {
        srtt_us_ = std::max<int64_t>(rtt_us, 1);
        rttvar_us_ = rtt_us / 2;
    } else {
        int64_t delta = srtt_us_ > rtt_us ? srtt_us_ - rtt_us : rtt_us - srtt_us_;
        rttvar_us_ = (3 * rttvar_us_ + delta) / 4;
        srtt_us_ = std::max<int64_t>((7 * srtt_us_ + rtt_us) / 8, 1);
    }
    rto_us_ = srtt_us_ + std::max(opt_.interval_us, 4 * rttvar_us_);
    rto_us_ = std::min(std::max(rto_us_, opt_.min_rto_us), opt_.max_rto_us);
}

void RudpControl::Transmit(Segment &seg, int64_t now) {
    if (++seg.xmit == 1) {
        seg.first_sent_us = now;
        seg.rto_us = rto_us_;
    }
    seg.sent_us = now;
    seg.resend_us = now + seg.rto_us;
    seg.skipped = 0;
    Out(seg.fin ? RudpType::kFin : RudpType::kData, seg.seq, seg.data);
}

void RudpControl::Update(int64_t now) {
    if (state_ == State::kClosed) {
        return;
    }
    if (state_ != State::kEstablished) {
        if (now >= hs_resend_us_) {
            hs_rto_us_ = std::min(hs_rto_us_ * 2, opt_.max_rto_us);
            EmitHandshake(now);
        }
    } else {
        for (Segment &seg : snd_buf_) {
            if (seg.acked || now < seg.resend_us) {
                continue;
            }
            // While the peer's window is closed a timeout is a probe left unanswered, which
            // is flow control, not congestion.
            if (rmt_wnd_ > 0) {
                OnLoss(seg, true);
            }
            seg.rto_us = std::min(seg.rto_us * 2, opt_.max_rto_us);
            Transmit(seg, now);
            ++stats_.rto_resends;
        }
    }
    CheckLink(now);
    Flush(now);
}

void RudpControl::CheckLink(int64_t now) {
    // A close is bounded even when the peer keeps answering without ever taking the data.
    if (close_requested_ && now - close_us_ >= opt_.link_timeout_us) {
        OutRst();
        End(ERROR_RUDP_TIMEOUT);
        return;
    }
    int64_t since = heard_us_;
    if (state_ != State::kEstablished) {
        since = std::max(since, hs_first_sent_us_);
    } else if (!snd_buf_.empty()) {
        since = std::max(since, snd_buf_.front().first_sent_us);
    } else {
        return;
    }
    if (now - since < opt_.link_timeout_us) {
        return;
    }
    // A server forgets a half-open connection quietly; a client tells it it gave up.
    if (state_ != State::kSynRcvd) {
        OutRst();
    }
    End(ERROR_RUDP_TIMEOUT);
}

size_t RudpControl::Send(const void *data, size_t size) {
    if (state_ != State::kEstablished || close_requested_) {
        return 0;
    }
    const char *p = (const char *)data;
    size_t n = 0;
    if (!snd_queue_.empty() && snd_queue_.back().data.size() < kRudpMss) {
        std::string &tail = snd_queue_.back().data;
        size_t k = std::min(size, kRudpMss - tail.size());
        tail.append(p, k);
        n += k;
    }
    while (n < size && snd_queue_.size() < (size_t)opt_.send_window) {
        size_t k = std::min(size - n, kRudpMss);
        snd_queue_.emplace_back();
        snd_queue_.back().data.assign(p + n, k);
        n += k;
    }
    return n;
}

bool RudpControl::CanSend() const {
    if (state_ != State::kEstablished || close_requested_) {
        return false;
    }
    return snd_queue_.size() < (size_t)opt_.send_window ||
           snd_queue_.back().data.size() < kRudpMss;
}

void RudpControl::Flush(int64_t now) {
    if (state_ != State::kEstablished) {
        return;
    }
    size_t limit = (size_t)std::min(std::min(opt_.send_window, (int)rmt_wnd_), cwnd_);
    if (limit == 0 && snd_buf_.empty()) {
        limit = 1;
    }
    while (!snd_queue_.empty() && snd_buf_.size() < limit) {
        snd_buf_.push_back(std::move(snd_queue_.front()));
        snd_queue_.pop_front();
        Segment &seg = snd_buf_.back();
        seg.seq = snd_nxt_++;
        Transmit(seg, now);
        ++stats_.segments_sent;
    }
}

size_t RudpControl::Recv(void *buf, size_t size) {
    char *p = (char *)buf;
    size_t n = 0;
    while (n < size && !rcv_queue_.empty()) {
        std::string &front = rcv_queue_.front();
        size_t k = std::min(size - n, front.size() - rcv_front_off_);
        memcpy(p + n, front.data() + rcv_front_off_, k);
        n += k;
        rcv_front_off_ += k;
        if (rcv_front_off_ == front.size()) {
            rcv_queue_.pop_front();
            rcv_front_off_ = 0;
        }
    }
    if (n > 0 && state_ == State::kEstablished && last_adv_wnd_ == 0 && AdvWindow() > 0) {
        OutPureAck();
    }
    return n;
}

void RudpControl::Close(int64_t now) {
    if (state_ != State::kEstablished) {
        Abort();
        return;
    }
    if (close_requested_) {
        return;
    }
    close_requested_ = true;
    close_us_ = now;
    rcv_queue_.clear();
    rcv_front_off_ = 0;
    snd_queue_.emplace_back();
    snd_queue_.back().fin = true;
    Flush(now);
    CheckCloseDone();
}

void RudpControl::Abort() {
    if (state_ == State::kClosed) {
        return;
    }
    OutRst();
    End(ERROR_RUDP_CLOSED);
}

bool RudpControl::DataAcked() const {
    if (snd_queue_.size() > 1 || (snd_queue_.size() == 1 && !snd_queue_.front().fin)) {
        return false;
    }
    for (const Segment &s : snd_buf_) {
        if (!s.acked && !s.fin) {
            return false;
        }
    }
    return true;
}

void RudpControl::CheckCloseDone() {
    if (state_ != State::kEstablished || !close_requested_) {
        return;
    }
    if ((snd_queue_.empty() && snd_buf_.empty()) || (peer_fin_ && DataAcked())) {
        End(COCO_SUCCESS);
    }
}

void RudpControl::End(int result) {
    state_ = State::kClosed;
    result_ = result;
    snd_queue_.clear();
    snd_buf_.clear();
}

int RudpControl::AdvWindow() const {
    if (state_ != State::kEstablished) {
        return opt_.recv_window;
    }
    return std::max(opt_.recv_window - (int)rcv_queue_.size(), 0);
}

void RudpControl::Out(RudpType type, uint32_t seq, const std::string &payload) {
    RudpPacket p;
    p.type = type;
    p.window = (uint16_t)AdvWindow();
    p.conn_id = conn_id_;
    p.seq = seq;
    p.ack = type == RudpType::kSynAck ? peer_isn_ : (type == RudpType::kSyn ? 0 : rcv_nxt_);
    p.payload = payload;
    last_adv_wnd_ = p.window;
    out_.emplace_back();
    EncodeRudpPacket(p, &out_.back());
}

void RudpControl::OutRst() {
    RudpPacket p;
    p.type = RudpType::kRst;
    p.conn_id = conn_id_;
    out_.emplace_back();
    EncodeRudpPacket(p, &out_.back());
}

std::vector<std::string> RudpControl::TakeOutput() {
    std::vector<std::string> out;
    out.swap(out_);
    return out;
}

RudpStats RudpControl::Stats() const {
    RudpStats s = stats_;
    s.srtt_us = srtt_us_;
    s.rto_us = rto_us_;
    s.cwnd = cwnd_;
    s.ssthresh = ssthresh_;
    s.in_flight = (int)snd_buf_.size();
    return s;
}

}  // namespace coco
