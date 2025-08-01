
#include "dispatch/Dispatch.hpp"
#include "decode/MavisUnit.hpp"
#include "CoreUtils.hpp"
#include "rename/Rename.hpp"
#include "execute/ExecutePipe.hpp"
#include "lsu/LSU.hpp"
#include "sim/OlympiaSim.hpp"
#include "OlympiaAllocators.hpp"

#include "test/core/common/SourceUnit.hpp"
#include "test/core/common/SinkUnit.hpp"
#include "test/core/rename/ROBSinkUnit.hpp"

#include "sparta/app/CommandLineSimulator.hpp"
#include "sparta/utils/SpartaTester.hpp"
#include "sparta/resources/Buffer.hpp"
#include "sparta/sparta.hpp"
#include "sparta/simulation/ClockManager.hpp"
#include "sparta/kernel/Scheduler.hpp"
#include "sparta/utils/SpartaTester.hpp"
#include "sparta/statistics/StatisticSet.hpp"
#include "sparta/report/Report.hpp"
#include "sparta/events/UniqueEvent.hpp"
#include "sparta/app/Simulation.hpp"
#include "sparta/utils/SpartaSharedPointer.hpp"

#include <memory>
#include <vector>
#include <cinttypes>
#include <sstream>
#include <initializer_list>

TEST_INIT

class olympia::LSUTester
{
  public:
    void test_inst_issue(olympia::LSU &lsu, int count){
        EXPECT_EQUAL(lsu.lsu_insts_issued_, count);
    }

    void test_replay_issue_abort(olympia::LSU &lsu, int count) {
        EXPECT_EQUAL(lsu.replay_buffer_.size(), count);
    }

    void test_pipeline_stages(olympia::LSU &lsu) {
        EXPECT_EQUAL(lsu.address_calculation_stage_, 0);
        EXPECT_EQUAL(lsu.mmu_lookup_stage_, 1);
        EXPECT_EQUAL(lsu.cache_lookup_stage_, 3);
        EXPECT_EQUAL(lsu.cache_read_stage_, 4);
        EXPECT_EQUAL(lsu.complete_stage_, 6);
    }

};

const char USAGE[] =
    "Usage:\n"
    "    \n"
    "\n";

sparta::app::DefaultValues DEFAULTS;

// The main tester of Rename.  The test is encapsulated in the
// parameter test_type of the Source unit.
void runTest(int argc, char **argv)
{
    DEFAULTS.auto_summary_default = "off";
    std::vector<std::string> datafiles;
    std::string input_file;

    sparta::app::CommandLineSimulator cls(USAGE, DEFAULTS);
    auto & app_opts = cls.getApplicationOptions();
    app_opts.add_options()
        ("output_file",
         sparta::app::named_value<std::vector<std::string>>("output_file", &datafiles),
         "Specifies the output file")
        ("input-file",
         sparta::app::named_value<std::string>("INPUT_FILE", &input_file)->default_value(""),
         "Provide a JSON instruction stream",
         "Provide a JSON file with instructions to run through Execute");

    po::positional_options_description& pos_opts = cls.getPositionalOptions();
    pos_opts.add("output_file", -1); // example, look for the <data file> at the end

    int err_code = 0;
    if(!cls.parse(argc, argv, err_code)){
        sparta_assert(false, "Command line parsing failed"); // Any errors already printed to cerr
    }

    sparta_assert(false == datafiles.empty(), "Need an output file as the last argument of the test");

    sparta::Scheduler scheduler;
    uint64_t ilimit = 0;
    uint32_t num_cores = 1;
    bool show_factories = false;
    OlympiaSim sim("simple",
                   scheduler,
                   num_cores, // cores
                   input_file,
                   ilimit,
                   show_factories);


    cls.populateSimulation(&sim);
    sparta::RootTreeNode* root_node = sim.getRoot();

    olympia::LSU *my_lsu = root_node->getChild("cpu.core0.lsu")->getResourceAs<olympia::LSU*>();
    olympia::LSUTester lsupipe_tester;
    lsupipe_tester.test_pipeline_stages(*my_lsu);

    if(my_lsu->allowDataForwardingEX()) {
        // Data forwarding enabled case
        std::cout << "allow data forwarding " <<  "\n";;
        // First store
        cls.runSimulator(&sim, 7);

        // First load - should get data from store forwarding
        auto start_cycle = my_lsu->getClock()->currentCycle();
        cls.runSimulator(&sim, 3);
        EXPECT_EQUAL(my_lsu->getClock()->currentCycle() - start_cycle, 3); // Fast path

        // Second load - no matching store, goes to cache
        start_cycle = my_lsu->getClock()->currentCycle();
        cls.runSimulator(&sim, 7);
        EXPECT_EQUAL(my_lsu->getClock()->currentCycle() - start_cycle, 7); // Cache access path

        // Second store and load
        cls.runSimulator(&sim, 47);
        lsupipe_tester.test_replay_issue_abort(*my_lsu, 0);
    }
    else {
        // Data forwarding disabled case

        // First store
        cls.runSimulator(&sim, 7);

        // First load - must go to cache
        auto start_cycle = my_lsu->getClock()->currentCycle();
        cls.runSimulator(&sim, 7); // Takes longer, must access cache
        EXPECT_EQUAL(my_lsu->getClock()->currentCycle() - start_cycle, 7);

        // Second load - also must go to cache
        start_cycle = my_lsu->getClock()->currentCycle();
        cls.runSimulator(&sim, 7);
        EXPECT_EQUAL(my_lsu->getClock()->currentCycle() - start_cycle, 7);

        // Second store and load
        cls.runSimulator(&sim, 47);
        lsupipe_tester.test_replay_issue_abort(*my_lsu, 2);
    }

   // Final state
   cls.runSimulator(&sim);
}

int main(int argc, char **argv)
{
    runTest(argc, argv);

    REPORT_ERROR;
    return (int)ERROR_CODE;
}
