// Copyright (c) 2014, Cloudera, inc.
// Confidential Cloudera Information: Covered by NDA.

#include <gtest/gtest.h>
#include <boost/assign/list_of.hpp>

#include "kudu/client/client.h"
#include "kudu/integration-tests/mini_cluster.h"
#include "kudu/master/mini_master.h"
#include "kudu/tools/data_gen_util.h"
#include "kudu/tools/ksck_remote.h"
#include "kudu/util/monotime.h"
#include "kudu/util/random.h"
#include "kudu/util/test_util.h"

DECLARE_int32(heartbeat_interval_ms);

namespace kudu {
namespace tools {

using client::KuduColumnSchema;
using client::KuduInsert;
using client::KuduSession;
using client::KuduTable;
using client::KuduTableCreator;
using std::tr1::static_pointer_cast;
using std::tr1::shared_ptr;
using std::vector;
using std::string;

static const char *kTableName = "ksck-test-table";

class RemoteKsckTest : public KuduTest {
 public:
  RemoteKsckTest()
      : schema_(boost::assign::list_of
                (KuduColumnSchema("key", KuduColumnSchema::INT32))
                (KuduColumnSchema("int_val", KuduColumnSchema::INT32)),
                1),
        random_(SeedRandom()) {
  }

  virtual void SetUp() OVERRIDE {
    KuduTest::SetUp();

    // Speed up testing, saves about 700ms per TEST_F.
    FLAGS_heartbeat_interval_ms = 10;

    MiniClusterOptions opts;
    opts.num_tablet_servers = 3;
    mini_cluster_.reset(new MiniCluster(env_.get(), opts));
    ASSERT_OK(mini_cluster_->Start());

    master_rpc_addr_ = mini_cluster_->mini_master()->bound_rpc_addr();

    // Connect to the cluster.
    ASSERT_OK(client::KuduClientBuilder()
                     .add_master_server_addr(master_rpc_addr_.ToString())
                     .Build(&client_));

    // Create one table.
    gscoped_ptr<KuduTableCreator> table_creator(client_->NewTableCreator());
    ASSERT_OK(table_creator->table_name(kTableName)
                     .schema(&schema_)
                     .num_replicas(3)
                     .split_keys(GenerateSplitKeys())
                     .Create());
    // Make sure we can open the table.
    ASSERT_OK(client_->OpenTable(kTableName, &client_table_));

    ASSERT_OK(RemoteKsckMaster::Build(master_rpc_addr_, &master_));
    cluster_.reset(new KsckCluster(master_));
    ksck_.reset(new Ksck(cluster_));
  }

  virtual void TearDown() OVERRIDE {
    if (mini_cluster_) {
      mini_cluster_->Shutdown();
      mini_cluster_.reset();
    }
    KuduTest::TearDown();
  }

  // Writes rows to the table until the continue_writing flag is set to false.
  //
  // Public for use with boost::bind.
  void GenerateRowWritesLoop(CountDownLatch* started_writing,
                             const AtomicBool& continue_writing,
                             Promise<Status>* promise) {
    scoped_refptr<KuduTable> table;
    Status status;
    status = client_->OpenTable(kTableName, &table);
    if (!status.ok()) {
      promise->Set(status);
    }
    shared_ptr<KuduSession> session(client_->NewSession());
    session->SetTimeoutMillis(10000);
    status = session->SetFlushMode(KuduSession::MANUAL_FLUSH);
    if (!status.ok()) {
      promise->Set(status);
    }

    for (uint64_t i = 0; continue_writing.Load(); i++) {
      gscoped_ptr<KuduInsert> insert(table->NewInsert());
      GenerateDataForRow(table->schema(), i, &random_, insert->mutable_row());
      status = session->Apply(insert.release());
      if (!status.ok()) {
        promise->Set(status);
      }
      status = session->Flush();
      if (!status.ok()) {
        promise->Set(status);
      }
      started_writing->CountDown(1);
    }
    promise->Set(Status::OK());
  }

 protected:
  // Generate a set of split keys for tablets used in this test.
  vector<string> GenerateSplitKeys() {
    vector<string> keys;
    vector<int> split_nums = boost::assign::list_of(33)(66);
    BOOST_FOREACH(int i, split_nums) {
      gscoped_ptr<KuduPartialRow> key(schema_.NewRow());
      CHECK_OK(key->SetInt32(0, i));
      keys.push_back(key->ToEncodedRowKeyOrDie());
    }
    return keys;
  }

  Status GenerateRowWrites(uint64_t num_rows) {
    scoped_refptr<KuduTable> table;
    RETURN_NOT_OK(client_->OpenTable(kTableName, &table));
    shared_ptr<KuduSession> session(client_->NewSession());
    session->SetTimeoutMillis(10000);
    RETURN_NOT_OK(session->SetFlushMode(KuduSession::MANUAL_FLUSH));
    for (uint64_t i = 0; i < num_rows; i++) {
      VLOG(1) << "Generating write for row id " << i;
      gscoped_ptr<KuduInsert> insert(table->NewInsert());
      GenerateDataForRow(table->schema(), i, &random_, insert->mutable_row());
      RETURN_NOT_OK(session->Apply(insert.release()));
    }
    RETURN_NOT_OK(session->Flush());
    return Status::OK();
  }

  shared_ptr<Ksck> ksck_;
  shared_ptr<client::KuduClient> client_;

 private:
  Sockaddr master_rpc_addr_;
  shared_ptr<MiniCluster> mini_cluster_;
  client::KuduSchema schema_;
  scoped_refptr<client::KuduTable> client_table_;
  shared_ptr<KsckMaster> master_;
  shared_ptr<KsckCluster> cluster_;
  Random random_;
};

TEST_F(RemoteKsckTest, TestMasterOk) {
  ASSERT_OK(ksck_->CheckMasterRunning());
}

TEST_F(RemoteKsckTest, TestTabletServersOk) {
  ASSERT_OK(ksck_->FetchTableAndTabletInfo());
  ASSERT_OK(ksck_->CheckTabletServersRunning());
}

TEST_F(RemoteKsckTest, TestTableConsistency) {
  Status s;
  // We may have to sleep and loop because it takes some time for the
  // tablet leader to be elected and report back to the Master.
  for (int i = 1; i <= 10; i++) {
    LOG(INFO) << "Consistency check attempt " << i << "...";
    SleepFor(MonoDelta::FromMilliseconds(700));
    ASSERT_OK(ksck_->FetchTableAndTabletInfo());
    s = ksck_->CheckTablesConsistency();
    if (s.ok()) break;
  }
  ASSERT_OK(s);
}

TEST_F(RemoteKsckTest, TestChecksum) {
  uint64_t num_writes = 100;
  LOG(INFO) << "Generating row writes...";
  ASSERT_OK(GenerateRowWrites(num_writes));
  ASSERT_OK(ksck_->FetchTableAndTabletInfo());
  Status s;
  // We may have to sleep and loop because it may take a little while for all
  // followers to sync up with the leader.
  for (int i = 1; i <= 10; i++) {
    LOG(INFO) << "Checksum attempt " << i << "...";
    SleepFor(MonoDelta::FromMilliseconds(700));
    s = ksck_->ChecksumData(vector<string>(),
                            vector<string>(),
                            ChecksumOptions(MonoDelta::FromSeconds(1), 16, false, 0));
    if (s.ok()) break;
  }
  ASSERT_OK(s);
}

TEST_F(RemoteKsckTest, TestChecksumTimeout) {
  uint64_t num_writes = 100;
  LOG(INFO) << "Generating row writes...";
  ASSERT_OK(GenerateRowWrites(num_writes));
  ASSERT_OK(ksck_->FetchTableAndTabletInfo());
  // Use an impossibly low timeout value of zero!
  Status s = ksck_->ChecksumData(vector<string>(),
                                 vector<string>(),
                                 ChecksumOptions(MonoDelta::FromNanoseconds(0), 16, false, 0));
  ASSERT_TRUE(s.IsTimedOut()) << "Expected TimedOut Status, got: " << s.ToString();
}

TEST_F(RemoteKsckTest, TestChecksumSnapshot) {
  CountDownLatch started_writing(1);
  AtomicBool continue_writing(true);
  Promise<Status> promise;
  scoped_refptr<Thread> writer_thread;

  Thread::Create("RemoteKsckTest", "TestChecksumSnapshot",
                 &RemoteKsckTest::GenerateRowWritesLoop, this,
                 &started_writing, boost::cref(continue_writing), &promise,
                 &writer_thread);
  CHECK(started_writing.WaitFor(MonoDelta::FromSeconds(1)));

  ASSERT_OK(ksck_->FetchTableAndTabletInfo());
  ASSERT_OK(ksck_->ChecksumData(vector<string>(), vector<string>(),
                                ChecksumOptions(MonoDelta::FromSeconds(10), 16, true,
                                                client_->GetLatestObservedTimestamp())));
  continue_writing.Store(false);
  ASSERT_OK(promise.Get());
  writer_thread->Join();
}

TEST_F(RemoteKsckTest, TestChecksumSnapshotCurrentTimestamp) {
  CountDownLatch started_writing(1);
  AtomicBool continue_writing(true);
  Promise<Status> promise;
  scoped_refptr<Thread> writer_thread;

  Thread::Create("RemoteKsckTest", "TestChecksumSnapshot",
                 &RemoteKsckTest::GenerateRowWritesLoop, this,
                 &started_writing, boost::cref(continue_writing), &promise,
                 &writer_thread);
  CHECK(started_writing.WaitFor(MonoDelta::FromSeconds(1)));

  ASSERT_OK(ksck_->FetchTableAndTabletInfo());
  ASSERT_OK(ksck_->ChecksumData(vector<string>(), vector<string>(),
                                ChecksumOptions(MonoDelta::FromSeconds(10), 16, true,
                                                ChecksumOptions::kCurrentTimestamp)));
  continue_writing.Store(false);
  ASSERT_OK(promise.Get());
  writer_thread->Join();
}

} // namespace tools
} // namespace kudu
