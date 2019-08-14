// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "net/third_party/quiche/src/quic/core/http/quic_header_list.h"
#include "net/third_party/quiche/src/quic/core/qpack/qpack_decoded_headers_accumulator.h"
#include "net/third_party/quiche/src/quic/core/qpack/qpack_decoder.h"
#include "net/third_party/quiche/src/quic/core/qpack/qpack_decoder_test_utils.h"
#include "net/third_party/quiche/src/quic/core/qpack/qpack_encoder.h"
#include "net/third_party/quiche/src/quic/core/qpack/qpack_stream_sender_delegate.h"
#include "net/third_party/quiche/src/quic/core/qpack/qpack_utils.h"
#include "net/third_party/quiche/src/quic/core/qpack/value_splitting_header_list.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_fuzzed_data_provider.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_ptr_util.h"
#include "net/third_party/quiche/src/spdy/core/spdy_header_block.h"

namespace quic {
namespace test {

// Class to hold QpackEncoder and its DecoderStreamErrorDelegate.
class EncodingEndpoint {
 public:
  EncodingEndpoint(uint64_t maximum_dynamic_table_capacity,
                   uint64_t maximum_blocked_streams)
      : encoder_(&decoder_stream_error_delegate) {
    encoder_.SetMaximumDynamicTableCapacity(maximum_dynamic_table_capacity);
    encoder_.SetMaximumBlockedStreams(maximum_blocked_streams);
  }

  void set_qpack_stream_sender_delegate(QpackStreamSenderDelegate* delegate) {
    encoder_.set_qpack_stream_sender_delegate(delegate);
  }

  std::string EncodeHeaderList(QuicStreamId stream_id,
                               const spdy::SpdyHeaderBlock* header_list) {
    return encoder_.EncodeHeaderList(stream_id, header_list);
  }

 private:
  // DecoderStreamErrorDelegate implementation that crashes on error.
  class CrashingDecoderStreamErrorDelegate
      : public QpackEncoder::DecoderStreamErrorDelegate {
   public:
    ~CrashingDecoderStreamErrorDelegate() override = default;

    void OnDecoderStreamError(QuicStringPiece error_message) override {
      CHECK(false) << error_message;
    }
  };

  CrashingDecoderStreamErrorDelegate decoder_stream_error_delegate;
  QpackEncoder encoder_;
};

// Class to decode and verify a header block.
class VerifyingDecoder : public QpackDecodedHeadersAccumulator::Visitor {
 public:
  VerifyingDecoder(QuicStreamId stream_id,
                   QpackDecoder* qpack_decoder,
                   QuicHeaderList expected_header_list)
      : accumulator_(
            stream_id,
            qpack_decoder,
            this,
            /* max_header_list_size = */ std::numeric_limits<size_t>::max()),
        expected_header_list_(std::move(expected_header_list)) {}

  VerifyingDecoder(const VerifyingDecoder&) = delete;
  VerifyingDecoder& operator=(const VerifyingDecoder&) = delete;
  // VerifyingDecoder must not be moved because it passes |this| to
  // |accumulator_| upon construction.
  VerifyingDecoder(VerifyingDecoder&&) = delete;
  VerifyingDecoder& operator=(VerifyingDecoder&&) = delete;

  virtual ~VerifyingDecoder() = default;

  // QpackDecodedHeadersAccumulator::Visitor implementation.
  void OnHeadersDecoded(QuicHeaderList /*headers*/) override {}

  void OnHeaderDecodingError() override {
    CHECK(false) << accumulator_.error_message();
  }

  void Decode(QuicStringPiece data) {
    const bool success = accumulator_.Decode(data);
    CHECK(success) << accumulator_.error_message();
  }

  void EndHeaderBlock() {
    QpackDecodedHeadersAccumulator::Status status =
        accumulator_.EndHeaderBlock();

    CHECK(status != QpackDecodedHeadersAccumulator::Status::kError)
        << accumulator_.error_message();

    CHECK(status == QpackDecodedHeadersAccumulator::Status::kSuccess);

    // Compare resulting header list to original.
    CHECK(expected_header_list_ == accumulator_.quic_header_list());
  }

 private:
  QpackDecodedHeadersAccumulator accumulator_;
  QuicHeaderList expected_header_list_;
};

// Class that holds QpackDecoder and its EncoderStreamErrorDelegate, and creates
// and keeps VerifyingDecoders for each received header block until decoding is
// complete.
class DecodingEndpoint {
 public:
  DecodingEndpoint(uint64_t maximum_dynamic_table_capacity,
                   uint64_t maximum_blocked_streams)
      : decoder_(maximum_dynamic_table_capacity,
                 maximum_blocked_streams,
                 &encoder_stream_error_delegate_) {}

  ~DecodingEndpoint() {
    // All decoding must have been completed.
    CHECK(expected_header_lists_.empty());
    CHECK(verifying_decoders_.empty());
  }

  void set_qpack_stream_sender_delegate(QpackStreamSenderDelegate* delegate) {
    decoder_.set_qpack_stream_sender_delegate(delegate);
  }

  void AddExpectedHeaderList(QuicStreamId stream_id,
                             QuicHeaderList expected_header_list) {
    auto it = expected_header_lists_.lower_bound(stream_id);
    if (it == expected_header_lists_.end() || it->first != stream_id) {
      it = expected_header_lists_.insert(it, {stream_id, {}});
    }
    CHECK_EQ(stream_id, it->first);
    it->second.push(std::move(expected_header_list));
  }

  void OnHeaderBlockStart(QuicStreamId stream_id) {
    auto it = expected_header_lists_.find(stream_id);
    CHECK(it != expected_header_lists_.end());

    auto& header_list_queue = it->second;
    QuicHeaderList expected_header_list = std::move(header_list_queue.front());

    header_list_queue.pop();
    if (header_list_queue.empty()) {
      expected_header_lists_.erase(it);
    }

    auto verifying_decoder = QuicMakeUnique<VerifyingDecoder>(
        stream_id, &decoder_, std::move(expected_header_list));
    auto result =
        verifying_decoders_.insert({stream_id, std::move(verifying_decoder)});
    CHECK(result.second);
  }

  void OnHeaderBlockFragment(QuicStreamId stream_id, QuicStringPiece data) {
    auto it = verifying_decoders_.find(stream_id);
    CHECK(it != verifying_decoders_.end());
    it->second->Decode(data);
  }

  void OnHeaderBlockEnd(QuicStreamId stream_id) {
    auto it = verifying_decoders_.find(stream_id);
    CHECK(it != verifying_decoders_.end());
    it->second->EndHeaderBlock();
    auto result = verifying_decoders_.erase(stream_id);
    CHECK_EQ(1u, result);
  }

 private:
  // EncoderStreamErrorDelegate implementation that crashes on error.
  class CrashingEncoderStreamErrorDelegate
      : public QpackDecoder::EncoderStreamErrorDelegate {
   public:
    ~CrashingEncoderStreamErrorDelegate() override = default;

    void OnEncoderStreamError(QuicStringPiece error_message) override {
      CHECK(false) << error_message;
    }
  };

  CrashingEncoderStreamErrorDelegate encoder_stream_error_delegate_;
  QpackDecoder decoder_;

  // Expected header lists in order for each stream.
  std::map<QuicStreamId, std::queue<QuicHeaderList>> expected_header_lists_;

  // A VerifyingDecoder object keeps context necessary for asynchronously
  // decoding blocked header blocks.  It is destroyed as soon as it signals that
  // decoding is completed, which might happen synchronously within an
  // EndHeaderBlock() call.
  std::map<QuicStreamId, std::unique_ptr<VerifyingDecoder>> verifying_decoders_;
};

// Generate header list using fuzzer data.
spdy::SpdyHeaderBlock GenerateHeaderList(QuicFuzzedDataProvider* provider) {
  spdy::SpdyHeaderBlock header_list;
  uint8_t header_count = provider->ConsumeIntegral<uint8_t>();
  for (uint8_t header_index = 0; header_index < header_count; ++header_index) {
    if (provider->remaining_bytes() == 0) {
      // Do not add more headers if there is no more fuzzer data.
      break;
    }

    std::string name;
    std::string value;
    switch (provider->ConsumeIntegral<uint8_t>()) {
      case 0:
        // Static table entry with no header value.
        name = ":authority";
        break;
      case 1:
        // Static table entry with no header value, using non-empty header
        // value.
        name = ":authority";
        value = "www.example.org";
        break;
      case 2:
        // Static table entry with header value, using that header value.
        name = ":accept-encoding";
        value = "gzip, deflate";
        break;
      case 3:
        // Static table entry with header value, using empty header value.
        name = ":accept-encoding";
        break;
      case 4:
        // Static table entry with header value, using different, non-empty
        // header value.
        name = ":accept-encoding";
        value = "brotli";
        break;
      case 5:
        // Header name that has multiple entries in the static table,
        // using header value from one of them.
        name = ":method";
        value = "GET";
        break;
      case 6:
        // Header name that has multiple entries in the static table,
        // using empty header value.
        name = ":method";
        break;
      case 7:
        // Header name that has multiple entries in the static table,
        // using different, non-empty header value.
        name = ":method";
        value = "CONNECT";
        break;
      case 8:
        // Header name not in the static table, empty header value.
        name = "foo";
        value = "";
        break;
      case 9:
        // Header name not in the static table, non-empty fixed header value.
        name = "foo";
        value = "bar";
        break;
      case 10:
        // Header name not in the static table, fuzzed header value.
        name = "foo";
        value = provider->ConsumeRandomLengthString(128);
        break;
      case 11:
        // Another header name not in the static table, empty header value.
        name = "bar";
        value = "";
        break;
      case 12:
        // Another header name not in the static table, non-empty fixed header
        // value.
        name = "bar";
        value = "baz";
        break;
      case 13:
        // Another header name not in the static table, fuzzed header value.
        name = "bar";
        value = provider->ConsumeRandomLengthString(128);
        break;
      default:
        // Fuzzed header name and header value.
        name = provider->ConsumeRandomLengthString(128);
        value = provider->ConsumeRandomLengthString(128);
    }

    header_list.AppendValueOrAddHeader(name, value);
  }

  return header_list;
}

// Splits |*header_list| header values along '\0' or ';' separators.
QuicHeaderList SplitHeaderList(const spdy::SpdyHeaderBlock& header_list) {
  QuicHeaderList split_header_list;
  split_header_list.set_max_header_list_size(
      std::numeric_limits<size_t>::max());
  split_header_list.OnHeaderBlockStart();

  size_t total_size = 0;
  ValueSplittingHeaderList splitting_header_list(&header_list);
  for (const auto& header : splitting_header_list) {
    split_header_list.OnHeader(header.first, header.second);
    total_size += header.first.size() + header.second.size();
  }

  split_header_list.OnHeaderBlockEnd(total_size, total_size);

  return split_header_list;
}

// This fuzzer exercises QpackEncoder and QpackDecoder.  It should be able to
// cover all possible code paths of QpackEncoder.  However, since the resulting
// header block is always valid and is encoded in a particular way, this fuzzer
// is not expected to cover all code paths of QpackDecoder.  On the other hand,
// encoding then decoding is expected to result in the original header list, and
// this fuzzer checks for that.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  QuicFuzzedDataProvider provider(data, size);

  // Maximum 256 byte dynamic table.  Such a small size helps test draining
  // entries and eviction.
  const uint64_t maximum_dynamic_table_capacity =
      provider.ConsumeIntegral<uint8_t>();
  // Maximum 256 blocked streams.
  const uint64_t maximum_blocked_streams = provider.ConsumeIntegral<uint8_t>();

  // Set up encoder.
  NoopQpackStreamSenderDelegate encoder_stream_sender_delegate;
  EncodingEndpoint encoder(maximum_dynamic_table_capacity,
                           maximum_blocked_streams);
  encoder.set_qpack_stream_sender_delegate(&encoder_stream_sender_delegate);

  // Set up decoder.
  NoopQpackStreamSenderDelegate decoder_stream_sender_delegate;
  DecodingEndpoint decoder(maximum_dynamic_table_capacity,
                           maximum_blocked_streams);
  decoder.set_qpack_stream_sender_delegate(&decoder_stream_sender_delegate);

  while (provider.remaining_bytes() > 0) {
    const QuicStreamId stream_id = provider.ConsumeIntegral<uint8_t>();

    // Generate header list.
    spdy::SpdyHeaderBlock header_list = GenerateHeaderList(&provider);

    // Encode header list.
    std::string encoded_header_block =
        encoder.EncodeHeaderList(stream_id, &header_list);

    // Encoder splits |header_list| header values along '\0' or ';' separators.
    // Do the same here so that we get matching results.
    QuicHeaderList expected_header_list = SplitHeaderList(header_list);
    decoder.AddExpectedHeaderList(stream_id, std::move(expected_header_list));

    // Decode header block.
    decoder.OnHeaderBlockStart(stream_id);
    decoder.OnHeaderBlockFragment(stream_id, encoded_header_block);
    decoder.OnHeaderBlockEnd(stream_id);
  }

  return 0;
}

}  // namespace test
}  // namespace quic
