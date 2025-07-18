// <ROB.cpp> -*- C++ -*-

#include <algorithm>
#include "ROB.hpp"

#include "sparta/utils/LogUtils.hpp"
#include "sparta/events/StartupEvent.hpp"
#include "CoreUtils.hpp"

namespace olympia
{
    const char ROB::name[] = "rob";

    ROB::ROB(sparta::TreeNode* node, const ROBParameterSet* p) :
        sparta::Unit(node),
        stat_ipc_(&unit_stat_set_, "ipc", "Instructions retired per cycle", &unit_stat_set_,
                  "total_number_retired/cycles"),
        num_retired_(&unit_stat_set_, "total_number_retired",
                     "The total number of instructions retired by this core",
                     sparta::Counter::COUNT_NORMAL),
        num_uops_retired_(&unit_stat_set_, "total_uops_retired",
                     "The total number of uops retired by this core",
                     sparta::Counter::COUNT_NORMAL),
        num_flushes_(&unit_stat_set_, "total_number_of_flushes",
                     "The total number of flushes performed by the ROB",
                     sparta::Counter::COUNT_NORMAL),
        overall_ipc_si_(&stat_ipc_),
        period_ipc_si_(&stat_ipc_),
        retire_timeout_interval_(p->retire_timeout_interval),
        num_to_retire_(p->num_to_retire),
        num_insts_to_retire_(p->num_insts_to_retire),
        retire_heartbeat_(p->retire_heartbeat),
        reorder_buffer_("ReorderBuffer", p->retire_queue_depth, node->getClock(), &unit_stat_set_)
    {
        // Set a cycle delay on the retire, just for kicks
        ev_retire_.setDelay(1);

        // Set up the reorder buffer to support pipeline collection.
        reorder_buffer_.enableCollection(node);

        in_reorder_buffer_write_.registerConsumerHandler(
            CREATE_SPARTA_HANDLER_WITH_DATA(ROB, robAppended_, InstGroup));

        in_reorder_flush_.registerConsumerHandler(
            CREATE_SPARTA_HANDLER_WITH_DATA(ROB, handleFlush_, FlushManager::FlushingCriteria));

        // Do not allow this event to keep simulation alive
        ev_ensure_forward_progress_.setContinuing(false);

        // Notify other components when ROB stops the simulation
        rob_stopped_notif_source_.reset(new sparta::NotificationSource<bool>(
            this->getContainer(), "rob_stopped_notif_channel", "ROB terminated simulation channel",
            "rob_stopped_notif_channel"));

        // Send initial credits to anyone that cares.  Probably Dispatch.
        sparta::StartupEvent(node, CREATE_SPARTA_HANDLER(ROB, sendInitialCredits_));
    }

    /// Destroy!
    ROB::~ROB()
    {
        // Logging can be done from destructors in the correct simulator setup
        ILOG("ROB is destructing now, but you can still see this message");
    }

    void ROB::sendInitialCredits_()
    {
        out_reorder_buffer_credits_.send(reorder_buffer_.capacity());
        ev_ensure_forward_progress_.schedule(retire_timeout_interval_);
    }

    // An illustration of the use of the callback -- instead of
    // getting a reference, you can pull the data from the port
    // directly, albeit inefficient and superfluous here...
    void ROB::robAppended_(const InstGroup &)
    {
        for (auto & i : *in_reorder_buffer_write_.pullData())
        {
            reorder_buffer_.push(i);
            ILOG("retire appended: " << i);
        }

        ev_retire_.schedule(sparta::Clock::Cycle(0));
    }

    void ROB::handleFlush_(const FlushManager::FlushingCriteria & criteria)
    {
        sparta_assert(expect_flush_, "Received a flush, but didn't expect one");

        expect_flush_ = false;

        uint32_t credits_to_send = 0;

        // Clean up internals and send new credit count
        while (reorder_buffer_.size())
        {
            auto youngest_inst = reorder_buffer_.back();
            if (criteria.includedInFlush(youngest_inst))
            {
                ILOG("flushing " << youngest_inst);
                youngest_inst->setStatus(Inst::Status::FLUSHED);
                reorder_buffer_.pop_back();
                ++credits_to_send;
            }
            else
            {
                break;
            }
        }
        out_reorder_buffer_credits_.send(credits_to_send);
    }

    void ROB::retireInstructions_()
    {
        // ROB is expecting a flush (back to itself)
        if (expect_flush_)
        {
            return;
        }

        const uint32_t num_to_retire = std::min(reorder_buffer_.size(), num_to_retire_);

        ILOG("num to retire: " << num_to_retire);

        // Send instructions to rename
        InstGroupPtr retired_insts =
            sparta::allocate_sparta_shared_pointer<InstGroup>(instgroup_allocator);

        uint32_t retired_this_cycle = 0;
        for (uint32_t i = 0; i < num_to_retire; ++i)
        {
            auto ex_inst_ptr = reorder_buffer_.access(0);
            sparta_assert(nullptr != ex_inst_ptr);
            auto & ex_inst = *ex_inst_ptr;
            sparta_assert(ex_inst.isSpeculative() == false,
                          "Uh, oh!  A speculative instruction is being retired: " << ex_inst);

            if (ex_inst.getStatus() == Inst::Status::COMPLETED)
            {
                // UPDATE:
                ex_inst.setStatus(Inst::Status::RETIRED);
                if (ex_inst.isStoreInst())
                {
                    out_rob_retire_ack_.send(ex_inst_ptr);
                }

                // All instructions count as 1 uop
                ++num_uops_retired_;
                if (ex_inst_ptr->getUOpID() == 0)
                {
                    ++num_retired_;
                    ++retired_this_cycle;

                    // Use the program ID to verify that the program order has been maintained.
                    sparta_assert(ex_inst.getProgramID() == expected_program_id_,
                        "\nUnexpected program ID when retiring instruction" <<
                        "\n(suggests wrong program order)" <<
                        "\n expected: " << expected_program_id_ <<
                        "\n received: " << ex_inst.getProgramID() <<
                        "\n UID: " << ex_inst_ptr->getMavisUid() <<
                        "\n incr: " << ex_inst_ptr->getProgramIDIncrement() <<
                        "\n inst " << ex_inst);

                    // The fused op records the number of insts that
                    // were eliminated and adjusts the progID as needed
                    expected_program_id_ += ex_inst.getProgramIDIncrement();
                }

                reorder_buffer_.pop();
                ILOG("retiring " << ex_inst);

                retire_event_.collect(*ex_inst_ptr);
                last_inst_retired_ = ex_inst_ptr;

                retired_insts->emplace_back(ex_inst_ptr);

                if (SPARTA_EXPECT_FALSE((num_retired_ % retire_heartbeat_) == 0))
                {
                    std::cout << "olympia: Retired " << num_retired_.get() << " instructions in "
                              << getClock()->currentCycle()
                              << " cycles.  Period IPC: " << period_ipc_si_.getValue()
                              << " overall IPC: " << overall_ipc_si_.getValue() << std::endl;
                    period_ipc_si_.start();
                }
                // Will be true if the user provides a -i option
                if (SPARTA_EXPECT_FALSE((num_retired_ == num_insts_to_retire_)))
                {
                    rob_stopped_simulation_ = true;
                    rob_stopped_notif_source_->postNotification(true);
                    getScheduler()->stopRunning();
                    break;
                }

                // Is this a misprdicted branch requiring a refetch?
                if (ex_inst.isMispredicted())
                {
                    FlushManager::FlushingCriteria criteria(FlushManager::FlushCause::MISPREDICTION,
                                                            ex_inst_ptr);
                    out_retire_flush_.send(criteria);
                    expect_flush_ = true;
                    break;
                }

                // This is rare for the example
                if (SPARTA_EXPECT_FALSE(ex_inst.getPipe() == InstArchInfo::TargetPipe::SYS))
                {
                    retireSysInst_(ex_inst_ptr);
		    // Fix for Issue #253
                    // If a flush is caused by retiring sys inst, then retire no more!
                    if (expect_flush_ == true) {
                       break;
                   }

                }
            }
            else
            {
                break;
            }
        }

        if(false == retired_insts->empty()) {
            // sending retired instruction to rename
            out_rob_retire_ack_rename_.send(retired_insts);
        }

        if (false == reorder_buffer_.empty())
        {
            const auto & oldest_inst = reorder_buffer_.front();
            if (oldest_inst->getStatus() == Inst::Status::COMPLETED)
            {
                ILOG("oldest is marked completed: " << oldest_inst);
                ev_retire_.schedule();
            }
            else if (false == oldest_inst->isMarkedOldest())
            {
                ILOG("set oldest: " << oldest_inst);
                oldest_inst->setOldest(true, &ev_retire_);
            }
        }

        if (retired_this_cycle != 0)
        {
            out_reorder_buffer_credits_.send(retired_this_cycle);
            last_retirement_ = getClock()->currentCycle();
        }
    }

    void ROB::dumpDebugContent_(std::ostream & output) const
    {
        output << "ROB Contents" << std::endl;
        for (const auto & entry : reorder_buffer_)
        {
            output << '\t' << entry << std::endl;
        }
    }

    // Make sure the pipeline is making forward progress
    void ROB::checkForwardProgress_()
    {
        if (getClock()->currentCycle() - last_retirement_ >= retire_timeout_interval_)
        {
            sparta::SpartaException e;
            e << "Been a while since we've retired an instruction.  Is the pipe stalled "
                 "indefinitely?";
            e << " currentCycle: " << getClock()->currentCycle();
            throw e;
        }
        ev_ensure_forward_progress_.schedule(retire_timeout_interval_);
    }

    void ROB::onStartingTeardown_()
    {
        if ((reorder_buffer_.size() > 0) && (false == rob_stopped_simulation_))
        {
            std::cerr
                << "WARNING! Simulation is ending, but the ROB didn't stop it.  Lock up situation?"
                << std::endl;
            dumpDebugContent_(std::cerr);
        }
    }

    // sys gets flushed unless it is csr rd
    void ROB::retireSysInst_(InstPtr &ex_inst)
    {
        // All sys instructions use integer registers
        auto srclist = ex_inst->getRenameData().getSourceList(core_types::RegFile::RF_INTEGER);

        // if SYS instr is not a csr instruction flush
        // if SYS instr is a csr but src1 is not x0, flush
        // otherwise it is a csr read, therefore don't flush
        if (ex_inst->isCSR())
        {   // this is the case if csr instr with src != x0
            DLOG("retiring SYS wr instr with src reg " << ex_inst->getMnemonic() );

            FlushManager::FlushingCriteria criteria(FlushManager::FlushCause::POST_SYNC, ex_inst);
                    out_retire_flush_.send(criteria);
                    expect_flush_ = true;
                    ++num_flushes_;
                    ILOG("Instigating flush due to SYS instruction... " << *ex_inst);

        }

    }

}
