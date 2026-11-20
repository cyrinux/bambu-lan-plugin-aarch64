// H.264 over RTP (RFC 6184) to Annex-B access units, and the SPS fields
// needed for Bambu_StreamInfo (coded size after cropping).
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace bbl_lan {

struct H264Size {
    int width  = 0;
    int height = 0;
};

// nal: one SPS NAL unit (header byte included, no start code).
std::optional<H264Size> parse_h264_sps(const uint8_t* nal, size_t len);

struct AccessUnit {
    std::vector<uint8_t> annexb;   // 00 00 00 01 + NAL, repeated
    uint32_t             rtp_timestamp = 0;
    bool                 keyframe      = false; // contains an IDR slice
};

class H264Depacketizer {
public:
    explicit H264Depacketizer(int payload_type = -1) : m_payload_type(payload_type) {}

    // Out-of-band SPS/PPS (SDP sprop-parameter-sets); in-band ones replace them.
    void set_parameter_sets(const std::vector<std::vector<uint8_t>>& nals);

    // Feeds one RTP packet; appends every completed access unit to *out.
    // Access units are only emitted from the first IDR on, and after packet
    // loss again from the next IDR (a decoder cannot use the frames between).
    void push_rtp(const uint8_t* pkt, size_t len, std::vector<AccessUnit>* out);

    std::optional<H264Size> size() const { return m_size; }
    uint64_t                lost_packets() const { return m_lost; }

private:
    void add_nal(const uint8_t* nal, size_t len);
    void finish(std::vector<AccessUnit>* out);
    void remember_parameter_set(const uint8_t* nal, size_t len);

    int                     m_payload_type;
    std::vector<uint8_t>    m_sps, m_pps;
    std::optional<H264Size> m_size;

    std::vector<std::vector<uint8_t>> m_nals; // current access unit
    uint32_t             m_ts       = 0;
    bool                 m_have_ts  = false;
    bool                 m_corrupt  = false; // current AU lost a packet
    bool                 m_need_key = true;
    std::vector<uint8_t> m_fu;               // FU-A reassembly
    bool                 m_in_fu    = false;
    uint16_t             m_seq      = 0;
    bool                 m_have_seq = false;
    uint64_t             m_lost     = 0;
};

} // namespace bbl_lan
