/*
 * Copyright (c) 2017, Matias Fontanini
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above
 *   copyright notice, this list of conditions and the following disclaimer
 *   in the documentation and/or other materials provided with the
 *   distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include "ip_reassembler.h"

#include <tins/constants.h>
#include <tins/detail/pdu_helpers.h>
#include <tins/ip.h>

using std::make_pair;

namespace Tins {
namespace Internals {

static const std::chrono::microseconds fragment_timeout(2000000);

IPv4Stream2::IPv4Stream2()
    : received_size_(), total_size_(), received_end_(false) {}

void IPv4Stream2::add_fragment(const std::chrono::microseconds& timestamp,
                               IP* ip) {
    // handle timeout
    if (fragments_.size()) {
        if ((timestamp - last_timestamp_) > fragment_timeout) {
            // if we timed out, clear out all old packets
            received_size_ = 0;
            total_size_ = 0;
            received_end_ = false;
            fragments_.clear();
            first_fragment_ = IP();
        }
    }
    last_timestamp_ = timestamp;

    const uint16_t offset = extract_offset(ip);
    fragments_type::iterator it = fragments_.begin();
    while (it != fragments_.end() && offset > it->offset()) {
        ++it;
    }
    // Replace duplicates
    if (it != fragments_.end() && it->offset() == offset) {
        it = fragments_.erase(it);
        fragments_.insert(it, IPv4Fragment2(ip->inner_pdu(), offset));
    } else {
        fragments_.insert(it, IPv4Fragment2(ip->inner_pdu(), offset));
        received_size_ += ip->inner_pdu()->size();
    }
    // If the MF flag is not set, this is the end of the packet
    if ((ip->flags() & IP::MORE_FRAGMENTS) == 0) {
        total_size_ = offset + ip->inner_pdu()->size();
        received_end_ = true;
    }
    if (offset == 0) {
        // Release the inner PDU, store this first fragment and restore the
        // inner PDU
        PDU* inner_pdu = ip->release_inner_pdu();
        first_fragment_ = *ip;
        ip->inner_pdu(inner_pdu);
    }
}

bool IPv4Stream2::is_complete() const {
    // If we haven't received the last chunk of we haven't received all the
    // data, then we're not complete
    if (!received_end_ || received_size_ != total_size_) {
        return false;
    }
    // Make sure the first fragment has offset 0
    return fragments_.begin()->offset() == 0;
}

PDU* IPv4Stream2::allocate_pdu() const {
    PDU::serialization_type buffer;
    buffer.reserve(total_size_);
    // Check if we actually have all the data we need. Otherwise return nullptr;
    size_t expected = 0;
    for (fragments_type::const_iterator it = fragments_.begin();
         it != fragments_.end(); ++it) {
        if (expected != it->offset()) {
            return 0;
        }
        expected = it->offset() + it->payload().size();
        buffer.insert(buffer.end(), it->payload().begin(), it->payload().end());
    }
    return Internals::pdu_from_flag(
        static_cast<Constants::IP::e>(first_fragment_.protocol()),
        buffer.empty() ? 0 : &buffer[0], static_cast<uint32_t>(buffer.size()));
}

const IP& IPv4Stream2::first_fragment() const { return first_fragment_; }

uint16_t IPv4Stream2::extract_offset(const IP* ip) {
    return ip->fragment_offset() * 8;
}

}  // namespace Internals

IPv4Reassembler2::IPv4Reassembler2() : technique_(NONE) {}

IPv4Reassembler2::IPv4Reassembler2(OverlappingTechnique technique)
    : technique_(technique) {}

IPv4Reassembler2::PacketStatus IPv4Reassembler2::process(
    const std::chrono::microseconds& timestamp, PDU& pdu) {
    IP* ip = pdu.find_pdu<IP>();
    if (ip && ip->inner_pdu()) {
        // There's fragmentation
        if (ip->is_fragmented()) {
            key_type key = make_key(ip);
            // Delete old streams if too many build up, we only expect one per
            // lidar in practice We just want to keep from building up too much
            // memory If we didnt do this we could end up with 99 MB or so in
            // junk worst case
            if (streams_.size() > 100) {
                for (auto it = streams_.cbegin(); it != streams_.cend();) {
                    auto age = timestamp - it->second.last_timestamp_;
                    if (age > Tins::Internals::fragment_timeout) {
                        it = streams_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }
            // Create it or look it up, it's the same
            Internals::IPv4Stream2& stream = streams_[key];
            stream.add_fragment(timestamp, ip);
            if (stream.is_complete()) {
                PDU* pdu = stream.allocate_pdu();
                // Use all field values from the first fragment
                *ip = stream.first_fragment();

                // Erase this stream, since it's already assembled
                streams_.erase(key);
                // The packet is corrupt
                if (!pdu) {
                    return FRAGMENTED;
                }
                ip->inner_pdu(pdu);
                ip->fragment_offset(0);
                ip->flags(static_cast<IP::Flags>(0));
                return REASSEMBLED;
            } else {
                return FRAGMENTED;
            }
        }
    }
    return NOT_FRAGMENTED;
}

IPv4Reassembler2::key_type IPv4Reassembler2::make_key(const IP* ip) const {
    return make_pair(ip->id(),
                     make_address_pair(ip->src_addr(), ip->dst_addr()));
}

IPv4Reassembler2::address_pair IPv4Reassembler2::make_address_pair(
    IPv4Address addr1, IPv4Address addr2) const {
    if (addr1 < addr2) {
        return make_pair(addr1, addr2);
    } else {
        return make_pair(addr2, addr1);
    }
}

}  // namespace Tins
