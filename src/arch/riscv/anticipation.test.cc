#include <gtest/gtest.h>

#include "arch/riscv/isa.hh"
#include "arch/riscv/regs/misc.hh"
#include "params/RiscvISA.hh"

/*
This test file does not currently test:
  APCTRL's M/S/U privilege mask
  APS, pending/active bitmasks
  APSTATUS's APCnt/Rev fields
  APSCRATCH, the apret instruction
*/

using namespace gem5;
using namespace gem5::RiscvISA;

constexpr RegVal ApieMask = 0x1;
constexpr RegVal ApCtrlEnableMask = 0x1;
constexpr size_t NumAnticipationPoints = 8;

// manages state for each test, and each test gets it's own instance of this
// fixture
class AnticipationFixture : public ::testing::Test
{
  protected:
    RiscvISAParams params = [] {
        RiscvISAParams p;
        p.name = "anticipation_test.isa";
        p.eventq_index = 0;
        p.riscv_type = enums::RV64;
        p.enable_rvv = false;
        p.vlen = 128;
        p.elen = 64;
        p.privilege_mode_set = enums::M;
        p.wfi_resume_on_pending = false;
        p.enable_Zcd = false;
        p.enable_Smrnmi = false;
        p.enable_Zicbom_fs = false;
        p.enable_Zicboz_fs = false;
        return p;
    }();
    ISA isa{params};

    void
    setLane(RegVal ctrl, Addr trigger, Addr target)
    {
        isa.setMiscReg(MISCREG_APCTRL, ctrl);
        isa.setMiscReg(MISCREG_APTRIG, trigger);
        isa.setMiscReg(MISCREG_APTAR, target);
    }

    void
    enableGlobalAP()
    { isa.setMiscRegNoEffect(MISCREG_APSTATUS, ApieMask); }
};

// First section of tests

TEST_F(AnticipationFixture, NoRedirectWhenApieClear)
{
    // we expect APIE to be false by default when instantiating the ISA object
    // in the fixture

    setLane(ApCtrlEnableMask, 0x2000, 0x3000);

    Addr target_pc = 0;
    EXPECT_FALSE(isa.checkAnticipationRedirect(0x2000, target_pc));
    EXPECT_EQ(target_pc, 0u);
}

TEST_F(AnticipationFixture, NoRedirectWhenLaneDisabled)
{
    enableGlobalAP();
    setLane(/*ctrl=*/0, 0x2000, 0x3000);

    Addr target_pc = 0;
    EXPECT_FALSE(isa.checkAnticipationRedirect(0x2000, target_pc));
}

TEST_F(AnticipationFixture, NoRedirectWhenPcDoesNotMatchTrigger)
{
    enableGlobalAP();
    setLane(ApCtrlEnableMask, 0x2000, 0x3000);

    Addr target_pc = 0;
    EXPECT_FALSE(isa.checkAnticipationRedirect(0x2004, target_pc));
}

TEST_F(AnticipationFixture, RedirectsOnTriggerMatch)
{
    enableGlobalAP();
    setLane(ApCtrlEnableMask, 0x2000, 0x3000);

    Addr target_pc = 0;
    EXPECT_TRUE(isa.checkAnticipationRedirect(0x2000, target_pc));
    EXPECT_EQ(target_pc, 0x3000u);
}

TEST_F(AnticipationFixture, RecordsLastExecutedLaneAndEpc)
{
    enableGlobalAP();
    isa.setMiscRegNoEffect(MISCREG_APSELECT, 0);
    setLane(ApCtrlEnableMask, 0x2000, 0x3000);

    Addr target_pc = 0;
    ASSERT_TRUE(isa.checkAnticipationRedirect(0x2000, target_pc));

    EXPECT_EQ(isa.readMiscRegNoEffect(MISCREG_APLASTEX), 0u);
    EXPECT_EQ(isa.readMiscRegNoEffect(MISCREG_APEPC), 0x2000u);
}

TEST_F(AnticipationFixture, ClearsApieAfterFiring)
{
    enableGlobalAP();
    setLane(ApCtrlEnableMask, 0x2000, 0x3000);

    Addr target_pc = 0;
    ASSERT_TRUE(isa.checkAnticipationRedirect(0x2000, target_pc));

    EXPECT_EQ(isa.readMiscRegNoEffect(MISCREG_APSTATUS) & ApieMask, 0u);
}

TEST_F(AnticipationFixture, OutOfRangeApSelectDisablesRedirect)
{
    isa.setMiscRegNoEffect(MISCREG_APSTATUS, ApieMask);

    // we need to bypass the gaurd in setMiscReg()
    isa.setMiscRegNoEffect(MISCREG_APSELECT, NumAnticipationPoints);

    setLane(ApCtrlEnableMask, 0x2000, 0x3000);

    Addr target_pc = 0;
    EXPECT_FALSE(isa.checkAnticipationRedirect(0x2000, target_pc));
}

TEST_F(AnticipationFixture, RedirectFiresFromNonActiveLane)
{
    // we expect checkAnticipationRedirect to return true for an address of
    // another AP even when the address of the AP currently selected by
    // APSELECT is different.
    isa.setMiscRegNoEffect(MISCREG_APSTATUS, ApieMask);

    isa.setMiscReg(MISCREG_APSELECT, 1);
    setLane(ApCtrlEnableMask, 0x4000, 0x5000);

    isa.setMiscReg(MISCREG_APSELECT, 0);

    Addr target_pc = 0;
    EXPECT_TRUE(isa.checkAnticipationRedirect(0x4000, target_pc));
    EXPECT_EQ(target_pc, 0x5000u);
    EXPECT_EQ(isa.readMiscRegNoEffect(MISCREG_APLASTEX), 1u);
}

// Second section of tests

TEST_F(AnticipationFixture, SwitchingLanesSavesAndRestoresShadowRegisters)
{
    // Lane 0 is active by default (APSELECT resets to 0 via ISA::clear()).
    setLane(0x11, 0x1000, 0x1100);

    // Switching to lane 1 (never configured) should save off lane 0's
    // registers and expose lane 1's still-zeroed shadow registers.
    isa.setMiscReg(MISCREG_APSELECT, 1);
    EXPECT_EQ(isa.readMiscRegNoEffect(MISCREG_APCTRL), 0u);
    EXPECT_EQ(isa.readMiscRegNoEffect(MISCREG_APTRIG), 0u);
    EXPECT_EQ(isa.readMiscRegNoEffect(MISCREG_APTAR), 0u);

    setLane(0x22, 0x2000, 0x2200);

    // Switching back to lane 0 should restore what was set there earlier.
    isa.setMiscReg(MISCREG_APSELECT, 0);
    EXPECT_EQ(isa.readMiscRegNoEffect(MISCREG_APCTRL), 0x11u);
    EXPECT_EQ(isa.readMiscRegNoEffect(MISCREG_APTRIG), 0x1000u);
    EXPECT_EQ(isa.readMiscRegNoEffect(MISCREG_APTAR), 0x1100u);

    // And lane 1 should still hold what was set moments ago.
    isa.setMiscReg(MISCREG_APSELECT, 1);
    EXPECT_EQ(isa.readMiscRegNoEffect(MISCREG_APCTRL), 0x22u);
    EXPECT_EQ(isa.readMiscRegNoEffect(MISCREG_APTRIG), 0x2000u);
    EXPECT_EQ(isa.readMiscRegNoEffect(MISCREG_APTAR), 0x2200u);
}

// Throwing an error should be considered here
TEST_F(AnticipationFixture, OutOfRangeApSelectClampsToLastLane)
{
    isa.setMiscReg(MISCREG_APSELECT, 100);
    EXPECT_EQ(isa.readMiscRegNoEffect(MISCREG_APSELECT),
              NumAnticipationPoints - 1);
}

TEST_F(AnticipationFixture, EachLaneIsIndependentlyAddressable)
{
    for (size_t lane = 0; lane < NumAnticipationPoints; lane++) {
        isa.setMiscReg(MISCREG_APSELECT, lane);
        setLane(ApCtrlEnableMask, 0x1000 + lane, 0x9000 + lane);
    }

    for (size_t lane = 0; lane < NumAnticipationPoints; lane++) {
        isa.setMiscReg(MISCREG_APSELECT, lane);
        EXPECT_EQ(isa.readMiscRegNoEffect(MISCREG_APCTRL), ApCtrlEnableMask);
        EXPECT_EQ(isa.readMiscRegNoEffect(MISCREG_APTRIG), 0x1000u + lane);
        EXPECT_EQ(isa.readMiscRegNoEffect(MISCREG_APTAR), 0x9000u + lane);
    }
}
