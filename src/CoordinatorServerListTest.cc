/* Copyright (c) 2011-2012 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any purpose
 * with or without fee is hereby granted, provided that the above copyright
 * notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR ANY
 * SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF
 * CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN
 * CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <queue>

#include "TestUtil.h"
#include "CoordinatorServerList.h"
#include "MockTransport.h"
#include "ServerTracker.h"
#include "ShortMacros.h"
#include "TransportManager.h"

namespace RAMCloud {

namespace {
struct MockServerTracker : public ServerTrackerInterface {
    MockServerTracker() : changes() {}
    void
    enqueueChange(const ServerDetails& server, ServerChangeEvent event)
    {
        changes.push({server, event});
    }
    void fireCallback() { TEST_LOG("called"); }
    std::queue<ServerTracker<int>::ServerChange> changes;
};
}

class CoordinatorServerListTest : public ::testing::Test {
  public:
    CoordinatorServerList sl;
    MockServerTracker tr;

    CoordinatorServerListTest()
        : sl()
        , tr()
    {
    }

    DISALLOW_COPY_AND_ASSIGN(CoordinatorServerListTest);
};

/*
 * Return true if a CoordinatorServerList::Entry is indentical to the
 * given serialized protobuf entry.
 */
static bool
protoBufMatchesEntry(const ProtoBuf::ServerList_Entry& protoBufEntry,
                     const CoordinatorServerList::Entry& serverListEntry,
                     ServerStatus status)
{
    if (serverListEntry.services.serialize() !=
        protoBufEntry.services())
        return false;
    if (*serverListEntry.serverId != protoBufEntry.server_id())
        return false;
    if (serverListEntry.serviceLocator != protoBufEntry.service_locator())
        return false;
    if (serverListEntry.expectedReadMBytesPerSec !=
        protoBufEntry.expected_read_mbytes_per_sec())
        return false;
    if (status != ServerStatus(protoBufEntry.status()))
        return false;

    return true;
}

TEST_F(CoordinatorServerListTest, constructor) {
    EXPECT_EQ(0U, sl.numberOfMasters);
    EXPECT_EQ(0U, sl.numberOfBackups);
    EXPECT_EQ(0U, sl.versionNumber);
}

TEST_F(CoordinatorServerListTest, add) {
    EXPECT_EQ(0U, sl.serverList.size());
    EXPECT_EQ(0U, sl.numberOfMasters);
    EXPECT_EQ(0U, sl.numberOfBackups);

    {
        ProtoBuf::ServerList update1;
        EXPECT_EQ(ServerId(1, 0), sl.add("hi", {MASTER_SERVICE},
                                         100, update1));
        EXPECT_TRUE(sl.serverList[1].entry);
        EXPECT_FALSE(sl.serverList[0].entry);
        EXPECT_EQ(1U, sl.numberOfMasters);
        EXPECT_EQ(0U, sl.numberOfBackups);
        EXPECT_EQ(ServerId(1, 0), sl.serverList[1].entry->serverId);
        EXPECT_EQ("hi", sl.serverList[1].entry->serviceLocator);
        EXPECT_TRUE(sl.serverList[1].entry->isMaster());
        EXPECT_FALSE(sl.serverList[1].entry->isBackup());
        EXPECT_EQ(0u, sl.serverList[1].entry->expectedReadMBytesPerSec);
        EXPECT_EQ(1U, sl.serverList[1].nextGenerationNumber);
        EXPECT_EQ(0U, sl.versionNumber);
        sl.incrementVersion(update1);
        EXPECT_EQ(1U, sl.versionNumber);
        EXPECT_EQ(1U, update1.version_number());
        EXPECT_EQ(1, update1.server_size());
        EXPECT_TRUE(protoBufMatchesEntry(update1.server(0),
            *sl.serverList[1].entry, ServerStatus::UP));
    }

    {
        ProtoBuf::ServerList update2;
        EXPECT_EQ(ServerId(2, 0), sl.add("hi again",
                                         {BACKUP_SERVICE}, 100, update2));
        EXPECT_TRUE(sl.serverList[2].entry);
        EXPECT_EQ(ServerId(2, 0), sl.serverList[2].entry->serverId);
        EXPECT_EQ("hi again", sl.serverList[2].entry->serviceLocator);
        EXPECT_FALSE(sl.serverList[2].entry->isMaster());
        EXPECT_TRUE(sl.serverList[2].entry->isBackup());
        EXPECT_EQ(100u, sl.serverList[2].entry->expectedReadMBytesPerSec);
        EXPECT_EQ(1U, sl.serverList[2].nextGenerationNumber);
        EXPECT_EQ(1U, sl.numberOfMasters);
        EXPECT_EQ(1U, sl.numberOfBackups);
        EXPECT_EQ(1U, sl.versionNumber);
        sl.incrementVersion(update2);
        EXPECT_EQ(2U, sl.versionNumber);
        EXPECT_EQ(2U, update2.version_number());
        EXPECT_TRUE(protoBufMatchesEntry(update2.server(0),
            *sl.serverList[2].entry, ServerStatus::UP));
    }
}

TEST_F(CoordinatorServerListTest, add_trackerUpdated) {
    sl.registerTracker(tr);
    ProtoBuf::ServerList update;
    TestLog::Enable _;
    sl.add("hi!", {MASTER_SERVICE}, 100, update);
    EXPECT_EQ("fireCallback: called", TestLog::get());
    ASSERT_FALSE(tr.changes.empty());
    auto& server = tr.changes.front().server;
    EXPECT_EQ(ServerId(1, 0), server.serverId);
    EXPECT_EQ("hi!", server.serviceLocator);
    EXPECT_EQ("MASTER_SERVICE", server.services.toString());
    // Not set when no BACKUP_SERVICE.
    EXPECT_EQ(0u, server.expectedReadMBytesPerSec);
    EXPECT_EQ(ServerStatus::UP, server.status);
    EXPECT_EQ(SERVER_ADDED, tr.changes.front().event);
}

TEST_F(CoordinatorServerListTest, crashed) {
    ProtoBuf::ServerList update;

    EXPECT_THROW(sl.crashed(ServerId(0, 0), update), Exception);
    EXPECT_EQ(0, update.server_size());

    sl.add("hi!", {MASTER_SERVICE}, 100, update);
    CoordinatorServerList::Entry entryCopy = sl[ServerId(1, 0)];
    update.Clear();
    EXPECT_NO_THROW(sl.crashed(ServerId(1, 0), update));
    ASSERT_TRUE(sl.serverList[1].entry);
    EXPECT_EQ(ServerStatus::CRASHED, sl.serverList[1].entry->status);
    EXPECT_TRUE(protoBufMatchesEntry(update.server(0),
                                     entryCopy, ServerStatus::CRASHED));

    update.Clear();
    // Already crashed; a no-op.
    sl.crashed(ServerId(1, 0), update);
    EXPECT_EQ(0, update.server_size());
    EXPECT_EQ(0U, sl.numberOfMasters);
    EXPECT_EQ(0U, sl.numberOfBackups);
}

TEST_F(CoordinatorServerListTest, crashed_trackerUpdated) {
    sl.registerTracker(tr);
    ProtoBuf::ServerList update;
    TestLog::Enable _;
    ServerId serverId = sl.add("hi!", {MASTER_SERVICE}, 100, update);
    sl.crashed(serverId, update);
    EXPECT_EQ("fireCallback: called | fireCallback: called", TestLog::get());
    ASSERT_FALSE(tr.changes.empty());
    tr.changes.pop();
    ASSERT_FALSE(tr.changes.empty());
    auto& server = tr.changes.front().server;
    EXPECT_EQ(serverId, server.serverId);
    EXPECT_EQ("hi!", server.serviceLocator);
    EXPECT_EQ("MASTER_SERVICE", server.services.toString());
    // Not set when no BACKUP_SERVICE.
    EXPECT_EQ(0u, server.expectedReadMBytesPerSec);
    EXPECT_EQ(ServerStatus::CRASHED, server.status);
    EXPECT_EQ(SERVER_CRASHED, tr.changes.front().event);
}

TEST_F(CoordinatorServerListTest, remove) {
    ProtoBuf::ServerList addUpdate, removeUpdate;

    EXPECT_THROW(sl.remove(ServerId(0, 0), removeUpdate), Exception);
    EXPECT_EQ(0, removeUpdate.server_size());

    sl.add("hi!", {MASTER_SERVICE}, 100, addUpdate);
    CoordinatorServerList::Entry entryCopy = sl[ServerId(1, 0)];
    EXPECT_NO_THROW(sl.remove(ServerId(1, 0), removeUpdate));
    EXPECT_FALSE(sl.serverList[1].entry);
    EXPECT_TRUE(protoBufMatchesEntry(removeUpdate.server(0),
            entryCopy, ServerStatus::CRASHED));
    EXPECT_TRUE(protoBufMatchesEntry(removeUpdate.server(1),
            entryCopy, ServerStatus::DOWN));

    EXPECT_THROW(sl.remove(ServerId(1, 0), removeUpdate), Exception);
    EXPECT_EQ(0U, sl.numberOfMasters);
    EXPECT_EQ(0U, sl.numberOfBackups);

    removeUpdate.Clear();
    sl.add("hi, again", {BACKUP_SERVICE}, 100, addUpdate);
    sl.crashed(ServerId(1, 1), addUpdate);
    EXPECT_TRUE(sl.serverList[1].entry);
    EXPECT_THROW(sl.remove(ServerId(1, 2), removeUpdate), Exception);
    EXPECT_NO_THROW(sl.remove(ServerId(1, 1), removeUpdate));
    EXPECT_EQ(uint32_t(ServerStatus::DOWN), removeUpdate.server(0).status());
    EXPECT_EQ(0U, sl.numberOfMasters);
    EXPECT_EQ(0U, sl.numberOfBackups);
}

TEST_F(CoordinatorServerListTest, remove_trackerUpdated) {
    sl.registerTracker(tr);
    ProtoBuf::ServerList update;
    TestLog::Enable _;
    ServerId serverId = sl.add("hi!", {MASTER_SERVICE}, 100, update);
    sl.remove(serverId, update);
    EXPECT_EQ("fireCallback: called | fireCallback: called | "
              "fireCallback: called", TestLog::get());
    ASSERT_FALSE(tr.changes.empty());
    tr.changes.pop();
    ASSERT_FALSE(tr.changes.empty());
    tr.changes.pop();
    ASSERT_FALSE(tr.changes.empty());
    auto& server = tr.changes.front().server;
    EXPECT_EQ(serverId, server.serverId);
    EXPECT_EQ("hi!", server.serviceLocator);
    EXPECT_EQ("MASTER_SERVICE", server.services.toString());
    // Not set when no BACKUP_SERVICE.
    EXPECT_EQ(0u, server.expectedReadMBytesPerSec);
    EXPECT_EQ(ServerStatus::DOWN, server.status);
    EXPECT_EQ(SERVER_REMOVED, tr.changes.front().event);
}

TEST_F(CoordinatorServerListTest, incrementVersion) {
    ProtoBuf::ServerList update;
    sl.incrementVersion(update);
    EXPECT_EQ(1u, sl.versionNumber);
    EXPECT_EQ(1u, update.version_number());
}

TEST_F(CoordinatorServerListTest, addToWill) {
    ProtoBuf::ServerList update;
    auto serverId = sl.add("chondrogenetic", {MASTER_SERVICE}, 0, update);

    ProtoBuf::Tablets will;
    ProtoBuf::Tablets::Tablet& t(*will.add_tablet());
    t.set_table_id(0);
    t.set_start_key_hash(235);
    t.set_end_key_hash(47234);
    t.set_state(ProtoBuf::Tablets::Tablet::NORMAL);
    t.set_user_data(~0ul);
    t.set_ctime_log_head_id(0);
    t.set_ctime_log_head_offset(0);
    ProtoBuf::Tablets::Tablet& t2(*will.add_tablet());
    t2.set_table_id(1);
    t2.set_start_key_hash(236);
    t2.set_end_key_hash(47235);
    t2.set_state(ProtoBuf::Tablets::Tablet::NORMAL);
    t2.set_user_data(~0ul);
    t2.set_ctime_log_head_id(0);
    t2.set_ctime_log_head_offset(0);

    sl.addToWill(serverId, will);
    auto entry = sl[serverId];
    auto tablet = entry.will->tablet(0);
    EXPECT_EQ(0u, tablet.table_id());
    EXPECT_EQ(235lu, tablet.start_key_hash());
    EXPECT_EQ(47234lu, tablet.end_key_hash());
    EXPECT_EQ(0lu, tablet.user_data());
    tablet = entry.will->tablet(1);
    EXPECT_EQ(1u, tablet.table_id());
    EXPECT_EQ(236lu, tablet.start_key_hash());
    EXPECT_EQ(47235lu, tablet.end_key_hash());
    EXPECT_EQ(1lu, tablet.user_data());
}

static bool
setWillFilter(string s) {
    return s == "setWill";
}

TEST_F(CoordinatorServerListTest, setWill) {
    ProtoBuf::ServerList update;
    auto server1 = sl.add("habitudinal coyoting", {MASTER_SERVICE}, 0, update);
    auto server2 = sl.add("archeocyte accompany", {BACKUP_SERVICE}, 0, update);

    ProtoBuf::Tablets will;
    ProtoBuf::Tablets::Tablet& t(*will.add_tablet());
    t.set_table_id(0);
    t.set_start_key_hash(235);
    t.set_end_key_hash(47234);
    t.set_state(ProtoBuf::Tablets::Tablet::NORMAL);
    t.set_user_data(19);
    t.set_ctime_log_head_id(0);
    t.set_ctime_log_head_offset(0);

    TestLog::Enable _(&setWillFilter);
    sl.setWill(server1, will);
    EXPECT_EQ("setWill: Master 1 updated its Will (now 1 entries, was 0)",
              TestLog::get());

    TestLog::reset();
    sl.setWill(server2, will);
    EXPECT_EQ("setWill: Server 2 is not a master! Ignoring new will.",
              TestLog::get());

    // bad master id should fail
    EXPECT_THROW(sl.setWill({23481234, 0}, will), Exception);
}

TEST_F(CoordinatorServerListTest, indexOperator) {
    ProtoBuf::ServerList update;
    EXPECT_THROW(sl[ServerId(0, 0)], Exception);
    sl.add("yo!", {MASTER_SERVICE}, 100, update);
    EXPECT_EQ(ServerId(1, 0), sl[ServerId(1, 0)].serverId);
    EXPECT_EQ("yo!", sl[ServerId(1, 0)].serviceLocator);
    sl.crashed(ServerId(1, 0), update);
    sl.remove(ServerId(1, 0), update);
    EXPECT_THROW(sl[ServerId(1, 0)], Exception);
}

TEST_F(CoordinatorServerListTest, contains) {
    ProtoBuf::ServerList update;

    EXPECT_FALSE(sl.contains(ServerId(0, 0)));
    EXPECT_FALSE(sl.contains(ServerId(1, 0)));

    sl.add("I love it when a plan comes together",
           {BACKUP_SERVICE}, 100, update);
    EXPECT_TRUE(sl.contains(ServerId(1, 0)));

    sl.add("Come with me if you want to live",
           {MASTER_SERVICE}, 100, update);
    EXPECT_TRUE(sl.contains(ServerId(2, 0)));

    sl.crashed(ServerId(1, 0), update);
    EXPECT_TRUE(sl.contains(ServerId(1, 0)));
    sl.remove(ServerId(1, 0), update);
    EXPECT_FALSE(sl.contains(ServerId(1, 0)));

    sl.crashed(ServerId(2, 0), update);
    sl.remove(ServerId(2, 0), update);
    EXPECT_FALSE(sl.contains(ServerId(2, 0)));

    sl.add("I'm running out 80s shows and action movie quotes",
           {BACKUP_SERVICE}, 100, update);
    EXPECT_TRUE(sl.contains(ServerId(1, 1)));
}

TEST_F(CoordinatorServerListTest, nextMasterIndex) {
    ProtoBuf::ServerList update;

    EXPECT_EQ(-1U, sl.nextMasterIndex(0));
    sl.add("", {BACKUP_SERVICE}, 100, update);
    sl.add("", {MASTER_SERVICE}, 100, update);
    sl.add("", {BACKUP_SERVICE}, 100, update);
    sl.add("", {BACKUP_SERVICE}, 100, update);
    sl.add("", {MASTER_SERVICE}, 100, update);
    sl.add("", {BACKUP_SERVICE}, 100, update);

    EXPECT_EQ(2U, sl.nextMasterIndex(0));
    EXPECT_EQ(2U, sl.nextMasterIndex(2));
    EXPECT_EQ(5U, sl.nextMasterIndex(3));
    EXPECT_EQ(-1U, sl.nextMasterIndex(6));
}

TEST_F(CoordinatorServerListTest, nextBackupIndex) {
    ProtoBuf::ServerList update;

    EXPECT_EQ(-1U, sl.nextMasterIndex(0));
    sl.add("", {MASTER_SERVICE}, 100, update);
    sl.add("", {BACKUP_SERVICE}, 100, update);
    sl.add("", {MASTER_SERVICE}, 100, update);

    EXPECT_EQ(2U, sl.nextBackupIndex(0));
    EXPECT_EQ(2U, sl.nextBackupIndex(2));
    EXPECT_EQ(-1U, sl.nextBackupIndex(3));
}

TEST_F(CoordinatorServerListTest, serialize) {
    ProtoBuf::ServerList update;

    {
        ProtoBuf::ServerList serverList;
        sl.serialize(serverList, {});
        EXPECT_EQ(0, serverList.server_size());
        sl.serialize(serverList, {MASTER_SERVICE, BACKUP_SERVICE});
        EXPECT_EQ(0, serverList.server_size());
    }

    ServerId first = sl.add("", {MASTER_SERVICE}, 100, update);
    sl.add("", {MASTER_SERVICE}, 100, update);
    sl.add("", {MASTER_SERVICE}, 100, update);
    sl.add("", {BACKUP_SERVICE}, 100, update);
    sl.remove(first, update);       // ensure removed entries are skipped

    auto masterMask = ServiceMask{MASTER_SERVICE}.serialize();
    auto backupMask = ServiceMask{BACKUP_SERVICE}.serialize();
    {
        ProtoBuf::ServerList serverList;
        sl.serialize(serverList, {});
        EXPECT_EQ(0, serverList.server_size());
        sl.serialize(serverList, {MASTER_SERVICE});
        EXPECT_EQ(2, serverList.server_size());
        EXPECT_EQ(masterMask, serverList.server(0).services());
        EXPECT_EQ(masterMask, serverList.server(1).services());
    }

    {
        ProtoBuf::ServerList serverList;
        sl.serialize(serverList, {BACKUP_SERVICE});
        EXPECT_EQ(1, serverList.server_size());
        EXPECT_EQ(backupMask, serverList.server(0).services());
    }

    {
        ProtoBuf::ServerList serverList;
        sl.serialize(serverList, {MASTER_SERVICE, BACKUP_SERVICE});
        EXPECT_EQ(3, serverList.server_size());
        EXPECT_EQ(masterMask, serverList.server(0).services());
        EXPECT_EQ(masterMask, serverList.server(1).services());
        EXPECT_EQ(backupMask, serverList.server(2).services());
    }
}

namespace {
bool statusFilter(string s) {
    return s != "checkStatus";
}
}

TEST_F(CoordinatorServerListTest, sendMembershipUpdate) {
    MockTransport transport;
    TransportManager::MockRegistrar _(transport);

    ProtoBuf::ServerList update;
    // Test unoccupied server slot. Remove must wait until after last add to
    // ensure slot isn't recycled.
    ServerId serverId1 =
        sl.add("mock:host=server1", {MEMBERSHIP_SERVICE}, 0, update);

    // Test crashed server gets skipped.
    ServerId serverId2 = sl.add("mock:host=server2", {}, 0, update);
    sl.crashed(serverId2, update);

    // Test server with no membership service.
    ServerId serverId3 = sl.add("mock:host=server3", {}, 0, update);

    // Test exclude list.
    ServerId serverId4 =
        sl.add("mock:host=server4", {MEMBERSHIP_SERVICE}, 0, update);
    sl.remove(serverId1, update);

    update.Clear();
    update.set_version_number(1);
    TestLog::Enable __(statusFilter);
    sl.sendMembershipUpdate(update, serverId4);

    // Nothing should be sent. All servers are invalid recipients for
    // various reasons.
    EXPECT_EQ("", transport.outputLog);
    EXPECT_EQ("", TestLog::get());

    ServerId serverId5 =
        sl.add("mock:host=server5", {MEMBERSHIP_SERVICE}, 0, update);

    update.Clear();
    update.set_version_number(1);

    transport.setInput("0 1"); // Server 5 (in the first slot) has trouble.
    transport.setInput("0");   // Server 5 ok to the send of the entire list.
    transport.setInput("0 0"); // Server 4 gets the update just fine.

    TestLog::reset();
    transport.outputLog = "";
    sl.sendMembershipUpdate(update, {});

    EXPECT_EQ("clientSend: 0x40024 9 273 0 /0 | " // Update to server 5.
              "clientSend: 0x40023 9 17 0 /0 | "  // Set list to server 5.
              "clientSend: 0x40024 9 273 0 /0",   // Update to server 4.
              transport.outputLog);
    EXPECT_EQ("sendMembershipUpdate: Server 4294967297 had lost an update. "
              "Sending whole list.",
              TestLog::get());
}

TEST_F(CoordinatorServerListTest, firstFreeIndex) {
    ProtoBuf::ServerList update;

    EXPECT_EQ(0U, sl.serverList.size());
    EXPECT_EQ(1U, sl.firstFreeIndex());
    EXPECT_EQ(2U, sl.serverList.size());
    sl.add("hi", {MASTER_SERVICE}, 100, update);
    EXPECT_EQ(2U, sl.firstFreeIndex());
    sl.add("hi again", {MASTER_SERVICE}, 100, update);
    EXPECT_EQ(3U, sl.firstFreeIndex());
    sl.remove(ServerId(2, 0), update);
    EXPECT_EQ(2U, sl.firstFreeIndex());
    sl.remove(ServerId(1, 0), update);
    EXPECT_EQ(1U, sl.firstFreeIndex());
}

TEST_F(CoordinatorServerListTest, registerTracker) {
    sl.registerTracker(tr);
    EXPECT_EQ(1U, sl.trackers.size());
    EXPECT_EQ(&tr, sl.trackers[0]);
    EXPECT_THROW(sl.registerTracker(tr), Exception);
}

TEST_F(CoordinatorServerListTest, registerTracker_pushAdds) {
    ProtoBuf::ServerList update;
    auto serverId1 = sl.add("mock:", {}, 100, update);
    auto serverId2 = sl.add("mock:", {}, 100, update);
    auto serverId3 = sl.add("mock:", {}, 100, update);
    auto serverId4 = sl.add("mock:", {}, 100, update);
    sl.crashed(serverId4, update);
    sl.remove(serverId2, update);
    sl.registerTracker(tr);

    // Should be serverId4 up/crashed first, then in order,
    // but missing serverId2
    EXPECT_EQ(4U, tr.changes.size());
    EXPECT_EQ(serverId4, tr.changes.front().server.serverId);
    EXPECT_EQ(ServerChangeEvent::SERVER_ADDED, tr.changes.front().event);
    tr.changes.pop();
    EXPECT_EQ(serverId4, tr.changes.front().server.serverId);
    EXPECT_EQ(ServerChangeEvent::SERVER_CRASHED, tr.changes.front().event);
    tr.changes.pop();
    EXPECT_EQ(serverId1, tr.changes.front().server.serverId);
    EXPECT_EQ(ServerChangeEvent::SERVER_ADDED, tr.changes.front().event);
    tr.changes.pop();
    EXPECT_EQ(serverId3, tr.changes.front().server.serverId);
    EXPECT_EQ(ServerChangeEvent::SERVER_ADDED, tr.changes.front().event);
    tr.changes.pop();
}

TEST_F(CoordinatorServerListTest, unregisterTracker) {
    EXPECT_EQ(0U, sl.trackers.size());

    sl.unregisterTracker(tr);
    EXPECT_EQ(0U, sl.trackers.size());

    sl.registerTracker(tr);
    EXPECT_EQ(1U, sl.trackers.size());

    sl.unregisterTracker(tr);
    EXPECT_EQ(0U, sl.trackers.size());
}

TEST_F(CoordinatorServerListTest, getReferenceFromServerId) {
    ProtoBuf::ServerList update;

    EXPECT_THROW(sl.getReferenceFromServerId(ServerId(0, 0)), Exception);
    EXPECT_THROW(sl.getReferenceFromServerId(ServerId(1, 0)), Exception);

    sl.add("", {MASTER_SERVICE}, 100, update);
    EXPECT_THROW(sl.getReferenceFromServerId(ServerId(0, 0)), Exception);
    EXPECT_NO_THROW(sl.getReferenceFromServerId(ServerId(1, 0)));
    EXPECT_THROW(sl.getReferenceFromServerId(ServerId(2, 0)), Exception);
}

TEST_F(CoordinatorServerListTest, Entry_constructor) {
    CoordinatorServerList::Entry a(ServerId(52, 374),
        "You forgot your boarding pass", {MASTER_SERVICE});
    EXPECT_EQ(ServerId(52, 374), a.serverId);
    EXPECT_EQ("You forgot your boarding pass", a.serviceLocator);
    EXPECT_TRUE(a.isMaster());
    EXPECT_FALSE(a.isBackup());
    EXPECT_EQ(static_cast<ProtoBuf::Tablets*>(NULL), a.will);
    EXPECT_EQ(0U, a.expectedReadMBytesPerSec);

    CoordinatorServerList::Entry b(ServerId(27, 72),
        "I ain't got time to bleed", {BACKUP_SERVICE});
    EXPECT_EQ(ServerId(27, 72), b.serverId);
    EXPECT_EQ("I ain't got time to bleed", b.serviceLocator);
    EXPECT_FALSE(b.isMaster());
    EXPECT_TRUE(b.isBackup());
    EXPECT_EQ(static_cast<ProtoBuf::Tablets*>(NULL), b.will);
    EXPECT_EQ(0U, b.expectedReadMBytesPerSec);
}

TEST_F(CoordinatorServerListTest, Entry_serialize) {
    CoordinatorServerList::Entry entry(ServerId(0, 0), "",
                                       {BACKUP_SERVICE});
    entry.serverId = ServerId(5234, 23482);
    entry.serviceLocator = "giggity";
    entry.expectedReadMBytesPerSec = 723;

    ProtoBuf::ServerList_Entry serialEntry;
    entry.serialize(serialEntry);
    auto backupMask = ServiceMask{BACKUP_SERVICE}.serialize();
    EXPECT_EQ(backupMask, serialEntry.services());
    EXPECT_EQ(ServerId(5234, 23482).getId(), serialEntry.server_id());
    EXPECT_EQ("giggity", serialEntry.service_locator());
    EXPECT_EQ(723U, serialEntry.expected_read_mbytes_per_sec());
    EXPECT_EQ(ServerStatus::UP, ServerStatus(serialEntry.status()));

    entry.services = ServiceMask{MASTER_SERVICE};
    ProtoBuf::ServerList_Entry serialEntry2;
    entry.serialize(serialEntry2);
    auto masterMask = ServiceMask{MASTER_SERVICE}.serialize();
    EXPECT_EQ(masterMask, serialEntry2.services());
    EXPECT_EQ(0U, serialEntry2.expected_read_mbytes_per_sec());
    EXPECT_EQ(ServerStatus::UP, ServerStatus(serialEntry2.status()));
}

}  // namespace RAMCloud
