// Copyright (c) 2026 Centre Tecnologic de Telecomunicacions de Catalunya (CTTC)
//
// SPDX-License-Identifier: GPL-2.0-only

/**
 * @ingroup test
 * @file nr-test-core-roaming.cc
 *
 * @brief Test suite (nr-core-roaming) for roaming between two core networks of the simulation.
 * A UE, subscriber of a home network, attaches through a gNB of its home network, moves to the
 * coverage of a gNB of a visited network, roaming partner of the home network, and comes back.
 * Each move causes a radio link failure, after which the UE selects the gNB of the other network
 * and attaches there. The test checks the attachments (roaming or not) in the MMEs of both
 * networks, the deletion of the session in the network the UE leaves (Cancel Location), the
 * address of the UE in each network, and that the UDP packets of the UE are echoed by the remote
 * host of the network serving it only.
 */

#include "ns3/core-module.h"
#include "ns3/internet-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/nr-module.h"
#include "ns3/udp-echo-helper.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE("NrCoreRoamingTest");

/**
 * @ingroup test
 * @brief Roaming of a UE between its home network and a visited network, and back.
 */
class NrCoreRoamingTestCase : public TestCase
{
  public:
    NrCoreRoamingTestCase()
        : TestCase("Roaming between two core networks, and back")
    {
    }

  private:
    void DoRun() override;

    /**
     * Record an attachment.
     * @param network the network of the MME
     * @param imsi the IMSI of the UE
     * @param cellId the cell ID of the gNB
     * @param roaming whether the UE is roaming
     */
    void Attach(std::string network, uint64_t imsi, uint16_t cellId, bool roaming)
    {
        m_attachments.push_back(network + "/" + std::to_string(cellId) +
                                (roaming ? "/roaming" : "/home"));
    }

    /**
     * Record the deletion of a session.
     * @param network the network of the MME
     * @param imsi the IMSI of the UE
     */
    void CancelLocation(std::string network, uint64_t imsi)
    {
        m_cancellations.push_back(network);
    }

    /**
     * Record the address of the UE.
     * @param key the key of the record
     * @param ue the UE node
     */
    void RecordAddress(std::string key, Ptr<Node> ue)
    {
        auto ipv4 = ue->GetObject<Ipv4>();
        m_addresses[key] = ipv4->GetAddress(1, 0).GetLocal();
    }

    /**
     * Count an echo, in the period of its reception.
     * @param host the remote host that echoed the packet
     * @param packet the echoed packet
     */
    void Echo(std::string host, Ptr<const Packet> packet)
    {
        const double t = Simulator::Now().GetSeconds();
        m_echoes[host + " host while " + (t < 3 ? "home" : (t < 9 ? "visited" : "home again"))]++;
    }

    std::vector<std::string> m_attachments;         ///< attachments: network/cell/roaming
    std::vector<std::string> m_cancellations;       ///< networks that deleted the session
    std::map<std::string, Ipv4Address> m_addresses; ///< addresses of the UE, by period
    std::map<std::string, uint32_t> m_echoes;       ///< echoes received, by host and period
};

void
NrCoreRoamingTestCase::DoRun()
{
    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(1);

    Config::SetDefault("ns3::NrHelper::UseIdealRrc", BooleanValue(true));
    Config::SetDefault("ns3::NrGnbPhy::TxPower", DoubleValue(40));
    Config::SetDefault("ns3::ThreeGppPropagationLossModel::ShadowingEnabled", BooleanValue(false));
    Config::SetDefault("ns3::NrUeRrc::N310", UintegerValue(1));
    Config::SetDefault("ns3::NrUeRrc::N311", UintegerValue(1));
    Config::SetDefault("ns3::NrUeRrc::T310", TimeValue(Seconds(1)));

    // the home network and the visited network, roaming partners, each with its remote host
    auto home = CreateObject<NrPointToPointEpcHelper>();
    auto visited = CreateObjectWithAttributes<NrPointToPointEpcHelper>("UeIpv4Network",
                                                                       Ipv4AddressValue("8.0.0.0"));
    home->AddRoamingPartner(visited);
    auto [homeHost, homeHostAddress] = home->SetupRemoteHost("100Gb/s", 1500, MilliSeconds(10));
    auto [visitedHost, visitedHostAddress] =
        visited->SetupRemoteHost("100Gb/s", 1500, MilliSeconds(10));
    home->GetMmeApplication()->TraceConnect("Attach",
                                            "home",
                                            MakeCallback(&NrCoreRoamingTestCase::Attach, this));
    visited->GetMmeApplication()->TraceConnect("Attach",
                                               "visited",
                                               MakeCallback(&NrCoreRoamingTestCase::Attach, this));
    home->GetMmeApplication()->TraceConnect(
        "CancelLocation",
        "home",
        MakeCallback(&NrCoreRoamingTestCase::CancelLocation, this));
    visited->GetMmeApplication()->TraceConnect(
        "CancelLocation",
        "visited",
        MakeCallback(&NrCoreRoamingTestCase::CancelLocation, this));

    NodeContainer gnbNodes(2);
    NodeContainer ueNodes(1);
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    auto positions = CreateObject<ListPositionAllocator>();
    positions->Add(Vector(0, 0, 10));
    positions->Add(Vector(15020, 0, 10));
    positions->Add(Vector(10, 0, 1.5));
    mobility.SetPositionAllocator(positions);
    mobility.Install(gnbNodes);
    mobility.Install(ueNodes);

    auto nrHelper = CreateObject<NrHelper>();
    nrHelper->SetSchedulerTypeId(TypeId::LookupByName("ns3::NrMacSchedulerTdmaRR"));
    auto bandwidthParts = nrHelper->CreateBandwidthParts({{1.93e9, 10e6, 1}}, "UMa", "LOS");
    nrHelper->SetEpcHelper(home);
    auto homeGnb = nrHelper->InstallGnbDevice(gnbNodes.Get(0), bandwidthParts.second);
    nrHelper->SetEpcHelper(visited);
    auto visitedGnb = nrHelper->InstallGnbDevice(gnbNodes.Get(1), bandwidthParts.second);
    nrHelper->SetEpcHelper(home);
    auto ueDevices = nrHelper->InstallUeDevice(ueNodes, bandwidthParts.second);
    const auto homeCell = DynamicCast<NrGnbNetDevice>(homeGnb.Get(0))->GetCellId();
    const auto visitedCell = DynamicCast<NrGnbNetDevice>(visitedGnb.Get(0))->GetCellId();

    InternetStackHelper internet;
    internet.Install(ueNodes);
    home->AssignUeIpv4Address(ueDevices);
    Ipv4StaticRoutingHelper routing;
    routing.GetStaticRouting(ueNodes.Get(0)->GetObject<Ipv4>())
        ->SetDefaultRoute(home->GetUeDefaultGatewayAddress(), 1);
    nrHelper->AttachToGnb(ueDevices.Get(0), homeGnb.Get(0));

    // the UE sends packets to both remote hosts, each one reachable from its network only
    UdpEchoServerHelper echoServer(9);
    echoServer.Install(homeHost).Start(Seconds(0.5));
    echoServer.Install(visitedHost).Start(Seconds(0.5));
    for (const auto& [host, address] :
         {std::pair{"home", homeHostAddress}, std::pair{"visited", visitedHostAddress}})
    {
        UdpEchoClientHelper echoClient(address, 9);
        echoClient.SetAttribute("Interval", TimeValue(MilliSeconds(100)));
        echoClient.SetAttribute("MaxPackets", UintegerValue(1000));
        auto client = echoClient.Install(ueNodes.Get(0));
        client.Start(Seconds(1));
        client.Get(0)->TraceConnect("Rx", host, MakeCallback(&NrCoreRoamingTestCase::Echo, this));
    }

    // to the visited network, and back home
    auto ueMobility = ueNodes.Get(0)->GetObject<MobilityModel>();
    Simulator::Schedule(Seconds(3), [ueMobility]() { ueMobility->SetPosition({15000, 0, 1.5}); });
    Simulator::Schedule(Seconds(9), [ueMobility]() { ueMobility->SetPosition({10, 0, 1.5}); });
    Simulator::Schedule(MilliSeconds(2900),
                        &NrCoreRoamingTestCase::RecordAddress,
                        this,
                        "home",
                        ueNodes.Get(0));
    Simulator::Schedule(MilliSeconds(8900),
                        &NrCoreRoamingTestCase::RecordAddress,
                        this,
                        "visited",
                        ueNodes.Get(0));
    Simulator::Schedule(MilliSeconds(14900),
                        &NrCoreRoamingTestCase::RecordAddress,
                        this,
                        "home again",
                        ueNodes.Get(0));

    Simulator::Stop(Seconds(15));
    Simulator::Run();

    const std::vector<std::string> expectedAttachments{
        "home/" + std::to_string(homeCell) + "/home",
        "visited/" + std::to_string(visitedCell) + "/roaming",
        "home/" + std::to_string(homeCell) + "/home"};
    NS_TEST_EXPECT_MSG_EQ(m_attachments.size(), expectedAttachments.size(), "Wrong attachments");
    for (size_t i = 0; i < std::min(m_attachments.size(), expectedAttachments.size()); i++)
    {
        NS_TEST_EXPECT_MSG_EQ(m_attachments[i], expectedAttachments[i], "Wrong attachment");
    }
    // the home network deletes its session when the UE roams, the visited network when it is back
    const std::vector<std::string> expectedCancellations{"home", "visited"};
    NS_TEST_EXPECT_MSG_EQ((m_cancellations == expectedCancellations),
                          true,
                          "Wrong deletions of the session");
    NS_TEST_EXPECT_MSG_EQ(m_addresses["home"], Ipv4Address("7.0.0.2"), "Wrong home address");
    NS_TEST_EXPECT_MSG_EQ(m_addresses["visited"], Ipv4Address("8.0.0.2"), "Wrong visited address");
    NS_TEST_EXPECT_MSG_EQ(m_addresses["home again"],
                          Ipv4Address("7.0.0.2"),
                          "Home address not restored");
    NS_TEST_EXPECT_MSG_GT(m_echoes["home host while home"], 10, "No traffic at home");
    NS_TEST_EXPECT_MSG_GT(m_echoes["visited host while visited"], 10, "No traffic while roaming");
    NS_TEST_EXPECT_MSG_GT(m_echoes["home host while home again"], 10, "No traffic back home");
    NS_TEST_EXPECT_MSG_EQ(m_echoes["visited host while home"] +
                              m_echoes["visited host while home again"] +
                              m_echoes["home host while visited"],
                          0,
                          "Traffic through a network that does not serve the UE");

    Simulator::Destroy();
}

/**
 * @ingroup test
 * @brief Test suite for the roaming between core networks.
 */
class NrCoreRoamingTestSuite : public TestSuite
{
  public:
    NrCoreRoamingTestSuite()
        : TestSuite("nr-core-roaming", Type::SYSTEM)
    {
        AddTestCase(new NrCoreRoamingTestCase, Duration::QUICK);
    }
};

static NrCoreRoamingTestSuite g_nrCoreRoamingTestSuite; //!< the test suite
