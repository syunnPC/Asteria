// SPDX-License-Identifier: Apache-2.0
#include "User/Component/BlockRequests.hpp"

#include "Support/TestContext.hpp"

using Asteria::TestSupport::TestContext;

namespace Asteria::Test
{
namespace
{
using Foundation::Size;
using Foundation::UInt32;
using Foundation::UInt64;
using User::Component::BlockAdmitOutcome;
using User::Component::BlockAdmitResult;
using User::Component::BlockCancelOutcome;
using User::Component::BlockCompleteOutcome;
using User::Component::BlockMessage;
using User::Component::BlockRequest;
using User::Component::BlockRequestState;

using Table = User::Component::BlockRequestTable<4U>;

constexpr UInt64 Alpha{0x1001U};
constexpr UInt64 Beta{0x2002U};
constexpr UInt64 Gamma{0x3003U};

[[nodiscard]] BlockMessage Request(const UInt64 transaction, const bool write = false, const UInt64 lba = 0U) noexcept
{
    BlockMessage message{};
    message.Method = write ? User::Component::BlockWriteMethod : User::Component::BlockReadMethod;
    message.Transaction = transaction;
    message.Words[1U] = lba;
    message.Words[2U] = 8U;
    message.Words[4U] = 19U;
    message.Reply = User::System::CapabilityHandle{transaction, ~transaction};
    message.Data = User::System::CapabilityHandle{transaction + 1U, transaction + 2U};
    return message;
}

[[nodiscard]] bool Admits(Table& table, const UInt64 sender, const UInt64 transaction) noexcept
{
    return table.Admit(sender, Request(transaction)).Outcome == BlockAdmitOutcome::Queued;
}

void TestAdmission(TestContext& runner) noexcept
{
    Table table{};
    const bool first = Admits(table, Alpha, 1U);
    const bool second = Admits(table, Alpha, 2U);
    const bool third = Admits(table, Alpha, 3U);
    const bool other = Admits(table, Beta, 1U);
    ASTERIA_EXPECT(runner, "the credit refuses a sender's third request; another sender is still admitted",
                   table.IsIdle() == false, first, second, !third, other, table.CountOutstanding(Alpha) == 2U,
                   table.CountOutstanding(Beta) == 1U, table.IsConsistent());

    Table duplicates{};
    const bool kept = Admits(duplicates, Alpha, 5U);
    const bool repeated = Admits(duplicates, Alpha, 5U);
    const bool sameOther = Admits(duplicates, Beta, 5U);
    ASTERIA_EXPECT(runner, "a duplicate transaction of one sender is refused, another sender's is not", kept, !repeated,
                   sameOther, duplicates.CountOutstanding(Alpha) == 1U);

    Table full{};
    bool admitted[User::Component::BlockRequestCapacity]{};
    for (Size index{0U}; index < User::Component::BlockRequestCapacity; ++index)
    {
        admitted[index] = Admits(full, 100U + (index >> 1U), index);
    }
    const BlockAdmitResult refused = full.Admit(999U, Request(1U));
    bool all{true};
    for (const bool one : admitted)
    {
        all = all && one;
    }
    ASTERIA_EXPECT(runner, "a full table refuses a new sender and keeps nothing", all,
                   refused.Outcome == BlockAdmitOutcome::Refused, full.CountOutstanding(999U) == 0U,
                   full.CountState(BlockRequestState::Queued) == User::Component::BlockRequestCapacity,
                   full.IsConsistent());

    BlockMessage cancel = Request(1U);
    cancel.Method = User::Component::BlockCancelMethod;
    BlockMessage empty = Request(2U);
    empty.Words[2U] = 0U;
    BlockMessage flagged = Request(3U);
    flagged.Words[3U] = User::Component::BlockCrashAfterIssue << 1U;
    Table malformed{};
    ASTERIA_EXPECT(runner, "a frame that is no Read or Write, has no sectors or an unknown flag is malformed",
                   malformed.Admit(Alpha, cancel).Outcome == BlockAdmitOutcome::Malformed,
                   malformed.Admit(Alpha, empty).Outcome == BlockAdmitOutcome::Malformed,
                   malformed.Admit(Alpha, flagged).Outcome == BlockAdmitOutcome::Malformed, malformed.IsIdle());
}

// The driver clears its static table as an instance starts; a request's
// flags reach the driver with its copy at issue.
void TestClearAndFlags(TestContext& runner) noexcept
{
    Table table{};
    BlockMessage crashing = Request(1U);
    crashing.Words[3U] = User::Component::BlockCrashAfterIssue;
    const bool queued = table.Admit(Alpha, crashing).Outcome == BlockAdmitOutcome::Queued;
    const bool plain = Admits(table, Beta, 2U);
    BlockRequest first{};
    BlockRequest second{};
    const bool i1 = table.NextToIssue(first);
    const bool i2 = table.NextToIssue(second);
    ASTERIA_EXPECT(runner, "a request's flags are kept with it", queued, plain, i1, i2,
                   first.Flags == User::Component::BlockCrashAfterIssue, second.Flags == 0U,
                   first.MediumGeneration == 19U, second.MediumGeneration == 19U);
    const bool third = Admits(table, Gamma, 3U);
    table.Clear();
    BlockRequest sink{};
    const bool none = !table.NextToIssue(sink);
    const bool again = Admits(table, Alpha, 1U);
    const bool another = Admits(table, Alpha, 4U);
    ASTERIA_EXPECT(runner, "a cleared table holds nothing, issued or queued, and gives every sender its credit back",
                   third, none, again, another, table.CountOutstanding(Alpha) == 2U,
                   table.Complete(first.Slot, sink) == BlockCompleteOutcome::NothingIssued,
                   table.CountState(BlockRequestState::Queued) == 2U, table.IsConsistent());
}

void TestIssue(TestContext& runner) noexcept
{
    Table table{};
    const bool a = Admits(table, Alpha, 1U);
    const bool b = Admits(table, Beta, 2U);
    const bool c = Admits(table, Alpha, 3U);
    const bool d = Admits(table, Gamma, 4U);
    const bool e = Admits(table, Beta, 5U);
    BlockRequest one{};
    BlockRequest two{};
    BlockRequest three{};
    BlockRequest four{};
    BlockRequest five{};
    const bool i1 = table.NextToIssue(one);
    const bool i2 = table.NextToIssue(two);
    const bool i3 = table.NextToIssue(three);
    const bool i4 = table.NextToIssue(four);
    const bool i5 = table.NextToIssue(five);
    ASTERIA_EXPECT(runner, "queued requests issue oldest first across senders on the lowest free slots", a, b, c, d, e,
                   i1, i2, i3, i4, !i5, one.Transaction == 1U, two.Transaction == 2U, three.Transaction == 3U,
                   four.Transaction == 4U, one.Slot == 0U, two.Slot == 1U, three.Slot == 2U, four.Slot == 3U,
                   one.State == BlockRequestState::Issued, one.Sender == Alpha, one.Reply.Selector == 1U,
                   one.Data.Authenticator == 3U, one.Sectors == 8U, table.CountState(BlockRequestState::Queued) == 1U,
                   table.IsConsistent());

    BlockRequest done{};
    const BlockCompleteOutcome middle = table.Complete(1U, done);
    BlockRequest next{};
    const bool issued = table.NextToIssue(next);
    ASTERIA_EXPECT(runner, "a completion hands back the copy, frees credit and slot, and the freed slot is reused",
                   middle == BlockCompleteOutcome::Completed, done.Transaction == 2U, done.Slot == 1U, issued,
                   next.Transaction == 5U, next.Slot == 1U, table.CountOutstanding(Beta) == 1U,
                   table.CountState(BlockRequestState::Queued) == 0U, table.IsConsistent());

    Table credit{};
    const bool x = Admits(credit, Alpha, 1U);
    const bool y = Admits(credit, Alpha, 2U);
    BlockRequest slot{};
    const bool z = credit.NextToIssue(slot);
    const bool refused = !Admits(credit, Alpha, 3U);
    const BlockCompleteOutcome freed = credit.Complete(slot.Slot, slot);
    ASTERIA_EXPECT(runner, "completion frees the sender's credit", x, y, z, refused,
                   freed == BlockCompleteOutcome::Completed, Admits(credit, Alpha, 3U),
                   credit.CountOutstanding(Alpha) == 2U);
}

void TestCompleteEmpty(TestContext& runner) noexcept
{
    Table table{};
    BlockRequest sink{};
    const bool queued = Admits(table, Alpha, 1U);
    ASTERIA_EXPECT(runner, "completing a slot with nothing issued, or out of range, is reported and changes nothing",
                   table.Complete(0U, sink) == BlockCompleteOutcome::NothingIssued,
                   table.Complete(3U, sink) == BlockCompleteOutcome::NothingIssued,
                   table.Complete(4U, sink) == BlockCompleteOutcome::NothingIssued,
                   table.Complete(~Size{0U}, sink) == BlockCompleteOutcome::NothingIssued, queued,
                   table.CountState(BlockRequestState::Queued) == 1U, table.IsConsistent());
    BlockRequest issued{};
    const bool took = table.NextToIssue(issued);
    const BlockCompleteOutcome first = table.Complete(issued.Slot, sink);
    ASTERIA_EXPECT(runner, "a second completion of the same slot is reported", took,
                   first == BlockCompleteOutcome::Completed,
                   table.Complete(issued.Slot, sink) == BlockCompleteOutcome::NothingIssued, table.IsIdle());
}

void TestCancel(TestContext& runner) noexcept
{
    Table table{};
    const bool a = Admits(table, Alpha, 1U);
    const bool b = Admits(table, Alpha, 2U);
    const bool c = Admits(table, Beta, 3U);
    BlockRequest copy{};
    const BlockCancelOutcome outcome = table.Cancel(Alpha, 2U, copy);
    ASTERIA_EXPECT(runner, "cancelling a queued request returns its copy and frees the credit", a, b, c,
                   outcome == BlockCancelOutcome::Cancelled, copy.Transaction == 2U, copy.Sender == Alpha,
                   copy.Reply.Selector == 2U, copy.Data.Selector == 3U, table.CountOutstanding(Alpha) == 1U,
                   table.IsConsistent());
    const bool again = table.Cancel(Alpha, 2U, copy) == BlockCancelOutcome::TooLate;
    BlockRequest first{};
    BlockRequest second{};
    const bool i1 = table.NextToIssue(first);
    const bool i2 = table.NextToIssue(second);
    const bool i3 = table.NextToIssue(copy);
    ASTERIA_EXPECT(runner, "a cancelled request is never issued and cancelling it twice is too late", again, i1, i2,
                   !i3, first.Transaction == 1U, second.Transaction == 3U, Admits(table, Alpha, 2U));

    BlockRequest sink{};
    // Alpha 1 and Beta 3 are issued; Alpha 2 is queued again.
    const BlockCancelOutcome issued = table.Cancel(Alpha, 1U, sink);
    const BlockCancelOutcome unknown = table.Cancel(Alpha, 77U, sink);
    const BlockCancelOutcome foreign = table.Cancel(Gamma, 2U, sink);
    const BlockCancelOutcome wrongSender = table.Cancel(Beta, 2U, sink);
    ASTERIA_EXPECT(runner, "cancelling an issued, unknown or another sender's request is too late and changes nothing",
                   issued == BlockCancelOutcome::TooLate, unknown == BlockCancelOutcome::TooLate,
                   foreign == BlockCancelOutcome::TooLate, wrongSender == BlockCancelOutcome::TooLate,
                   table.CountState(BlockRequestState::Issued) == 2U, table.CountState(BlockRequestState::Queued) == 1U,
                   table.CountOutstanding(Alpha) == 2U, table.CountOutstanding(Beta) == 1U, table.IsConsistent());
    const BlockCompleteOutcome completed = table.Complete(first.Slot, sink);
    ASTERIA_EXPECT(runner, "a request whose cancel was too late still completes normally",
                   completed == BlockCompleteOutcome::Completed, sink.Transaction == 1U,
                   table.CountOutstanding(Alpha) == 1U);
}

void TestWrap(TestContext& runner) noexcept
{
    // Arrival order holds across the counter's wrap.
    Table table{0xFFFF'FFFEU};
    BlockRequest first{};
    BlockRequest second{};
    BlockRequest third{};
    const bool a = Admits(table, Alpha, 1U);
    const bool b = Admits(table, Beta, 2U);
    const bool c = Admits(table, Gamma, 3U);
    const bool i1 = table.NextToIssue(first);
    const bool i2 = table.NextToIssue(second);
    const bool i3 = table.NextToIssue(third);
    ASTERIA_EXPECT(runner, "arrival order holds across the counter's wrap", a, b, c, i1, i2, i3,
                   first.Transaction == 1U, second.Transaction == 2U, third.Transaction == 3U, table.IsConsistent());
}

// A deterministic pseudo-random sequence of every operation, checking after
// each step that the table's invariants hold and that a model of the
// outstanding requests agrees with it.
void TestRandomised(TestContext& runner) noexcept
{
    constexpr UInt64 Senders[3U]{Alpha, Beta, Gamma};
    Table table{};
    UInt64 state{0x9E37'79B9'7F4A'7C15ULL};
    UInt64 transaction{1U};
    UInt64 serial{0U};
    UInt64 issuedOrder{0U};
    UInt64 lastQueuedIssued{0U};
    bool consistent{true};
    bool creditKept{true};
    bool fifo{true};
    bool slotsKept{true};
    bool slotHeld[4U]{};
    UInt64 slotTransaction[4U]{};
    Size operations[4U]{};
    for (Size step{0U}; step < 20000U; ++step)
    {
        state = (state * 6364136223846793005ULL) + 1442695040888963407ULL;
        const UInt64 pick = state >> 33U;
        const UInt64 sender = Senders[pick % 3U];
        BlockRequest request{};
        switch ((pick >> 4U) % 8U)
        {
        case 0U:
        case 1U:
        case 2U:
            ++operations[0U];
            (void)table.Admit(sender, Request(transaction, (pick & 1U) != 0U, ++serial));
            ++transaction;
            break;
        case 3U:
            // Often a duplicate of a recent transaction.
            (void)table.Admit(sender, Request(transaction - 1U - (pick % 3U), false, ++serial));
            break;
        case 4U:
        case 5U:
            if (table.NextToIssue(request))
            {
                ++operations[1U];
                // FIFO: Lba is the admission serial, so issues must rise (a
                // cancelled request leaves a gap, never a reversal).
                fifo = fifo && (request.Lba > lastQueuedIssued);
                lastQueuedIssued = request.Lba;
                slotsKept = slotsKept && (request.Slot < 4U) && !slotHeld[request.Slot];
                slotHeld[request.Slot] = true;
                slotTransaction[request.Slot] = request.Transaction;
                ++issuedOrder;
            }
            break;
        case 6U:
            if (table.Complete(pick % 5U, request) == BlockCompleteOutcome::Completed)
            {
                ++operations[2U];
                slotsKept = slotsKept && (request.Slot < 4U) && slotHeld[request.Slot] &&
                            (slotTransaction[request.Slot] == request.Transaction);
                slotHeld[request.Slot] = false;
            }
            else
            {
                slotsKept = slotsKept && ((pick % 5U) >= 4U || !slotHeld[pick % 5U]);
            }
            break;
        default:
            if (table.Cancel(sender, transaction - 1U - (pick % 6U), request) == BlockCancelOutcome::Cancelled)
            {
                ++operations[3U];
            }
            break;
        }
        consistent = consistent && table.IsConsistent();
        for (const UInt64 one : Senders)
        {
            creditKept = creditKept && (table.CountOutstanding(one) <= User::Component::BlockMaximumOutstanding);
        }
        Size held{0U};
        for (const bool one : slotHeld)
        {
            held += one ? 1U : 0U;
        }
        creditKept = creditKept && (held == table.CountState(BlockRequestState::Issued));
    }
    ASTERIA_EXPECT(runner, "a long deterministic sequence keeps the invariants after every step", consistent,
                   creditKept, fifo, slotsKept, operations[0U] > 1000U, operations[1U] > 1000U, operations[2U] > 1000U,
                   operations[3U] > 100U, issuedOrder == operations[1U]);
}
} // namespace
} // namespace Asteria::Test

int main()
{
    TestContext runner{"BlockRequestsTests"};
    Asteria::Test::TestAdmission(runner);
    Asteria::Test::TestClearAndFlags(runner);
    Asteria::Test::TestIssue(runner);
    Asteria::Test::TestCompleteEmpty(runner);
    Asteria::Test::TestCancel(runner);
    Asteria::Test::TestWrap(runner);
    Asteria::Test::TestRandomised(runner);
    return runner.Finish();
}
