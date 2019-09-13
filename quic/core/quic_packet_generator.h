// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Responsible for generating packets on behalf of a QuicConnection.
// Packets are serialized just-in-time.
// Ack and Feedback frames will be requested from the Connection
// just-in-time.  When a packet needs to be sent, the Generator
// will serialize a packet and pass it to QuicConnection::SendOrQueuePacket()
//
// The Generator's mode of operation is controlled by two conditions:
//
// 1) Is the Delegate writable?
//
// If the Delegate is not writable, then no operations will cause
// a packet to be serialized.  In particular:
// * SetShouldSendAck will simply record that an ack is to be sent.
// * AddControlFrame will enqueue the control frame.
// * ConsumeData will do nothing.
//
// If the Delegate is writable, then the behavior depends on the second
// condition:
//
// 2) Is the Generator in batch mode?
//
// If the Generator is NOT in batch mode, then each call to a write
// operation will serialize one or more packets.  The contents will
// include any previous queued frames.  If an ack should be sent
// but has not been sent, then the Delegate will be asked to create
// an Ack frame which will then be included in the packet.  When
// the write call completes, the current packet will be serialized
// and sent to the Delegate, even if it is not full.
//
// If the Generator is in batch mode, then each write operation will
// add data to the "current" packet.  When the current packet becomes
// full, it will be serialized and sent to the packet.  When batch
// mode is ended via |FinishBatchOperations|, the current packet
// will be serialzied, even if it is not full.

#ifndef QUICHE_QUIC_CORE_QUIC_PACKET_GENERATOR_H_
#define QUICHE_QUIC_CORE_QUIC_PACKET_GENERATOR_H_

#include <cstddef>
#include <cstdint>
#include <list>

#include "net/third_party/quiche/src/quic/core/quic_packet_creator.h"
#include "net/third_party/quiche/src/quic/core/quic_pending_retransmission.h"
#include "net/third_party/quiche/src/quic/core/quic_sent_packet_manager.h"
#include "net/third_party/quiche/src/quic/core/quic_types.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_export.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_mem_slice_span.h"

namespace quic {

namespace test {
class QuicPacketGeneratorPeer;
}  // namespace test

class QUIC_EXPORT_PRIVATE QuicPacketGenerator {
 public:
  class QUIC_EXPORT_PRIVATE DelegateInterface
      : public QuicPacketCreator::DelegateInterface {
   public:
    ~DelegateInterface() override {}
    // Consults delegate whether a packet should be generated.
    virtual bool ShouldGeneratePacket(HasRetransmittableData retransmittable,
                                      IsHandshake handshake) = 0;
    // Called when there is data to be sent. Retrieves updated ACK frame from
    // the delegate.
    virtual const QuicFrames MaybeBundleAckOpportunistically() = 0;
  };

  QuicPacketGenerator(QuicConnectionId server_connection_id,
                      QuicFramer* framer,
                      QuicRandom* random_generator,
                      DelegateInterface* delegate);
  QuicPacketGenerator(const QuicPacketGenerator&) = delete;
  QuicPacketGenerator& operator=(const QuicPacketGenerator&) = delete;

  ~QuicPacketGenerator();

  // Consumes retransmittable control |frame|. Returns true if the frame is
  // successfully consumed. Returns false otherwise.
  bool ConsumeRetransmittableControlFrame(const QuicFrame& frame);

  // Given some data, may consume part or all of it and pass it to the
  // packet creator to be serialized into packets. If not in batch
  // mode, these packets will also be sent during this call.
  // When |state| is FIN_AND_PADDING, random padding of size [1, 256] will be
  // added after stream frames. If current constructed packet cannot
  // accommodate, the padding will overflow to the next packet(s).
  QuicConsumedData ConsumeData(QuicStreamId id,
                               size_t write_length,
                               QuicStreamOffset offset,
                               StreamSendingState state);

  // Consumes data for CRYPTO frames sent at |level| starting at |offset| for a
  // total of |write_length| bytes, and returns the number of bytes consumed.
  // The data is passed into the packet creator and serialized into one or more
  // packets.
  size_t ConsumeCryptoData(EncryptionLevel level,
                           size_t write_length,
                           QuicStreamOffset offset);

  // Sends as many data only packets as allowed by the send algorithm and the
  // available iov.
  // This path does not support padding, or bundling pending frames.
  // In case we access this method from ConsumeData, total_bytes_consumed
  // keeps track of how many bytes have already been consumed.
  QuicConsumedData ConsumeDataFastPath(QuicStreamId id,
                                       size_t write_length,
                                       QuicStreamOffset offset,
                                       bool fin,
                                       size_t total_bytes_consumed);

  // Generates an MTU discovery packet of specified size.
  void GenerateMtuDiscoveryPacket(QuicByteCount target_mtu);

  // Indicates whether packet flusher is currently attached.
  bool PacketFlusherAttached() const;
  // Attaches packet flusher.
  void AttachPacketFlusher();
  // Flushes everything, including current open packet and pending padding.
  void Flush();

  // Flushes current open packet.
  void FlushAllQueuedFrames();

  // Returns true if there are frames pending to be serialized.
  bool HasPendingFrames() const;

  // Makes the framer not serialize the protocol version in sent packets.
  void StopSendingVersion();

  // SetDiversificationNonce sets the nonce that will be sent in each public
  // header of packets encrypted at the initial encryption level. Should only
  // be called by servers.
  void SetDiversificationNonce(const DiversificationNonce& nonce);

  // Creates a version negotiation packet which supports |supported_versions|.
  std::unique_ptr<QuicEncryptedPacket> SerializeVersionNegotiationPacket(
      bool ietf_quic,
      bool use_length_prefix,
      const ParsedQuicVersionVector& supported_versions);

  // Creates a connectivity probing packet.
  OwningSerializedPacketPointer SerializeConnectivityProbingPacket();

  // Create connectivity probing request and response packets using PATH
  // CHALLENGE and PATH RESPONSE frames, respectively.
  // SerializePathChallengeConnectivityProbingPacket will pad the packet to be
  // MTU bytes long.
  OwningSerializedPacketPointer SerializePathChallengeConnectivityProbingPacket(
      QuicPathFrameBuffer* payload);

  // If |is_padded| is true then SerializePathResponseConnectivityProbingPacket
  // will pad the packet to be MTU bytes long, else it will not pad the packet.
  // |payloads| is cleared.
  OwningSerializedPacketPointer SerializePathResponseConnectivityProbingPacket(
      const QuicDeque<QuicPathFrameBuffer>& payloads,
      const bool is_padded);

  // Re-serializes frames with the original packet's packet number length.
  // Used for retransmitting packets to ensure they aren't too long.
  void ReserializeAllFrames(const QuicPendingRetransmission& retransmission,
                            char* buffer,
                            size_t buffer_len);

  // Update the packet number length to use in future packets as soon as it
  // can be safely changed.
  void UpdatePacketNumberLength(QuicPacketNumber least_packet_awaited_by_peer,
                                QuicPacketCount max_packets_in_flight);

  // Skip |count| packet numbers.
  void SkipNPacketNumbers(QuicPacketCount count,
                          QuicPacketNumber least_packet_awaited_by_peer,
                          QuicPacketCount max_packets_in_flight);

  // Set the minimum number of bytes for the server connection id length;
  void SetServerConnectionIdLength(uint32_t length);

  // Sets the encrypter to use for the encryption level.
  void SetEncrypter(EncryptionLevel level,
                    std::unique_ptr<QuicEncrypter> encrypter);

  // Returns true if there are control frames or current constructed packet has
  // pending retransmittable frames.
  bool HasRetransmittableFrames() const;

  // Returns true if current constructed packet has pending stream frames for
  // stream |id|.
  bool HasPendingStreamFramesOfStream(QuicStreamId id) const;

  // Sets the encryption level that will be applied to new packets.
  void set_encryption_level(EncryptionLevel level);

  // packet number of the last created packet, or 0 if no packets have been
  // created.
  QuicPacketNumber packet_number() const;

  // Returns the maximum length a current packet can actually have.
  QuicByteCount GetCurrentMaxPacketLength() const;

  // Set maximum packet length in the creator immediately.  May not be called
  // when there are frames queued in the creator.
  void SetMaxPacketLength(QuicByteCount length);

  // Set transmission type of next constructed packets.
  void SetTransmissionType(TransmissionType type);

  // Sets the retry token to be sent over the wire in IETF Initial packets.
  void SetRetryToken(QuicStringPiece retry_token);

  // Allow/Disallow setting transmission type of next constructed packets.
  void SetCanSetTransmissionType(bool can_set_transmission_type);

  // Tries to add a message frame containing |message| and returns the status.
  MessageStatus AddMessageFrame(QuicMessageId message_id,
                                QuicMemSliceSpan message);

  // Called to flush ACK and STOP_WAITING frames, returns false if the flush
  // fails.
  bool FlushAckFrame(const QuicFrames& frames);

  // Returns the largest payload that will fit into a single MESSAGE frame.
  QuicPacketLength GetCurrentLargestMessagePayload() const;
  QuicPacketLength GetGuaranteedLargestMessagePayload() const;

  // Update the server connection ID used in outgoing packets.
  void SetServerConnectionId(QuicConnectionId server_connection_id);

  // Update the client connection ID used in outgoing packets.
  void SetClientConnectionId(QuicConnectionId client_connection_id);

  void set_debug_delegate(QuicPacketCreator::DebugDelegate* debug_delegate) {
    packet_creator_.set_debug_delegate(debug_delegate);
  }

  void set_fully_pad_crypto_hadshake_packets(bool new_value) {
    fully_pad_crypto_handshake_packets_ = new_value;
  }

  bool fully_pad_crypto_handshake_packets() const {
    return fully_pad_crypto_handshake_packets_;
  }

 private:
  friend class test::QuicPacketGeneratorPeer;

  // Adds a random amount of padding (between 1 to 256 bytes).
  void AddRandomPadding();

  // Sends remaining pending padding.
  // Pending paddings should only be sent when there is nothing else to send.
  void SendRemainingPendingPadding();

  // Called when there is data to be sent, Retrieves updated ACK frame from
  // delegate_ and flushes it.
  void MaybeBundleAckOpportunistically();

  DelegateInterface* delegate_;

  QuicPacketCreator packet_creator_;

  // Transmission type of the next serialized packet.
  TransmissionType next_transmission_type_;

  // True if packet flusher is currently attached.
  bool flusher_attached_;

  QuicRandom* random_generator_;

  // Whether crypto handshake packets should be fully padded.
  bool fully_pad_crypto_handshake_packets_;

  // Packet number of the first packet of a write operation. This gets set
  // when the out-most flusher attaches and gets cleared when the out-most
  // flusher detaches.
  QuicPacketNumber write_start_packet_number_;
};

}  // namespace quic

#endif  // QUICHE_QUIC_CORE_QUIC_PACKET_GENERATOR_H_
