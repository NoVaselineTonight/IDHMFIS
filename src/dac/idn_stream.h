#pragma once
// IdnStreamOutput — IDN-Stream UDP laser output driver (ILDA IDN-Stream F01).
//
// Implements both IDacOutput and IDac for unicast IDN-Stream delivery to a
// named host/IP on port 7255.  Wire format per ILDA IDN-Stream F01:
//   First chunk (CCLF=1): primary(4) + channel(8) + config(4) + GTS dict(12) + points
//   Continuation chunks : primary(4) + channel(8) + points
//
// All wire structures and constants come from idn_sender.h (no duplication).
// Default destination port: 7255 (ILDA registered).

#include "dac_interface.h"
#include "idn_sender.h"   // IdnPrimaryHeader, IdnChannelHeader, IdnChannelConfig,
                           // IdnPoint, all IDN constants, SockFd / kIdnInvalidSock

#include <mutex>
#include <string>

namespace idhmfis {

// ─────────────────────────────────────────────────────────────────────────────
//  IdnStreamOutput
// ─────────────────────────────────────────────────────────────────────────────
class IdnStreamOutput final : public IDacOutput, public IDac {
public:
    explicit IdnStreamOutput(std::string host,
                             uint16_t    port = kIdnDefaultPort);
    ~IdnStreamOutput() override;

    // ── IDacOutput interface ─────────────────────────────────────────────────
    bool        connect()                         override;
    void        disconnect()                      override;
    bool        is_connected()          const     override;
    int         negotiated_pps()        const     override;
    int         max_pps()               const     override { return kIdnMaxPPS; }
    bool        send_frame(const PointBuffer& pts,
                           int target_pps)        override;
    std::string id()                    const     override;
    std::string name()                  const     override;

    // ── IDac interface (delegates to IDacOutput semantics) ───────────────────
    bool        open()                            override { return connect(); }
    void        close()                           override { disconnect(); }
    bool        is_open()               const     override { return is_connected(); }
    DacStatus   status()                const     override;
    bool        set_point_rate(int pps)           override;
    int         max_point_rate()        const     override { return kIdnMaxPPS; }
    int         min_point_rate()        const     override { return kIdnMinPPS; }
    int         send_points(const PointBuffer&)   override;
    const char* type_name()             const     override { return "idn_stream"; }

private:
    static IdnPoint convert_point(const LaserPoint& p);
    bool send_packet(const IdnPoint* pts, int count,
                     uint32_t timestamp_us, bool include_config);

    std::string   host_;
    uint16_t      port_       = kIdnDefaultPort;
    int           point_rate_ = kIdnDefaultPPS;
    int           negotiated_ = kIdnDefaultPPS;

    SockFd        sock_       = kIdnInvalidSock;
    sockaddr_in   dest_addr_  = {};
    bool          connected_  = false;

    uint16_t      sequence_   = 0;

    mutable std::mutex mutex_;
};

} // namespace idhmfis
