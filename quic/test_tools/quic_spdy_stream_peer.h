// Copyright (c) 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef QUICHE_QUIC_TEST_TOOLS_QUIC_SPDY_STREAM_PEER_H_
#define QUICHE_QUIC_TEST_TOOLS_QUIC_SPDY_STREAM_PEER_H_

#include "net/third_party/quiche/src/quic/core/quic_ack_listener_interface.h"
#include "net/third_party/quiche/src/quic/core/quic_interval_set.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_containers.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_string_piece.h"

namespace quic {

class QpackDecodedHeadersAccumulator;
class QuicSpdyStream;

namespace test {

class QuicSpdyStreamPeer {
 public:
  static void set_ack_listener(
      QuicSpdyStream* stream,
      QuicReferenceCountedPointer<QuicAckListenerInterface> ack_listener);
  static const QuicIntervalSet<QuicStreamOffset>& unacked_frame_headers_offsets(
      QuicSpdyStream* stream);

  // TODO(b/112770235): Remove once blocked decoding is implemented
  // and can be tested with delayed encoder stream data.
  static void pretend_blocked_decoding(QuicSpdyStream* stream);
  static QpackDecodedHeadersAccumulator* qpack_decoded_headers_accumulator(
      QuicSpdyStream* stream);
};

}  // namespace test

}  // namespace quic

#endif  // QUICHE_QUIC_TEST_TOOLS_QUIC_SPDY_STREAM_PEER_H_
