/* -*-  Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil; -*- */

// Copyright (c) 2020 Centre Tecnologic de Telecomunicacions de Catalunya (CTTC)
//
// SPDX-License-Identifier: GPL-2.0-only

#include "nr-sl-sci-f2a-header.h"

#include <ns3/log.h>

#include <algorithm>

namespace ns3
{

NS_OBJECT_ENSURE_REGISTERED(NrSlSciF2aHeader);
NS_LOG_COMPONENT_DEFINE("NrSlSciF2aHeader");

NrSlSciF2aHeader::NrSlSciF2aHeader()
{
}

NrSlSciF2aHeader::~NrSlSciF2aHeader()
{
}

/**
 * TS 38.212 Table Table 8.4.1.1-1 specifies the values for cast type indicator.
 */
std::vector<NrSlSciF2aHeader::CastTypeIndicator_t> NrSlSciF2aHeader::m_allowedCastType = {Broadcast,
                                                                                          Groupcast,
                                                                                          Unicast,
                                                                                          GroupcastOnlyNack};

TypeId
NrSlSciF2aHeader::GetTypeId()
{
    static TypeId tid = TypeId("ns3::NrSlSciF2aHeader")
                            .SetParent<NrSlSciF2Header>()
                            .AddConstructor<NrSlSciF2aHeader>();
    return tid;
}

TypeId
NrSlSciF2aHeader::GetInstanceTypeId() const
{
    return GetTypeId();
}

void
NrSlSciF2aHeader::SetHarqFeedbackIndicator(uint8_t harqFeedbackIndicator)
{
    m_harqFeedbackIndicator = harqFeedbackIndicator;
}

void
NrSlSciF2aHeader::SetCastType(uint8_t castType)
{
    NS_ASSERT(std::find(m_allowedCastType.begin(), m_allowedCastType.end(), castType) !=
              m_allowedCastType.end());
    m_castType = castType;
}

void
NrSlSciF2aHeader::SetCsiReq(uint8_t csiReq)
{
    m_csiReq = csiReq;
}

uint8_t
NrSlSciF2aHeader::GetHarqFeedbackIndicator() const
{
    return m_harqFeedbackIndicator;
}

uint8_t
NrSlSciF2aHeader::GetCastType() const
{
    return m_castType;
}

uint8_t
NrSlSciF2aHeader::GetCsiReq() const
{
    return m_csiReq;
}

void
NrSlSciF2aHeader::SetNdiBit(uint8_t ndiBit)
{
    m_ndiBit = ndiBit & 0x1;
}

uint8_t
NrSlSciF2aHeader::GetNdiBit() const
{
    return m_ndiBit;
}

void
NrSlSciF2aHeader::Print(std::ostream& os) const
{
    NS_LOG_FUNCTION(this);
    NrSlSciF2Header::Print(os);
    os << ", HARQ feedback enabled/disabled indicator " << +m_harqFeedbackIndicator
       << ", Cast type indicator " << +m_castType << ", Channel state information request "
       << +m_csiReq << ", NDI bit " << +m_ndiBit;
}

uint32_t
NrSlSciF2aHeader::GetAirSize() const
{
    return NrSlSciF2Header::GetSerializedSize() + 1;
}

bool
NrSlSciF2aHeader::HasCoopInfo() const
{
    return m_coop.helperId != 0 || m_coop.relayedFromRnti != 0;
}

uint32_t
NrSlSciF2aHeader::GetSerializedSize() const
{
    // Cooperative side information (4 + 2 + 2*8 + 2*1 + 1 + 1 + 4 + 2 + 4*1 + 1 + 1 + 8 bytes) is
    // appended only when present, flagged by a padding bit: without it the header
    // is byte-identical to the stock SCI-2A
    return GetAirSize() + (HasCoopInfo() ? 46 : 0);
}

void
NrSlSciF2aHeader::Serialize(Buffer::Iterator start) const
{
    Buffer::Iterator i = start;

    NrSlSciF2aHeader::PreSerialize(i);

    uint8_t scif02a = 0;

    scif02a = (m_harqFeedbackIndicator & 0x1);
    scif02a = (m_castType & 0x3) | (scif02a << 2);
    scif02a = (m_csiReq & 0x1) | (scif02a << 1);

    // now 4 bits of padding; the lowest one flags the cooperative side information, the
    // next one carries the 3GPP NDI bit (1 bit of the real SCI-2A: the air size is unchanged)
    uint8_t padding = (HasCoopInfo() ? 0x01 : 0x00) | ((m_ndiBit & 0x1) << 1);
    scif02a = (padding & 0x0F) | (scif02a << 4);
    i.WriteU8(scif02a);
    if (!HasCoopInfo())
    {
        return;
    }

    i.WriteU32(m_coop.helperId);
    i.WriteU16(m_coop.relayedFromRnti);
    i.WriteU64(m_coop.relaySlot[0]);
    i.WriteU64(m_coop.relaySlot[1]);
    i.WriteU8(m_coop.relaySbStart[0]);
    i.WriteU8(m_coop.relaySbStart[1]);
    i.WriteU8(m_coop.sbLength);
    i.WriteU8(m_coop.mcs);
    i.WriteU32(m_coop.tbSize);
    i.WriteU16(m_coop.pscchRbs);
    i.WriteU8(m_coop.pscchSymStart);
    i.WriteU8(m_coop.pscchSymLength);
    i.WriteU8(m_coop.psschSymStart);
    i.WriteU8(m_coop.psschSymLength);
    i.WriteU8(m_coop.ownRes);
    i.WriteU8(m_coop.relayTxMax);
    i.WriteU64(m_coop.deadline);
}

uint32_t
NrSlSciF2aHeader::Deserialize(Buffer::Iterator start)
{
    Buffer::Iterator i = start;

    NrSlSciF2aHeader::PreDeserialize(i);

    uint8_t scif02a = i.ReadU8();

    m_harqFeedbackIndicator = (scif02a >> 7) & 0x1;
    m_castType = (scif02a >> 5) & 0x3;
    m_csiReq = (scif02a >> 4) & 0x1;
    m_ndiBit = (scif02a >> 1) & 0x1;
    m_coop = CoopInfo{};
    if (!(scif02a & 0x01))
    {
        return GetSerializedSize();
    }

    m_coop.helperId = i.ReadU32();
    m_coop.relayedFromRnti = i.ReadU16();
    m_coop.relaySlot[0] = i.ReadU64();
    m_coop.relaySlot[1] = i.ReadU64();
    m_coop.relaySbStart[0] = i.ReadU8();
    m_coop.relaySbStart[1] = i.ReadU8();
    m_coop.sbLength = i.ReadU8();
    m_coop.mcs = i.ReadU8();
    m_coop.tbSize = i.ReadU32();
    m_coop.pscchRbs = i.ReadU16();
    m_coop.pscchSymStart = i.ReadU8();
    m_coop.pscchSymLength = i.ReadU8();
    m_coop.psschSymStart = i.ReadU8();
    m_coop.psschSymLength = i.ReadU8();
    m_coop.ownRes = i.ReadU8();
    m_coop.relayTxMax = i.ReadU8();
    m_coop.deadline = i.ReadU64();

    return GetSerializedSize();
}

bool
NrSlSciF2aHeader::operator==(const NrSlSciF2aHeader& b) const
{
    return m_harqId == b.m_harqId && m_ndi == b.m_ndi && m_rv == b.m_rv && m_srcId == b.m_srcId &&
           m_dstId == b.m_dstId && m_csiReq == b.m_csiReq && m_castType == b.m_castType &&
           m_harqFeedbackIndicator == b.m_harqFeedbackIndicator && m_ndiBit == b.m_ndiBit;
}

void
NrSlSciF2aHeader::SetHelperId(uint32_t helperId)
{
    m_coop.helperId = helperId;
}

uint32_t
NrSlSciF2aHeader::GetHelperId() const
{
    return m_coop.helperId;
}

void
NrSlSciF2aHeader::SetCoopInfo(const CoopInfo& info)
{
    m_coop = info;
}

const NrSlSciF2aHeader::CoopInfo&
NrSlSciF2aHeader::GetCoopInfo() const
{
    return m_coop;
}

} // namespace ns3
