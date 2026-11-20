#include "h264.hpp"

#include <algorithm>

namespace bbl_lan {
namespace {

constexpr uint8_t kNalIdr = 5, kNalSps = 7, kNalPps = 8, kStapA = 24, kFuA = 28;
constexpr size_t  kMaxAccessUnitBytes = 8 * 1024 * 1024;

uint8_t nal_type(const std::vector<uint8_t>& nal) { return nal.empty() ? 0 : nal[0] & 0x1F; }

// Exp-Golomb bit reader over an RBSP (emulation prevention bytes removed).
class BitReader {
public:
    explicit BitReader(std::vector<uint8_t> rbsp) : m_data(std::move(rbsp)) {}

    bool ok() const { return !m_overrun; }

    uint32_t u(int n)
    {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i)
            v = (v << 1) | bit();
        return v;
    }
    uint32_t ue()
    {
        int zeros = 0;
        while (bit() == 0 && !m_overrun && zeros < 32)
            ++zeros;
        if (zeros >= 32) {
            m_overrun = true;
            return 0;
        }
        return ((1u << zeros) - 1) + u(zeros);
    }
    int32_t se()
    {
        const uint32_t k = ue();
        return (k & 1) ? static_cast<int32_t>((k + 1) / 2) : -static_cast<int32_t>(k / 2);
    }

private:
    uint32_t bit()
    {
        if (m_pos >= m_data.size() * 8) {
            m_overrun = true;
            return 0;
        }
        const uint32_t b = (m_data[m_pos / 8] >> (7 - m_pos % 8)) & 1;
        ++m_pos;
        return b;
    }

    std::vector<uint8_t> m_data;
    size_t               m_pos     = 0;
    bool                 m_overrun = false;
};

std::vector<uint8_t> to_rbsp(const uint8_t* p, size_t len)
{
    std::vector<uint8_t> out;
    out.reserve(len);
    int zeros = 0;
    for (size_t i = 0; i < len; ++i) {
        if (zeros >= 2 && p[i] == 3) {
            zeros = 0;
            continue; // emulation prevention byte
        }
        zeros = p[i] == 0 ? zeros + 1 : 0;
        out.push_back(p[i]);
    }
    return out;
}

void skip_scaling_list(BitReader& br, int size)
{
    int last = 8, next = 8;
    for (int j = 0; j < size; ++j) {
        if (next != 0)
            next = (last + br.se() + 256) % 256;
        last = next == 0 ? last : next;
    }
}

bool has_chroma_info(uint32_t profile)
{
    switch (profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86: case 118: case 128: case 138: case 139: case 134: case 135:
        return true;
    default:
        return false;
    }
}

void append_annexb(std::vector<uint8_t>* out, const std::vector<uint8_t>& nal)
{
    static const uint8_t kStart[] = {0, 0, 0, 1};
    out->insert(out->end(), std::begin(kStart), std::end(kStart));
    out->insert(out->end(), nal.begin(), nal.end());
}

} // namespace

std::optional<H264Size> parse_h264_sps(const uint8_t* nal, size_t len)
{
    if (nal == nullptr || len < 4 || (nal[0] & 0x1F) != kNalSps)
        return std::nullopt;
    BitReader      br(to_rbsp(nal + 1, len - 1));
    const uint32_t profile = br.u(8);
    br.u(16); // constraint flags, level_idc
    br.ue();  // seq_parameter_set_id
    uint32_t chroma_format = 1;
    bool     separate      = false;
    if (has_chroma_info(profile)) {
        chroma_format = br.ue();
        if (chroma_format == 3)
            separate = br.u(1) != 0;
        br.ue(); // bit_depth_luma_minus8
        br.ue(); // bit_depth_chroma_minus8
        br.u(1); // qpprime_y_zero_transform_bypass_flag
        if (br.u(1)) { // seq_scaling_matrix_present_flag
            for (int i = 0; i < (chroma_format == 3 ? 12 : 8); ++i) {
                if (br.u(1))
                    skip_scaling_list(br, i < 6 ? 16 : 64);
            }
        }
    }
    br.ue(); // log2_max_frame_num_minus4
    const uint32_t poc_type = br.ue();
    if (poc_type == 0) {
        br.ue();
    } else if (poc_type == 1) {
        br.u(1);
        br.se();
        br.se();
        const uint32_t n = br.ue();
        for (uint32_t i = 0; i < n && i < 256; ++i)
            br.se();
    }
    br.ue(); // max_num_ref_frames
    br.u(1); // gaps_in_frame_num_value_allowed_flag
    const uint32_t width_mbs  = br.ue() + 1;
    const uint32_t height_map = br.ue() + 1;
    const uint32_t frame_mbs  = br.u(1);
    if (!frame_mbs)
        br.u(1); // mb_adaptive_frame_field_flag
    br.u(1);     // direct_8x8_inference_flag
    uint32_t crop[4] = {0, 0, 0, 0};
    if (br.u(1)) {
        for (auto& c : crop)
            c = br.ue();
    }
    if (!br.ok() || width_mbs > 1024 || height_map > 1024)
        return std::nullopt;

    const uint32_t chroma_array = separate ? 0 : chroma_format;
    const uint32_t crop_x       = chroma_array == 0 ? 1 : (chroma_array == 3 ? 1 : 2);
    const uint32_t crop_y       = (chroma_array == 1 ? 2 : 1) * (2 - frame_mbs);
    const int64_t  width        = int64_t(width_mbs) * 16 - int64_t(crop_x) * (crop[0] + crop[1]);
    const int64_t  height       = int64_t(2 - frame_mbs) * height_map * 16 - int64_t(crop_y) * (crop[2] + crop[3]);
    if (width <= 0 || height <= 0)
        return std::nullopt;
    return H264Size{static_cast<int>(width), static_cast<int>(height)};
}

void H264Depacketizer::set_parameter_sets(const std::vector<std::vector<uint8_t>>& nals)
{
    for (const auto& nal : nals)
        remember_parameter_set(nal.data(), nal.size());
}

void H264Depacketizer::remember_parameter_set(const uint8_t* nal, size_t len)
{
    if (len == 0)
        return;
    const uint8_t type = nal[0] & 0x1F;
    if (type == kNalSps) {
        m_sps.assign(nal, nal + len);
        if (const auto s = parse_h264_sps(nal, len))
            m_size = s;
    } else if (type == kNalPps) {
        m_pps.assign(nal, nal + len);
    }
}

void H264Depacketizer::add_nal(const uint8_t* nal, size_t len)
{
    if (len == 0)
        return;
    remember_parameter_set(nal, len);
    m_nals.emplace_back(nal, nal + len);
}

void H264Depacketizer::finish(std::vector<AccessUnit>* out)
{
    if (m_in_fu) { // a fragmented NAL never ended
        m_corrupt = true;
        m_in_fu   = false;
    }
    if (m_corrupt)
        m_need_key = true;
    if (m_nals.empty()) {
        m_corrupt = false;
        return;
    }
    const bool key = std::any_of(m_nals.begin(), m_nals.end(), [](const auto& n) { return nal_type(n) == kNalIdr; });
    const bool sps = std::any_of(m_nals.begin(), m_nals.end(), [](const auto& n) { return nal_type(n) == kNalSps; });
    if (!m_corrupt && key && !m_sps.empty() && !m_pps.empty())
        m_need_key = false;

    if (!m_need_key && !m_corrupt) {
        AccessUnit au;
        au.rtp_timestamp = m_ts;
        au.keyframe      = key;
        if (key && !sps) { // decoders get no extradata: every IDR carries SPS/PPS
            append_annexb(&au.annexb, m_sps);
            append_annexb(&au.annexb, m_pps);
        }
        for (const auto& nal : m_nals)
            append_annexb(&au.annexb, nal);
        if (au.annexb.size() <= kMaxAccessUnitBytes)
            out->push_back(std::move(au));
    }
    m_nals.clear();
    m_corrupt = false;
}

void H264Depacketizer::push_rtp(const uint8_t* pkt, size_t len, std::vector<AccessUnit>* out)
{
    if (pkt == nullptr || len < 12 || (pkt[0] >> 6) != 2)
        return;
    const bool     padding = (pkt[0] & 0x20) != 0;
    const bool     ext     = (pkt[0] & 0x10) != 0;
    const size_t   cc      = pkt[0] & 0x0F;
    const bool     marker  = (pkt[1] & 0x80) != 0;
    const int      pt      = pkt[1] & 0x7F;
    const uint16_t seq     = static_cast<uint16_t>((pkt[2] << 8) | pkt[3]);
    const uint32_t ts      = (uint32_t(pkt[4]) << 24) | (uint32_t(pkt[5]) << 16) | (uint32_t(pkt[6]) << 8) | pkt[7];
    if (m_payload_type >= 0 && pt != m_payload_type)
        return;

    size_t off = 12 + 4 * cc;
    if (ext) {
        if (off + 4 > len)
            return;
        off += 4 + 4 * ((size_t(pkt[off + 2]) << 8) | pkt[off + 3]);
    }
    size_t end = len;
    if (padding) {
        const size_t pad = pkt[len - 1];
        if (pad > len)
            return;
        end -= pad;
    }
    if (off >= end)
        return;

    bool lost = false;
    if (m_have_seq) {
        const uint16_t gap = static_cast<uint16_t>(seq - m_seq - 1);
        if (gap >= 0x8000)
            return; // duplicate or late packet
        if (gap != 0) {
            m_lost += gap;
            m_corrupt = true;
            lost      = true;
        }
    }
    m_seq      = seq;
    m_have_seq = true;
    if (m_have_ts && ts != m_ts) {
        finish(out); // the previous access unit lost its marker packet
        // Lost packets between two timestamps may belong to either unit.
        m_corrupt = lost;
    }
    if (lost)
        m_in_fu = false;
    m_ts      = ts;
    m_have_ts = true;

    const uint8_t* p    = pkt + off;
    const size_t   n    = end - off;
    const uint8_t  type = p[0] & 0x1F;
    if (type >= 1 && type <= 23) {
        add_nal(p, n);
    } else if (type == kStapA) {
        for (size_t i = 1; i + 2 <= n;) {
            const size_t sz = (size_t(p[i]) << 8) | p[i + 1];
            i += 2;
            if (i + sz > n)
                break;
            add_nal(p + i, sz);
            i += sz;
        }
    } else if (type == kFuA && n >= 2) {
        const bool start = (p[1] & 0x80) != 0, stop = (p[1] & 0x40) != 0;
        if (start) {
            m_fu.assign(1, static_cast<uint8_t>((p[0] & 0xE0) | (p[1] & 0x1F)));
            m_in_fu = true;
        } else if (!m_in_fu) {
            m_corrupt = true; // lost the first fragment
        }
        if (m_in_fu) {
            m_fu.insert(m_fu.end(), p + 2, p + n);
            if (m_fu.size() > kMaxAccessUnitBytes) {
                m_in_fu   = false;
                m_corrupt = true;
            } else if (stop) {
                m_in_fu = false;
                add_nal(m_fu.data(), m_fu.size());
            }
        }
    }
    if (marker)
        finish(out);
}

} // namespace bbl_lan
