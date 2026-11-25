/* -*-  Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil; -*- */

// Copyright (c) 2020 Centre Tecnologic de Telecomunicacions de Catalunya (CTTC)
//
// SPDX-License-Identifier: GPL-2.0-only

#ifndef NR_SL_SCI_F2A_HEADER_H
#define NR_SL_SCI_F2A_HEADER_H

#include "nr-sl-sci-f2-header.h"

#include <iostream>

namespace ns3
{

/**
 * \ingroup nr
 * \brief The packet header for the NR Sidelink Control Information (SCI)
 * format 2A (TS 38.212 Sec 8.3 Rel 16). The following includes two fields.
 *
 * - m_castTypeIndicator [2 bits]
 * - m_csiReq [1 bit]
 *
 * The total size of this header is 5 bytes. 4 bytes of \link NrSlSciF2aHeader \endlink
 * , 3 bits of above fields plus 5 bits of zero padding.
 *
 * The use of this header is only possible if:
 * - All the mandatory fields in \link NrSlSciF2aHeader \endlink are set using
 *   their respective setter methods. Otherwise, serialization will hit an assert.
 *
 * \see NrSlSciF2Header
 */
class NrSlSciF2aHeader : public NrSlSciF2Header
{
  public:
    /**
     * \brief Constructor
     *
     * Creates an SCI header
     */
    NrSlSciF2aHeader();
    ~NrSlSciF2aHeader() override;

    /// HARQ feedback enabled/disabled indicator enumeration
    enum HarqFeedbackIndicator_t : uint8_t
    {
        Disabled = 0,
        Enabled = 1
    };

    /// Cast type indicator enumeration
    enum CastTypeIndicator_t : uint8_t
    {
        Broadcast = 0,
        Groupcast = 1,
        Unicast = 2,
        GroupcastOnlyNack = 3,
    };

    /**
     * \brief Set the HARQ feedback enabled/disabled indicator field
     *
     * \param harqFeedbackIndicator  Whether HARQ feedback is enabled
     */
    void SetHarqFeedbackIndicator(uint8_t harqFeedbackIndicator);
    /**
     * \brief Set the cast type indicator field
     *
     * \param castType The cast type indicator value
     */
    void SetCastType(uint8_t castType);
    /**
     * \brief Set the Channel State Information request flag
     *
     * \param csiReq The channel state information request flag
     */
    void SetCsiReq(uint8_t csiReq);
    /**
     * \brief Set the 3GPP new data indicator bit (TS 38.212 8.4.1.1): toggled by the
     *        transmitter at each new TB of the HARQ process. GetNdi() is the simulator's
     *        "new data" flag (1 on the first transmission of a TB only).
     *
     * \param ndiBit The NDI bit (0 or 1)
     */
    void SetNdiBit(uint8_t ndiBit);

    /**
     * Cooperative HARQ (chained reservations) side information.
     *
     * Carried in the simulated SCI-2A so a designated helper can relay a TB in
     * the reserved retransmission slots the leader gives up, and so receivers
     * can soft-combine the helper's copy with the leader's HARQ process. In a
     * real system the helper would read the slots / subchannels / MCS from the
     * leader's SCI-1A; only the helper id would need a few SCI-2 bits. These
     * fields therefore do NOT count towards the SCI-2A size used by the error
     * model: see GetAirSize().
     */
    struct CoopInfo
    {
        uint32_t helperId{0};       //!< RNTI of the designated helper (0 = none)
        uint16_t relayedFromRnti{0}; //!< set by a helper: RNTI of the original TX (0 = own TB)
        uint64_t relaySlot[2]{0, 0}; //!< SfnSf encodings of the slots handed to the helper (0 = none)
        uint8_t relaySbStart[2]{0, 0}; //!< start subchannel of those slots
        uint8_t sbLength{0};         //!< subchannels of the TB
        uint8_t mcs{0};              //!< MCS of the TB
        uint32_t tbSize{0};          //!< TB size (bytes)
        uint16_t pscchRbs{0};        //!< PSCCH PRBs of the pool
        uint8_t pscchSymStart{0};    //!< PSCCH first symbol
        uint8_t pscchSymLength{0};   //!< PSCCH symbols
        uint8_t psschSymStart{0};    //!< PSSCH first symbol
        uint8_t psschSymLength{0};   //!< PSSCH symbols in a slot without PSFCH (3 fewer with)
        uint8_t ownRes{0};           //!< 1: the helper selects its own relay resources
        uint8_t relayTxMax{0};       //!< own resources: max relay transmissions
        uint64_t deadline{0};        //!< own resources: SfnSf encoding of the packet deadline
    };

    void SetHelperId(uint32_t helperId);
    uint32_t GetHelperId() const;
    void SetCoopInfo(const CoopInfo& info);
    const CoopInfo& GetCoopInfo() const;

    /**
     * \brief Size of the SCI-2A on the air (stage-2 format 2A fields only),
     * used by the PHY error model; excludes the simulation side information.
     */
    uint32_t GetAirSize() const;

    /// true if helperId or relayedFromRnti is set (side information serialized)
    bool HasCoopInfo() const;

    /**
     * \brief Get the HARQ feedback enabled/disabled indicator field value
     *
     * \return The HARQ feedback enabled/disabled indicator field value
     */
    uint8_t GetHarqFeedbackIndicator() const;
    /**
     * \brief Get the cast type indicator field value
     *
     * \return The cast type indicator value
     */
    uint8_t GetCastType() const;
    /**
     * \brief Get the Channel State Information request flag
     *
     * \return The channel state information request flag
     */
    uint8_t GetCsiReq() const;
    /**
     * \brief Get the 3GPP new data indicator bit (toggled per new TB of the HARQ process)
     *
     * \return The NDI bit
     */
    uint8_t GetNdiBit() const;

    /**
     * \brief Equality operator
     *
     * \param [in] b the NrSlSciF2aHeader to compare
     * \returns \c true if \pname{b} is equal
     */
    bool operator==(const NrSlSciF2aHeader& b) const;

    /**
     * \brief Get the type ID.
     * \return the object TypeId
     */
    static TypeId GetTypeId();
    TypeId GetInstanceTypeId() const override;
    void Print(std::ostream& os) const override;
    uint32_t GetSerializedSize() const override;
    void Serialize(Buffer::Iterator start) const override;
    uint32_t Deserialize(Buffer::Iterator start) override;

  private:
    uint8_t m_harqFeedbackIndicator{Disabled}; //!< Whether HARQ feedback is enabled or disabled
    uint8_t m_castType{Broadcast}; //!< The type of communication this NrSlSciF2aHeader is used for
    uint8_t m_csiReq{0};           //!< The channel state information request flag
    uint8_t m_ndiBit{0};           //!< 3GPP NDI bit, toggled per new TB of the HARQ process
    CoopInfo m_coop;               //!< cooperative HARQ side information (not on the air)

    static std::vector<CastTypeIndicator_t>
        m_allowedCastType; //!< Vector of allowed Stage 2 SCI formats, to speed up checking
};

} // namespace ns3

#endif // NR_SL_SCI_F2A_HEADER_H
