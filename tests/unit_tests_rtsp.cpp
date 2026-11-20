// Unit tests for the RTSPS camera path: URL parsing, RTSP/SDP/auth helpers,
// SPS parsing and RTP depacketisation of a real H.264 stream
// (tests/fixtures/camera_320x180.h264: x264 High profile, 6 frames, GOP 3).
#include "source/bambu_url.hpp"
#include "source/h264.hpp"
#include "source/rtsp.hpp"

#include <cstdio>
#include <fstream>
#include <functional>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
int g_checks   = 0;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) {                                                                    \
            ++g_failures;                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                                 \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

using namespace bbl_lan;
using Nal = std::vector<uint8_t>;

std::vector<Nal> fixture_nals()
{
    std::ifstream        in(BBL_TEST_FIXTURES "/camera_320x180.h264", std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<Nal>     nals;
    size_t               start = std::string::npos;
    for (size_t i = 0; i + 3 <= d.size(); ++i) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (start != std::string::npos) {
                size_t end = i;
                while (end > start && d[end - 1] == 0)
                    --end; // 4-byte start code
                nals.emplace_back(d.begin() + static_cast<long>(start), d.begin() + static_cast<long>(end));
            }
            start = i + 3;
            i += 2;
        }
    }
    if (start != std::string::npos && start < d.size())
        nals.emplace_back(d.begin() + static_cast<long>(start), d.end());
    return nals;
}

uint8_t type_of(const Nal& n) { return n[0] & 0x1F; }

struct Packetizer {
    uint16_t seq = 1000;
    size_t   mtu = 200;

    std::vector<uint8_t> header(bool marker, uint32_t ts)
    {
        const uint16_t s = seq++;
        return {0x80, static_cast<uint8_t>((marker ? 0x80 : 0) | 96), uint8_t(s >> 8), uint8_t(s), uint8_t(ts >> 24),
                uint8_t(ts >> 16), uint8_t(ts >> 8), uint8_t(ts), 0, 0, 0, 1};
    }

    // One access unit: SPS+PPS as STAP-A, big NALs as FU-A, others single.
    std::vector<std::vector<uint8_t>> au(const std::vector<Nal>& nals, uint32_t ts)
    {
        std::vector<std::vector<uint8_t>> pkts;
        for (size_t i = 0; i < nals.size(); ++i) {
            const bool last = i + 1 == nals.size();
            const Nal& n    = nals[i];
            if (type_of(n) == 7 && i + 1 < nals.size() && type_of(nals[i + 1]) == 8) {
                auto p = header(false, ts);
                p.push_back(24);
                for (const Nal* x : {&n, &nals[i + 1]}) {
                    p.push_back(uint8_t(x->size() >> 8));
                    p.push_back(uint8_t(x->size()));
                    p.insert(p.end(), x->begin(), x->end());
                }
                pkts.push_back(p);
                ++i;
                continue;
            }
            if (n.size() <= mtu) {
                auto p = header(last, ts);
                p.insert(p.end(), n.begin(), n.end());
                pkts.push_back(p);
                continue;
            }
            for (size_t off = 1; off < n.size(); off += mtu) {
                const bool first = off == 1, end = off + mtu >= n.size();
                auto       p     = header(last && end, ts);
                p.push_back(static_cast<uint8_t>((n[0] & 0xE0) | 28));
                p.push_back(static_cast<uint8_t>((first ? 0x80 : 0) | (end ? 0x40 : 0) | type_of(n)));
                p.insert(p.end(), n.begin() + static_cast<long>(off), n.begin() + static_cast<long>(std::min(n.size(), off + mtu)));
                pkts.push_back(p);
            }
        }
        return pkts;
    }
};

// Groups the fixture into access units (each starts at SPS or a slice).
std::vector<std::vector<Nal>> fixture_access_units(bool keep_parameter_sets)
{
    std::vector<std::vector<Nal>> aus;
    std::vector<Nal>              cur;
    for (const Nal& n : fixture_nals()) {
        const uint8_t t = type_of(n);
        if (!keep_parameter_sets && (t == 7 || t == 8))
            continue;
        cur.push_back(n);
        if (t == 1 || t == 5) {
            aus.push_back(cur);
            cur.clear();
        }
    }
    return aus;
}

void test_rtsp_url()
{
    auto u = parse_rtsp_camera_url("bambu:///rtsps___bblp:12345678@192.168.1.60/streaming/live/1?proto=rtsps");
    CHECK(u.has_value());
    CHECK(u->secure);
    CHECK_EQ(u->host, "192.168.1.60");
    CHECK_EQ(u->port, 322);
    CHECK_EQ(u->user, "bblp");
    CHECK_EQ(u->password, "12345678");
    CHECK_EQ(u->path, "/streaming/live/1");
    CHECK_EQ(u->request_url(), "rtsps://192.168.1.60:322/streaming/live/1");

    u = parse_rtsp_camera_url("bambu:///rtsps___bblp:a@b:c@10.0.0.2:8322/streaming/live/1?proto=rtsps");
    CHECK(u.has_value());
    CHECK_EQ(u->password, "a@b:c");
    CHECK_EQ(u->host, "10.0.0.2");
    CHECK_EQ(u->port, 8322);

    u = parse_rtsp_camera_url("bambu:///rtsp___bblp:x@10.0.0.3/streaming/live/1?proto=rtsp");
    CHECK(u.has_value());
    CHECK(!u->secure);
    CHECK_EQ(u->port, 554);

    CHECK(!parse_rtsp_camera_url("bambu:///local/10.0.0.1.?port=6000").has_value());
    CHECK(!parse_rtsp_camera_url("bambu:///rtsps___bblp:x@/streaming").has_value());
    u = parse_rtsp_camera_url("bambu:///rtsps___bblp:x@[fe80::1]:8322/streaming/live/1?proto=rtsps");
    CHECK(u.has_value());
    CHECK_EQ(u->host, "fe80::1");
    CHECK_EQ(u->port, 8322);
    u = parse_rtsp_camera_url("bambu:///rtsps___bblp:x@[fe80::1]/streaming/live/1");
    CHECK(u.has_value() && u->host == "fe80::1" && u->port == 322);
    CHECK(!parse_rtsp_camera_url("bambu:///rtsps___bblp:x@[fe80::1/s").has_value());
    CHECK(!parse_rtsp_camera_url("bambu:///rtsps___bblp:x@h:99999/s").has_value());
}

void test_rtsp_messages()
{
    const std::string req = build_rtsp_request("DESCRIBE", "rtsps://h:322/s", 2, {"Accept: application/sdp"});
    CHECK_EQ(req, "DESCRIBE rtsps://h:322/s RTSP/1.0\r\nCSeq: 2\r\nUser-Agent: bambu-lan-plugin\r\n"
                  "Accept: application/sdp\r\n\r\n");

    const auto r = parse_rtsp_head("RTSP/1.0 401 Unauthorized\r\nCSeq: 2\r\n"
                                   "WWW-Authenticate: Digest realm=\"r\", nonce=\"n\"\r\n"
                                   "WWW-Authenticate: Basic realm=\"r\"\r\nContent-Length: 12\r\n\r\n");
    CHECK(r.has_value());
    CHECK_EQ(r->status, 401);
    CHECK_EQ(r->content_length(), 12u);
    CHECK_EQ(r->header("www-authenticate").rfind("Digest", 0), 0u);
    CHECK(!parse_rtsp_head("HTTP/1.1 200 OK\r\n\r\n").has_value());

    const auto s = parse_rtsp_head("RTSP/1.0 200 OK\r\nSession: 1A2B;timeout=60\r\n\r\n");
    CHECK(s.has_value());
    CHECK_EQ(s->header("session"), "1A2B;timeout=60");
}

void test_auth()
{
    // RFC 2617 section 3.5 example.
    const auto c = parse_www_authenticate("Digest realm=\"testrealm@host.com\", qop=\"auth,auth-int\", "
                                          "nonce=\"dcd98b7102dd2f0e8b11d0f600bfb0c093\", "
                                          "opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"");
    CHECK(c.has_value());
    CHECK_EQ(c->scheme, "digest");
    CHECK_EQ(c->params.at("realm"), "testrealm@host.com");
    const std::string v = authorization_value(*c, "Mufasa", "Circle Of Life", "GET", "/dir/index.html");
    CHECK(v.find("response=\"6629fae49393a05397450978507c4ef1\"") != std::string::npos);
    CHECK(v.find("opaque=\"5ccc069c403ebaf9f0171e9517f40e41\"") != std::string::npos);
    CHECK(v.find("nc=00000001") != std::string::npos);
    const std::string v2 = authorization_value(*c, "Mufasa", "Circle Of Life", "GET", "/dir/index.html", 2);
    CHECK(v2.find("nc=00000002") != std::string::npos);
    CHECK(v2.find("6629fae49393a05397450978507c4ef1") == std::string::npos); // nc is part of the hash

    const auto b = parse_www_authenticate("Basic realm=\"x\"");
    CHECK(b.has_value());
    CHECK_EQ(authorization_value(*b, "Aladdin", "open sesame", "DESCRIBE", "/"), "Basic QWxhZGRpbjpvcGVuIHNlc2FtZQ==");
    CHECK(!parse_www_authenticate("Bearer x").has_value());

    CHECK_EQ(base64_encode("ab"), "YWI=");
    CHECK_EQ(base64_encode("abc"), "YWJj");
    const auto d = base64_decode("Z0LAH9kA");
    CHECK_EQ(d.size(), 6u);
    CHECK_EQ(d[0], 0x67);
    CHECK_EQ(md5_hex(""), "d41d8cd98f00b204e9800998ecf8427e");
}

void test_sdp()
{
    const std::string sdp = "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\ns=live\r\na=control:*\r\nt=0 0\r\n"
                            "m=audio 0 RTP/AVP 97\r\na=control:track0\r\n"
                            "m=video 0 RTP/AVP 96\r\na=rtpmap:96 H264/90000\r\n"
                            "a=fmtp:96 packetization-mode=1;profile-level-id=640028;sprop-parameter-sets=Z2QAKA==,aO48gA==\r\n"
                            "a=framerate:30\r\na=control:track1\r\n";
    const auto t = parse_sdp_video(sdp);
    CHECK(t.has_value());
    CHECK_EQ(t->payload_type, 96);
    CHECK_EQ(t->encoding, "H264");
    CHECK_EQ(t->control, "track1");
    CHECK_EQ(t->session_control, "*");
    CHECK_EQ(t->frame_rate, 30);
    CHECK_EQ(t->parameter_sets.size(), 2u);
    CHECK_EQ(t->parameter_sets[0][0] & 0x1F, 7);
    CHECK_EQ(t->parameter_sets[1][0] & 0x1F, 8);
    CHECK(!parse_sdp_video("v=0\r\nm=audio 0 RTP/AVP 0\r\n").has_value());

    CHECK_EQ(resolve_control("rtsps://h:322/streaming/live/1/", "track1"), "rtsps://h:322/streaming/live/1/track1");
    CHECK_EQ(resolve_control("rtsps://h:322/streaming/live/1", "track1"), "rtsps://h:322/streaming/live/1/track1");
    CHECK_EQ(resolve_control("rtsps://h:322/s", "*"), "rtsps://h:322/s");
    CHECK_EQ(resolve_control("rtsps://h:322/s", "rtsps://h:322/other"), "rtsps://h:322/other");
}

void test_sps()
{
    const auto nals = fixture_nals();
    CHECK(!nals.empty());
    if (nals.empty())
        return;
    CHECK_EQ(type_of(nals[0]), 7);
    const auto size = parse_h264_sps(nals[0].data(), nals[0].size());
    CHECK(size.has_value());
    if (size) {
        CHECK_EQ(size->width, 320);
        CHECK_EQ(size->height, 180); // 192 coded, cropped
    }
    const uint8_t not_sps[] = {0x68, 0xEE, 0x3C, 0x80};
    CHECK(!parse_h264_sps(not_sps, sizeof(not_sps)).has_value());
}

void test_depacketize()
{
    // In-band parameter sets, FU-A fragmentation and STAP-A.
    {
        Packetizer              pk;
        H264Depacketizer        d(96);
        std::vector<AccessUnit> out;
        uint32_t                ts = 0;
        for (const auto& au : fixture_access_units(true)) {
            for (const auto& p : pk.au(au, ts))
                d.push_rtp(p.data(), p.size(), &out);
            ts += 6000;
        }
        CHECK_EQ(out.size(), 6u);
        CHECK(d.size().has_value() && d.size()->width == 320);
        if (out.size() == 6) {
            CHECK(out[0].keyframe);
            CHECK(!out[1].keyframe);
            CHECK(out[3].keyframe);
            CHECK_EQ(out[1].rtp_timestamp, 6000u);
            CHECK_EQ(out[0].annexb[4] & 0x1F, 7); // starts with SPS
        }
    }
    // Parameter sets only out of band (SDP): inserted before every IDR.
    {
        Packetizer              pk;
        H264Depacketizer        d(96);
        std::vector<AccessUnit> out;
        std::vector<Nal>        ps;
        for (const auto& n : fixture_nals())
            if ((type_of(n) == 7 || type_of(n) == 8) && ps.size() < 2)
                ps.push_back(n);
        d.set_parameter_sets(ps);
        uint32_t ts = 0;
        for (const auto& au : fixture_access_units(false)) {
            for (const auto& p : pk.au(au, ts))
                d.push_rtp(p.data(), p.size(), &out);
            ts += 6000;
        }
        CHECK_EQ(out.size(), 6u);
        if (out.size() == 6) {
            CHECK_EQ(out[0].annexb[4] & 0x1F, 7);
            CHECK_EQ(out[3].annexb[4] & 0x1F, 7);
            CHECK(out[1].annexb[4] != 0x67);
        }
    }
    // A lost packet in frame 2: frames 2 and 3 are dropped, output resumes at
    // the next IDR (frame 4).
    {
        Packetizer              pk;
        H264Depacketizer        d(96);
        std::vector<AccessUnit> out;
        uint32_t                ts = 0;
        int                     au_index = 0;
        for (const auto& au : fixture_access_units(true)) {
            const auto pkts = pk.au(au, ts);
            for (size_t i = 0; i < pkts.size(); ++i) {
                if (au_index == 1 && i == 0)
                    continue; // lose the first packet of frame 2
                d.push_rtp(pkts[i].data(), pkts[i].size(), &out);
            }
            ts += 6000;
            ++au_index;
        }
        CHECK_EQ(d.lost_packets(), 1u);
        CHECK_EQ(out.size(), 4u); // frames 1, 4, 5, 6
        if (out.size() == 4) {
            CHECK(out[0].keyframe);
            CHECK(out[1].keyframe);
            CHECK_EQ(out[1].rtp_timestamp, 18000u);
        }
    }
    // Wrong payload type and truncated packets are ignored.
    {
        H264Depacketizer        d(96);
        std::vector<AccessUnit> out;
        const uint8_t           other[] = {0x80, 0xE1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0x65, 0x88};
        const uint8_t           tiny[]  = {0x80, 0xE0, 0};
        d.push_rtp(other, sizeof(other), &out);
        d.push_rtp(tiny, sizeof(tiny), &out);
        CHECK(out.empty());
    }
}

} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"rtsp_url", test_rtsp_url}, {"rtsp_messages", test_rtsp_messages}, {"auth", test_auth},
        {"sdp", test_sdp},           {"sps", test_sps},                     {"depacketize", test_depacketize},
    };
    for (const auto& [name, fn] : tests) {
        const int before = g_failures;
        fn();
        std::printf("[%s] %s\n", g_failures == before ? " OK " : "FAIL", name);
    }
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
