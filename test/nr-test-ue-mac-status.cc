// Copyright (c) 2026 Yifeng He
// SPDX-License-Identifier: GPL-2.0-only
#include "nr-rlc-test-utils.h"

#include "ns3/log.h"
#include "ns3/nr-amc.h"
#include "ns3/nr-mac-header-fs-ul.h"
#include "ns3/nr-mac-header-vs.h"
#include "ns3/nr-mac-scheduler-ofdma-rr.h"
#include "ns3/nr-mac-scheduler-tdma-rr.h"
#include "ns3/nr-mac-short-bsr-ce.h"
#include "ns3/nr-phy-sap.h"
#include "ns3/nr-radio-bearer-tag.h"
#include "ns3/nr-rlc-am-header.h"
#include "ns3/nr-rlc-am.h"
#include "ns3/nr-rlc-tag.h"
#include "ns3/nr-ue-mac.h"
#include "ns3/simulator.h"
#include "ns3/test.h"

#include <algorithm>
#include <functional>
#include <vector>

namespace ns3
{
NS_LOG_COMPONENT_DEFINE("NrTestUeMacStatus");

namespace
{
/** @ingroup tests
 * @brief Ignore scheduler configuration confirmations.
 */
class StatusCschedSap : public NrMacCschedSapUser
{
  public:
    StatusCschedSap() = default;

    void CschedCellConfigCnf(
        [[maybe_unused]] const struct CschedCellConfigCnfParameters& params) override
    {
    }

    void CschedUeConfigCnf(
        [[maybe_unused]] const struct CschedUeConfigCnfParameters& params) override
    {
    }

    void CschedLcConfigCnf(
        [[maybe_unused]] const struct CschedLcConfigCnfParameters& params) override
    {
    }

    void CschedLcReleaseCnf(
        [[maybe_unused]] const struct CschedLcReleaseCnfParameters& params) override
    {
    }

    void CschedUeReleaseCnf(
        [[maybe_unused]] const struct CschedUeReleaseCnfParameters& params) override
    {
    }

    void CschedUeConfigUpdateInd(
        [[maybe_unused]] const struct CschedUeConfigUpdateIndParameters& params) override
    {
    }

    void CschedCellConfigUpdateInd(
        [[maybe_unused]] const struct CschedCellConfigUpdateIndParameters& params) override
    {
    }
};

/**
 * @ingroup tests
 * @brief SCHED SAP user that forwards the scheduling decision to a callback.
 */
class StatusSchedSap : public NrMacSchedSapUser
{
  public:
    /**
     * @brief Constructor
     * @param schedConfigIndCallback Callback invoked with each scheduling
     *        decision (SchedConfigInd).
     */
    StatusSchedSap(
        std::function<void(const struct SchedConfigIndParameters&)> schedConfigIndCallback)
        : NrMacSchedSapUser(),
          m_schedConfigIndCallback(std::move(schedConfigIndCallback))
    {
    }

    void SchedConfigInd(const NrMacSchedSapUser::SchedConfigIndParameters& params) override
    {
        if (m_schedConfigIndCallback)
        {
            m_schedConfigIndCallback(params);
        }
    }

    Ptr<const SpectrumModel> GetSpectrumModel() const override
    {
        return nullptr;
    }

    uint32_t GetNumRbPerRbg() const override
    {
        return 1;
    }

    uint8_t GetNumHarqProcess() const override
    {
        return 20;
    }

    uint16_t GetBwpId() const override
    {
        return 0;
    }

    uint16_t GetCellId() const override
    {
        return 0;
    }

    uint32_t GetSymbolsPerSlot() const override
    {
        return 14;
    }

    Time GetSlotPeriod() const override
    {
        return MilliSeconds(1);
    }

    void BuildRarList([[maybe_unused]] SlotAllocInfo& allocInfo) override
    {
    }

  private:
    std::function<void(const NrMacSchedSapUser::SchedConfigIndParameters&)>
        m_schedConfigIndCallback; //!< Decision callback
};

/** @ingroup tests
 * @brief Capture legacy RLC AM STATUS output and forward its BSR to UE MAC.
 */
class StatusCapture : public NrRlcTestCaptureMac
{
  public:
    NrMacSapProvider* forward{nullptr};                ///< Real MAC SAP.
    std::vector<BufferStatusReportParameters> reports; ///< BSRs.

    void BufferStatusReport(BufferStatusReportParameters p) override
    {
        reports.push_back(p);
        NS_LOG_INFO("RLC_BSR lcid=" << +p.lcid << " status=" << p.statusPduSize);
        if (forward)
        {
            forward->BufferStatusReport(p);
        }
    }

    void TransmitPdu(TransmitPduParameters p) override
    {
        m_pdus.push_back(p.pdu->Copy());
        NS_LOG_INFO("RLC_TX lcid=" << +p.lcid << " bytes=" << p.pdu->GetSize());
        if (forward)
        {
            forward->TransmitPdu(p);
        }
    }
};

/** Deliver legacy AMD data through the public MAC SAP.
 * @param rlc RLC entity.
 * @param lcid Receiving logical channel.
 * @param sn Sequence number.
 * @param poll Whether to request STATUS.
 */
void
DeliverStatusData(Ptr<NrRlcAm> rlc, uint8_t lcid, uint16_t sn, bool poll)
{
    auto p = Create<Packet>(20);
    NrRlcAmHeader h;
    h.SetDataPdu();
    h.SetSequenceNumber(nr::SequenceNumber10(sn));
    h.SetResegmentationFlag(NrRlcAmHeader::PDU);
    h.SetLastSegmentFlag(NrRlcAmHeader::LAST_PDU_SEGMENT);
    h.SetSegmentOffset(0);
    h.SetFramingInfo(0);
    h.SetPollingBit(poll ? NrRlcAmHeader::STATUS_REPORT_IS_REQUESTED
                         : NrRlcAmHeader::STATUS_REPORT_NOT_REQUESTED);
    h.PushExtensionBit(NrRlcAmHeader::DATA_FIELD_FOLLOWS);
    p->AddHeader(h);
    NrRlcTag tag(Simulator::Now());
    p->AddByteTag(tag, 1, h.GetSerializedSize());
    rlc->GetNrMacSapUser()->ReceivePdu(NrMacSapUser::ReceivePduParameters(p, 1, lcid));
}

/** Record the actual sub-PDUs emitted by real UE MAC. */
class StatusPhy : public NrPhySapProvider
{
  public:
    std::vector<Ptr<Packet>> packets; ///< MAC sub-PDUs.
    Ptr<NrMacSchedulerNs3> scheduler; ///< Optional ordinary scheduler.

    void SendMacPdu(const Ptr<Packet>& p, const SfnSf&, uint8_t, uint16_t) override
    {
        packets.push_back(p->Copy());
        NrRadioBearerTag tag;
        p->PeekPacketTag(tag);
        NS_LOG_INFO("PHY_TX lcid=" << +tag.GetLcid() << " bytes=" << p->GetSize());
        if (scheduler && tag.GetLcid() == NrMacHeaderFsUl::SHORT_BSR)
        {
            NrMacShortBsrCe shortBsr;
            p->PeekHeader(shortBsr);
            MacCeElement ce{};
            ce.m_rnti = tag.GetRnti();
            ce.m_macCeType = MacCeElement::BSR;
            ce.m_macCeValue.m_bufferStatus = {shortBsr.m_bufferSizeLevel_0,
                                              shortBsr.m_bufferSizeLevel_1,
                                              shortBsr.m_bufferSizeLevel_2,
                                              shortBsr.m_bufferSizeLevel_3};
            NrMacSchedSapProvider::SchedUlMacCtrlInfoReqParameters req{};
            req.m_macCeList.push_back(ce);
            scheduler->GetMacSchedSapProvider()->SchedUlMacCtrlInfoReq(req);
            NS_LOG_INFO("ORDINARY_BSR_FORWARDED level0=" << +shortBsr.m_bufferSizeLevel_0);
        }
    }

    void SendControlMessage(Ptr<NrControlMessage> msg) override
    {
        if (scheduler && msg->GetMessageType() == NrControlMessage::SR)
        {
            NrMacSchedSapProvider::SchedUlSrInfoReqParameters sr{};
            sr.m_snfSf = SfnSf(0, 2, 0, 0);
            sr.m_srList.push_back(DynamicCast<NrSRMessage>(msg)->GetRNTI());
            scheduler->GetMacSchedSapProvider()->SchedUlSrInfoReq(sr);
            NS_LOG_INFO("ORDINARY_SR_FORWARDED");
        }
    }

    void SendRachPreamble(uint8_t, uint8_t) override
    {
    }

    void SetSlotAllocInfo(const SlotAllocInfo&) override
    {
    }

    void NotifyConnectionSuccessful() override
    {
    }

    BeamId GetBeamId(uint8_t) const override
    {
        return BeamId(0, 0);
    }

    Ptr<const SpectrumModel> GetSpectrumModel() override
    {
        return nullptr;
    }

    uint16_t GetBwpId() const override
    {
        return 0;
    }

    uint16_t GetCellId() const override
    {
        return 1;
    }

    uint32_t GetSymbolsPerSlot() const override
    {
        return 14;
    }

    Time GetSlotPeriod() const override
    {
        return MilliSeconds(1);
    }

    uint32_t GetRbNum() const override
    {
        return 1;
    }

    void RegisterToGnb(uint16_t) override
    {
    }

    uint32_t GetArfcn() const override
    {
        return 0;
    }
};

/** Check real UE MAC STATUS admission and pending-state retry. */
class StatusMacCase : public TestCase
{
  public:
    /** @param tbs Injected transport block size, unused for ordinary scheduler cases.
     * @param lcs Number of pending AM logical channels.
     * @param loss Whether to generate NACK-bearing STATUS.
     * @param scheduler Empty for injected grants, TDMA or OFDMA for normal SR allocation.
     */
    StatusMacCase(uint32_t tbs, uint32_t lcs, bool loss, const std::string& scheduler = "")
        : TestCase("UE MAC STATUS: TBS " + std::to_string(tbs) + ", LCs " + std::to_string(lcs) +
                   (loss ? ", NACK " : ", ACK ") + scheduler),
          m_scheduler(scheduler),
          m_tbs(tbs),
          m_lcs(lcs),
          m_loss(loss)
    {
    }

  private:
    std::string m_scheduler; ///< Ordinary scheduler type, empty for injected grants.
    std::vector<std::shared_ptr<DciInfoElementTdma>> m_dcis; ///< Unmodified scheduled DCIs.
    uint32_t m_tbs, m_lcs;                                   ///< Injected TBS and real AM LC count.
    bool m_loss;           ///< NACK-bearing rather than ACK-only STATUS.
    uint32_t m_pending{0}; ///< Pending STATUS from public trace.

    /** Record pending STATUS bytes from the public UE MAC state trace.
     * @param bsr Current cached BSRs.
     * @param where Trace source location.
     */
    void State(uint64_t,
               SfnSf,
               uint16_t,
               uint16_t,
               uint8_t,
               NrUeMac::SrBsrMachine,
               std::unordered_map<uint8_t, NrMacSapProvider::BufferStatusReportParameters> bsr,
               int,
               std::string where)
    {
        if (where == "ProcessUlDci")
        {
            m_pending = 0;
            for (const auto& entry : bsr)
            {
                m_pending += entry.second.statusPduSize;
            }
            NS_LOG_INFO("MAC_PENDING_STATUS=" << m_pending);
        }
    }

    /** Deliver an injected grant through the PHY SAP.
     * @param mac UE MAC.
     * @param tbs Transport block size.
     */
    void Inject(Ptr<NrUeMac> mac, uint32_t tbs)
    {
        auto dci = std::make_shared<DciInfoElementTdma>(1,
                                                        DciInfoElementTdma::UL,
                                                        0,
                                                        1,
                                                        0,
                                                        1,
                                                        nullptr,
                                                        tbs,
                                                        1,
                                                        0,
                                                        DciInfoElementTdma::DATA,
                                                        0,
                                                        0);
        auto msg = Create<NrUlDciMessage>(dci);
        msg->SetKDelay(0);
        NS_LOG_INFO("INJECTED_TBS=" << tbs);
        mac->GetPhySapUser()->ReceiveControlMessage(msg);
    }

    void DoRun() override
    {
        StatusPhy phy;
        StatusCschedSap csap;
        StatusSchedSap ssap([this](const auto& p) {
            for (const auto& a : p.m_slotAllocInfo.m_varTtiAllocInfo)
            {
                if (a.m_dci->m_format == DciInfoElementTdma::UL &&
                    a.m_dci->m_type == DciInfoElementTdma::DATA && a.m_dci->m_ndi == 1)
                {
                    m_dcis.push_back(a.m_dci);
                }
            }
        });
        if (!m_scheduler.empty())
        {
            phy.scheduler =
                m_scheduler == "TDMA"
                    ? StaticCast<NrMacSchedulerNs3>(CreateObject<NrMacSchedulerTdmaRR>())
                    : StaticCast<NrMacSchedulerNs3>(CreateObject<NrMacSchedulerOfdmaRR>());
            phy.scheduler->InstallDlAmc(CreateObject<NrAmc>());
            phy.scheduler->InstallUlAmc(CreateObject<NrAmc>());
            phy.scheduler->SetMacSchedSapUser(&ssap);
            phy.scheduler->SetMacCschedSapUser(&csap);
            NrMacCschedSapProvider::CschedCellConfigReqParameters cell{};
            cell.m_dlBandwidth = 10;
            cell.m_ulBandwidth = 10;
            phy.scheduler->GetMacCschedSapProvider()->CschedCellConfigReq(cell);
            NrMacCschedSapProvider::CschedUeConfigReqParameters ue{};
            ue.m_rnti = 1;
            ue.m_beamId = BeamId(0, 0);
            phy.scheduler->GetMacCschedSapProvider()->CschedUeConfigReq(ue);
            phy.scheduler->SetDlCtrlSyms(1);
            phy.scheduler->SetUlCtrlSyms(1);
        }
        auto mac = CreateObject<NrUeMac>();
        mac->SetPhySapProvider(&phy);
        mac->SetNumHarqProcess(20);
        mac->GetUeCmacSapProvider()->SetRnti(1);
        mac->TraceConnectWithoutContext("UeMacStateMachineTrace",
                                        MakeCallback(&StatusMacCase::State, this));
        std::vector<StatusCapture> captures(m_lcs);
        std::vector<NrRlcTestSduSink> sinks(m_lcs);
        std::vector<Ptr<NrRlcAm>> rlcs;
        for (uint32_t i = 0; i < m_lcs; ++i)
        {
            auto rlc = CreateObject<NrRlcAm>();
            rlc->SetRnti(1);
            rlc->SetLcId(3 + i);
            captures[i].forward = mac->GetUeMacSapProvider();
            rlc->SetNrMacSapProvider(&captures[i]);
            rlc->SetNrRlcSapUser(sinks[i].GetSapUser());
            rlc->SetAttribute("ReorderingTimer", TimeValue(MilliSeconds(1)));
            NrUeCmacSapProvider::LogicalChannelConfig lc{};
            lc.logicalChannelGroup = 0;
            mac->GetUeCmacSapProvider()->AddLc(3 + i, lc, rlc->GetNrMacSapUser());
            if (phy.scheduler)
            {
                NrMacCschedSapProvider::CschedLcConfigReqParameters req{};
                req.m_rnti = 1;
                nr::LogicalChannelConfigListElement_s cfg{};
                cfg.m_logicalChannelIdentity = 3 + i;
                cfg.m_logicalChannelGroup = 0;
                cfg.m_direction = nr::LogicalChannelConfigListElement_s::DIR_BOTH;
                cfg.m_fiveQi = 9;
                req.m_logicalChannelConfigList.push_back(cfg);
                phy.scheduler->GetMacCschedSapProvider()->CschedLcConfigReq(req);
            }
            DeliverStatusData(rlc, 3 + i, m_loss ? 3 : 0, !m_loss);
            rlcs.push_back(rlc);
        }
        Simulator::Stop(MilliSeconds(2));
        Simulator::Run();
        for (const auto& c : captures)
        {
            NS_TEST_ASSERT_MSG_EQ(c.reports.back().statusPduSize, 4, "Use actual upstream BSR4");
        }
        mac->GetPhySapUser()->SlotIndication(SfnSf(0, 2, 0, 0));
        if (phy.scheduler)
        {
            NrMacSchedSapProvider::SchedUlTriggerReqParameters req{};
            req.m_snfSf = SfnSf(0, 2, 0, 0);
            req.m_slotType = LteNrTddSlotType::UL;
            phy.scheduler->GetMacSchedSapProvider()->SchedUlTriggerReq(req);
            NS_TEST_ASSERT_MSG_EQ(m_dcis.size(), 1, "Ordinary SR creates one grant");
            if (m_dcis.empty())
            {
                Simulator::Destroy();
                for (auto rlc : rlcs)
                {
                    rlc->Dispose();
                }
                mac->Dispose();
                phy.scheduler->Dispose();
                return;
            }
            const auto dci = m_dcis.front();
            m_tbs = dci->m_tbSize;
            NS_LOG_INFO("ORDINARY_DCI scheduler=" << m_scheduler << " tbs=" << m_tbs << " symbols="
                                                  << +dci->m_numSym << " mcs=" << +dci->m_mcs);
            auto msg = Create<NrUlDciMessage>(dci);
            msg->SetKDelay(0);
            mac->GetPhySapUser()->ReceiveControlMessage(msg);
        }
        else
        {
            Inject(mac, m_tbs);
        }
        uint32_t used = 0;
        uint32_t sent = 0;
        for (const auto& p : phy.packets)
        {
            used += p->GetSize();
        }
        for (const auto& c : captures)
        {
            sent += c.m_pdus.size();
        }
        const uint32_t statusMacBytes = 6;
        const uint32_t expectedSent = std::min(m_lcs, (m_tbs - 5) / statusMacBytes);
        NS_LOG_INFO("RESULT tbs=" << m_tbs << " lcs=" << m_lcs << " sent=" << sent
                                  << " used=" << used << " pending=" << m_pending);
        NS_TEST_ASSERT_MSG_EQ(sent, expectedSent, "Admission uses the advertised STATUS size");
        NS_TEST_ASSERT_MSG_EQ(used <= m_tbs, true, "TB respected");
        NS_TEST_ASSERT_MSG_EQ(used,
                              sent * (m_loss ? 6 : 4) + 5,
                              "Actual STATUS subheaders and SHORT_BSR bytes");
        NrMacShortBsrCe shortBsr;
        phy.packets.back()->PeekHeader(shortBsr);
        NS_TEST_ASSERT_MSG_EQ(phy.packets.back()->GetSize(), 5, "SHORT_BSR serialized size");
        NS_TEST_ASSERT_MSG_EQ(shortBsr.m_bufferSizeLevel_0 > 0,
                              sent < m_lcs,
                              "Deferred STATUS remains advertised in the SHORT_BSR");
        NS_TEST_ASSERT_MSG_EQ(m_pending, 4 * (m_lcs - sent), "Deferred STATUS remains pending");
        if (sent < m_lcs)
        {
            if (!m_loss)
            {
                for (uint32_t repeat = 0; repeat < 3; ++repeat)
                {
                    const auto start = phy.packets.size();
                    Inject(mac, m_tbs);
                    uint32_t repeatedBytes = 0;
                    for (std::size_t i = start; i < phy.packets.size(); ++i)
                    {
                        repeatedBytes += phy.packets[i]->GetSize();
                    }
                    NS_TEST_ASSERT_MSG_EQ(repeatedBytes <= m_tbs,
                                          true,
                                          "Repeated grant stays bounded");
                    NS_TEST_ASSERT_MSG_EQ(captures.front().m_pdus.size(),
                                          0,
                                          "Small ACK grant remains deferred");
                    NS_TEST_ASSERT_MSG_EQ(m_pending, 4, "Repeated grants preserve pending STATUS");
                }
            }
            const auto firstPackets = phy.packets.size();
            uint32_t retryTbs = m_loss ? std::max(12u, m_tbs) : 11;
            if (phy.scheduler)
            {
                m_dcis.clear();
                NrMacSchedSapProvider::SchedUlTriggerReqParameters req{};
                req.m_snfSf = SfnSf(0, 3, 0, 0);
                req.m_slotType = LteNrTddSlotType::UL;
                phy.scheduler->GetMacSchedSapProvider()->SchedUlTriggerReq(req);
                NS_TEST_ASSERT_MSG_EQ(m_dcis.size(), 1, "Actual BSR schedules remaining STATUS");
                if (m_dcis.empty())
                {
                    Simulator::Destroy();
                    for (auto rlc : rlcs)
                    {
                        rlc->Dispose();
                    }
                    mac->Dispose();
                    phy.scheduler->Dispose();
                    return;
                }
                const auto& dci = m_dcis.front();
                retryTbs = dci->m_tbSize;
                NS_LOG_INFO("ORDINARY_RETRY_DCI scheduler=" << m_scheduler << " tbs=" << retryTbs);
                mac->GetPhySapUser()->SlotIndication(SfnSf(0, 3, 0, 0));
                auto msg = Create<NrUlDciMessage>(dci);
                msg->SetKDelay(0);
                mac->GetPhySapUser()->ReceiveControlMessage(msg);
            }
            else
            {
                Inject(mac, retryTbs);
            }
            uint32_t retryUsed = 0;
            uint32_t finalSent = 0;
            for (std::size_t i = firstPackets; i < phy.packets.size(); ++i)
            {
                retryUsed += phy.packets[i]->GetSize();
            }
            for (const auto& c : captures)
            {
                finalSent += c.m_pdus.size();
            }
            NS_LOG_INFO("RETRY sent_total=" << finalSent << " used=" << retryUsed
                                            << " pending=" << m_pending);
            NS_TEST_ASSERT_MSG_EQ(finalSent, m_lcs, "Retry sends exactly once");
            NS_TEST_ASSERT_MSG_EQ(m_pending, 0, "Retry clears cached STATUS");
            NS_TEST_ASSERT_MSG_EQ(retryUsed <= retryTbs, true, "Retry TB respected");
        }
        Simulator::Destroy();
        for (auto rlc : rlcs)
        {
            rlc->Dispose();
        }
        mac->Dispose();
        if (phy.scheduler)
        {
            phy.scheduler->Dispose();
        }
    }
};

/** @ingroup tests
 * @brief Synthetic STATUS producer to exercise MAC header boundaries and reentrant BSR.
 *
 * Legacy AM advertises four bytes. Larger STATUS sizes here test the MAC SAP contract,
 * rather than claiming that the current AM entity produces these sizes.
 */
class BoundaryStatusProducer : public NrMacSapUser
{
  public:
    /** @param mac UE MAC SAP.
     * @param size Advertised and emitted STATUS size.
     */
    BoundaryStatusProducer(NrMacSapProvider* mac, uint32_t size)
        : m_mac(mac),
          m_size(size)
    {
    }

    void NotifyTxOpportunity(TxOpportunityParameters params) override
    {
        ++m_count;
        NrMacSapProvider::TransmitPduParameters tx{};
        tx.pdu = Create<Packet>(m_size);
        tx.rnti = params.rnti;
        tx.lcid = params.lcid;
        tx.layer = params.layer;
        tx.harqProcessId = params.harqId;
        tx.componentCarrierId = params.componentCarrierId;
        m_mac->TransmitPdu(tx);
        NrMacSapProvider::BufferStatusReportParameters bsr{};
        bsr.rnti = params.rnti;
        bsr.lcid = params.lcid;
        bsr.txQueueSize = 2;
        m_mac->BufferStatusReport(bsr);
    }

    void NotifyHarqDeliveryFailure() override
    {
    }

    void ReceivePdu(ReceivePduParameters) override
    {
    }

    uint32_t m_count{0}; ///< STATUS opportunities received.
  private:
    NrMacSapProvider* m_mac; ///< Real UE MAC SAP.
    uint32_t m_size;         ///< STATUS bytes.
};

/** @ingroup tests
 * @brief Check variable-size MAC subheaders and synchronous BSR updates.
 */
class StatusBoundaryCase : public TestCase
{
  public:
    /** @param size STATUS payload size at the MAC header boundary.
     * @param fits Whether the first grant has all overhead space.
     */
    StatusBoundaryCase(uint32_t size, bool fits)
        : TestCase("STATUS header boundary " + std::to_string(size) +
                   (fits ? " fits" : " deferred")),
          m_size(size),
          m_fits(fits)
    {
    }

  private:
    uint32_t m_size;      ///< Advertised STATUS size.
    bool m_fits;          ///< Whether all overhead fits.
    uint32_t m_status{0}; ///< Pending STATUS bytes.
    uint32_t m_data{0};   ///< New data reported during the synchronous STATUS callback.

    /** Observe the cached BSR after processing an UL DCI.
     * @param bsr Cached BSRs.
     * @param where Trace location.
     */
    void State(uint64_t,
               SfnSf,
               uint16_t,
               uint16_t,
               uint8_t,
               NrUeMac::SrBsrMachine,
               std::unordered_map<uint8_t, NrMacSapProvider::BufferStatusReportParameters> bsr,
               int,
               std::string where)
    {
        if (where == "ProcessUlDci")
        {
            m_status = bsr.at(3).statusPduSize;
            m_data = bsr.at(3).txQueueSize;
        }
    }

    void DoRun() override
    {
        StatusPhy phy;
        auto mac = CreateObject<NrUeMac>();
        mac->SetPhySapProvider(&phy);
        mac->SetNumHarqProcess(20);
        mac->GetUeCmacSapProvider()->SetRnti(1);
        BoundaryStatusProducer producer(mac->GetUeMacSapProvider(), m_size);
        NrUeCmacSapProvider::LogicalChannelConfig lc{};
        mac->GetUeCmacSapProvider()->AddLc(3, lc, &producer);
        mac->TraceConnectWithoutContext("UeMacStateMachineTrace",
                                        MakeCallback(&StatusBoundaryCase::State, this));
        NrMacSapProvider::BufferStatusReportParameters bsr{};
        bsr.rnti = 1;
        bsr.lcid = 3;
        bsr.statusPduSize = m_size;
        mac->GetUeMacSapProvider()->BufferStatusReport(bsr);
        mac->GetPhySapUser()->SlotIndication(SfnSf(0, 2, 0, 0));
        NrMacHeaderVs header;
        header.SetSize(m_size);
        const uint32_t headerBytes = m_size == 127 ? 2 : 3;
        NS_TEST_ASSERT_MSG_EQ(header.GetSerializedSize(), headerBytes, "Actual header threshold");
        const uint32_t tbs = m_size + headerBytes + 5;
        auto grant = [mac](uint32_t bytes) {
            auto dci = std::make_shared<DciInfoElementTdma>(1,
                                                            DciInfoElementTdma::UL,
                                                            0,
                                                            1,
                                                            0,
                                                            1,
                                                            nullptr,
                                                            bytes,
                                                            1,
                                                            0,
                                                            DciInfoElementTdma::DATA,
                                                            0,
                                                            0);
            auto msg = Create<NrUlDciMessage>(dci);
            msg->SetKDelay(0);
            mac->GetPhySapUser()->ReceiveControlMessage(msg);
        };
        grant(tbs - (m_fits ? 0 : 1));
        NS_TEST_ASSERT_MSG_EQ(producer.m_count,
                              m_fits ? 1 : 0,
                              "Account for header and BSR before admission");
        NS_TEST_ASSERT_MSG_EQ(m_status, m_fits ? 0 : m_size, "Preserve deferred STATUS");
        if (!m_fits)
        {
            grant(tbs);
        }
        NS_TEST_ASSERT_MSG_EQ(producer.m_count, 1, "Transmit exactly once");
        NS_TEST_ASSERT_MSG_EQ(m_status, 0, "Clear transmitted STATUS");
        NS_TEST_ASSERT_MSG_EQ(m_data, 2, "Preserve reentrant new-data BSR");
        const auto& status = phy.packets.at(phy.packets.size() - 2);
        NS_TEST_ASSERT_MSG_EQ(status->GetSize(), m_size + headerBytes, "Serialized STATUS size");
        NS_TEST_ASSERT_MSG_EQ(phy.packets.back()->GetSize(), 5, "SHORT_BSR reservation");
        Simulator::Destroy();
        mac->Dispose();
    }
};

/** @ingroup tests
 * @brief UE MAC STATUS overhead admission and pending-state regression tests.
 */
class UeMacStatusSuite : public TestSuite
{
  public:
    UeMacStatusSuite()
        : TestSuite("nr-ue-mac-status", Type::UNIT)
    {
        for (uint32_t tbs : {9u, 10u, 11u, 12u})
        {
            AddTestCase(new StatusMacCase(tbs, 1, true), Duration::QUICK);
        }
        AddTestCase(new StatusMacCase(12, 2, true), Duration::QUICK);
        AddTestCase(new StatusMacCase(9, 1, false), Duration::QUICK);
        AddTestCase(new StatusMacCase(11, 1, false), Duration::QUICK);
        AddTestCase(new StatusMacCase(12, 1, false), Duration::QUICK);
        AddTestCase(new StatusMacCase(0, 3, true, "TDMA"), Duration::QUICK);
        AddTestCase(new StatusMacCase(0, 3, true, "OFDMA"), Duration::QUICK);
        for (uint32_t size : {127u, 128u})
        {
            AddTestCase(new StatusBoundaryCase(size, false), Duration::QUICK);
            AddTestCase(new StatusBoundaryCase(size, true), Duration::QUICK);
        }
    }
};

UeMacStatusSuite g_ueMacStatusSuite;
} // namespace
} // namespace ns3
