// Copyright (c) 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef QUICHE_QUIC_QUARTC_QUARTC_FAKES_H_
#define QUICHE_QUIC_QUARTC_QUARTC_FAKES_H_

#include <string>

#include "net/third_party/quiche/src/quic/core/quic_error_codes.h"
#include "net/third_party/quiche/src/quic/core/quic_types.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_clock.h"
#include "net/third_party/quiche/src/quic/quartc/quartc_endpoint.h"
#include "net/third_party/quiche/src/quic/quartc/quartc_session.h"
#include "net/third_party/quiche/src/quic/quartc/quartc_stream.h"

namespace quic {

class FakeQuartcEndpointDelegate : public QuartcEndpoint::Delegate {
 public:
  explicit FakeQuartcEndpointDelegate(QuartcSession::Delegate* session_delegate)
      : session_delegate_(session_delegate) {}

  void OnSessionCreated(QuartcSession* session) override {
    CHECK_EQ(session_, nullptr);
    CHECK_NE(session, nullptr);
    session_ = session;
    session_->SetDelegate(session_delegate_);
    session_->StartCryptoHandshake();
  }

  void OnConnectError(QuicErrorCode error,
                      const std::string& error_details) override {
    QUIC_LOG(FATAL)
        << "Unexpected error during QuartcEndpoint::Connect(); error=" << error
        << ", error_details=" << error_details;
  }

  QuartcSession* session() { return session_; }

 private:
  QuartcSession::Delegate* session_delegate_;
  QuartcSession* session_ = nullptr;
};

class FakeQuartcSessionDelegate : public QuartcSession::Delegate {
 public:
  explicit FakeQuartcSessionDelegate(QuartcStream::Delegate* stream_delegate,
                                     const QuicClock* clock)
      : stream_delegate_(stream_delegate), clock_(clock) {}

  void OnConnectionWritable() override {
    QUIC_LOG(INFO) << "Connection writable!";
    if (!writable_time_.IsInitialized()) {
      writable_time_ = clock_->Now();
    }
  }

  // Called when peers have established forward-secure encryption
  void OnCryptoHandshakeComplete() override {
    QUIC_LOG(INFO) << "Crypto handshake complete!";
    crypto_handshake_time_ = clock_->Now();
  }

  // Called when connection closes locally, or remotely by peer.
  void OnConnectionClosed(QuicErrorCode error_code,
                          const std::string& error_details,
                          ConnectionCloseSource source) override {
    connected_ = false;
  }

  // Called when an incoming QUIC stream is created.
  void OnIncomingStream(QuartcStream* quartc_stream) override {
    last_incoming_stream_ = quartc_stream;
    last_incoming_stream_->SetDelegate(stream_delegate_);
  }

  void OnMessageReceived(QuicStringPiece message) override {
    incoming_messages_.emplace_back(message);
  }

  void OnMessageSent(int64_t datagram_id) override {
    sent_datagram_ids_.push_back(datagram_id);
  }

  void OnCongestionControlChange(QuicBandwidth bandwidth_estimate,
                                 QuicBandwidth pacing_rate,
                                 QuicTime::Delta latest_rtt) override {}

  QuartcStream* last_incoming_stream() const { return last_incoming_stream_; }

  // Returns all received messages.
  const std::vector<std::string>& incoming_messages() const {
    return incoming_messages_;
  }

  // Returns all sent datagram ids in the order sent.
  const std::vector<int64_t>& sent_datagram_ids() const {
    return sent_datagram_ids_;
  }

  bool connected() const { return connected_; }
  QuicTime writable_time() const { return writable_time_; }
  QuicTime crypto_handshake_time() const { return crypto_handshake_time_; }

 private:
  QuartcStream* last_incoming_stream_;
  std::vector<std::string> incoming_messages_;
  std::vector<int64_t> sent_datagram_ids_;
  bool connected_ = true;
  QuartcStream::Delegate* stream_delegate_;
  QuicTime writable_time_ = QuicTime::Zero();
  QuicTime crypto_handshake_time_ = QuicTime::Zero();
  const QuicClock* clock_;
};

class FakeQuartcStreamDelegate : public QuartcStream::Delegate {
 public:
  size_t OnReceived(QuartcStream* stream,
                    iovec* iov,
                    size_t iov_length,
                    bool fin) override {
    size_t bytes_consumed = 0;
    for (size_t i = 0; i < iov_length; ++i) {
      received_data_[stream->id()] += std::string(
          static_cast<const char*>(iov[i].iov_base), iov[i].iov_len);
      bytes_consumed += iov[i].iov_len;
    }
    return bytes_consumed;
  }

  void OnClose(QuartcStream* stream) override {
    errors_[stream->id()] = stream->stream_error();
  }

  void OnBufferChanged(QuartcStream* stream) override {}

  bool has_data() { return !received_data_.empty(); }
  std::map<QuicStreamId, std::string> data() { return received_data_; }

  QuicRstStreamErrorCode stream_error(QuicStreamId id) { return errors_[id]; }

 private:
  std::map<QuicStreamId, std::string> received_data_;
  std::map<QuicStreamId, QuicRstStreamErrorCode> errors_;
};

}  // namespace quic

#endif  // QUICHE_QUIC_QUARTC_QUARTC_FAKES_H_
