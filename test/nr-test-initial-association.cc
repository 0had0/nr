// Copyright (c) 2026 Centre Tecnologic de Telecomunicacions de Catalunya (CTTC)
//
// SPDX-License-Identifier: GPL-2.0-only

/**
 * @ingroup test
 * @file nr-test-initial-association.cc
 *
 * @brief Test suite `nr-initial-association`: checks the per-gNB RSRP and best beam computed by
 * NrInitialAssociation.
 */

#include "ns3/antenna-module.h"
#include "ns3/core-module.h"
#include "ns3/mobility-module.h"
#include "ns3/network-module.h"
#include "ns3/nr-module.h"

using namespace ns3;

/**
 * @ingroup nr-test
 *
 * @brief Verifies that each gNB is credited with its own best beam, whatever the scan order.
 *
 * Two co-located sectors, one with its boresight towards the UE and one facing away, share the
 * same path loss, so only the antenna orientation separates them.
 */
class NrInitialAssociationOwnBeamTestCase : public TestCase
{
  public:
    /**
     * Constructor.
     *
     * @param facingScannedFirst Whether the sector facing the UE is scanned first.
     */
    NrInitialAssociationOwnBeamTestCase(bool facingScannedFirst);

  private:
    void DoRun() override;

    bool m_facingScannedFirst; ///< Whether the sector facing the UE is scanned first
};

NrInitialAssociationOwnBeamTestCase::NrInitialAssociationOwnBeamTestCase(bool facingScannedFirst)
    : TestCase(std::string("Sector facing the UE wins when scanned ") +
               (facingScannedFirst ? "first" : "last")),
      m_facingScannedFirst(facingScannedFirst)
{
}

void
NrInitialAssociationOwnBeamTestCase::DoRun()
{
    Config::Reset();
    RngSeedManager::SetSeed(1);
    RngSeedManager::SetRun(1);

    NodeContainer gnbNodes;
    gnbNodes.Create(2);
    NodeContainer ueNodes;
    ueNodes.Create(1);

    auto positions = CreateObject<ListPositionAllocator>();
    positions->Add(Vector(0, 0, 25));
    positions->Add(Vector(0, 0, 25));
    positions->Add(Vector(100, 0, 1.5));
    MobilityHelper mobility;
    mobility.SetMobilityModel("ns3::ConstantPositionMobilityModel");
    mobility.SetPositionAllocator(positions);
    mobility.Install(gnbNodes);
    mobility.Install(ueNodes);

    auto epcHelper = CreateObject<NrPointToPointEpcHelper>();
    auto nrHelper = CreateObject<NrHelper>();
    nrHelper->SetEpcHelper(epcHelper);
    nrHelper->SetGnbAntennaAttribute("NumRows", UintegerValue(4));
    nrHelper->SetGnbAntennaAttribute("NumColumns", UintegerValue(4));
    nrHelper->SetGnbAntennaAttribute("AntennaElement",
                                     PointerValue(CreateObject<ThreeGppAntennaModel>()));
    nrHelper->SetUeAntennaAttribute("NumRows", UintegerValue(1));
    nrHelper->SetUeAntennaAttribute("NumColumns", UintegerValue(1));

    auto channelHelper = CreateObject<NrChannelHelper>();
    channelHelper->ConfigureFactories("UMa", "LOS", "ThreeGpp");
    channelHelper->SetPathlossAttribute("ShadowingEnabled", BooleanValue(false));

    CcBwpCreator ccBwpCreator;
    CcBwpCreator::SimpleOperationBandConf bandConf(3.5e9, 20e6, 1);
    auto band = ccBwpCreator.CreateOperationBandContiguousCc(bandConf);
    channelHelper->AssignChannelsToBands({band});
    auto allBwps = CcBwpCreator::GetAllBwps({band});

    auto gnbDevices = nrHelper->InstallGnbDevice(gnbNodes, allBwps);
    auto ueDevices = nrHelper->InstallUeDevice(ueNodes, allBwps);
    nrHelper->AssignStreams({.gnbDevs = gnbDevices});
    nrHelper->AssignStreams({.ueDevs = ueDevices});

    const uint32_t facingIdx = m_facingScannedFirst ? 0 : 1;
    const uint32_t awayIdx = 1 - facingIdx;
    const auto setBearing = [&](uint32_t idx, double bearing) {
        DynamicCast<NrGnbNetDevice>(gnbDevices.Get(idx))
            ->GetPhy(0)
            ->GetSpectrumPhy()
            ->GetAntenna()
            ->SetAttribute("BearingAngle", DoubleValue(bearing));
    };
    setBearing(facingIdx, 0.0);
    setBearing(awayIdx, M_PI);

    // Neither grid holds the 90 degree default of NrAnglePair, so a best beam left unset is
    // detectable.
    const std::vector<double> beamAngles{60, 120};

    Simulator::Schedule(MilliSeconds(1), [&]() {
        auto association = CreateObject<NrInitialAssociation>();
        association->AssignStreams(1);
        association->SetUeDevice(ueDevices.Get(0));
        association->SetGnbDevices(gnbDevices);
        association->SetRowBeamAngles(beamAngles);
        association->SetColBeamAngles(beamAngles);
        const auto [associatedGnb, associatedRsrp] = association->FindAssociatedGnb();

        NS_TEST_EXPECT_MSG_EQ(associatedGnb,
                              gnbDevices.Get(facingIdx),
                              "The UE should associate with the sector facing it");
        NS_TEST_EXPECT_MSG_GT(association->GetMaxRsrp(facingIdx) - association->GetMaxRsrp(awayIdx),
                              10.0,
                              "The sector facing away should lose its antenna gain");
        for (uint32_t idx = 0; idx < gnbDevices.GetN(); idx++)
        {
            NS_TEST_EXPECT_MSG_NE(association->GetBestBfv(idx).rowAng,
                                  90.0,
                                  "Every gNB should report its own best beam");
        }
    });
    Simulator::Stop(MilliSeconds(2));
    Simulator::Run();
    Simulator::Destroy();
}

/**
 * @ingroup nr-test
 *
 * @brief Test suite for NrInitialAssociation.
 */
class NrInitialAssociationTestSuite : public TestSuite
{
  public:
    NrInitialAssociationTestSuite()
        : TestSuite("nr-initial-association", Type::UNIT)
    {
        AddTestCase(new NrInitialAssociationOwnBeamTestCase(true), Duration::QUICK);
        AddTestCase(new NrInitialAssociationOwnBeamTestCase(false), Duration::QUICK);
    }
};

/// Static instance of the test suite
static NrInitialAssociationTestSuite g_nrInitialAssociationTestSuite;
