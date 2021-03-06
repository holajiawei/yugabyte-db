// Copyright (c) YugaByte, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file except
// in compliance with the License.  You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software distributed under the License
// is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express
// or implied.  See the License for the specific language governing permissions and limitations
// under the License.

#include "yb/tools/yb-admin_client.h"

#include <iostream>

#include <boost/algorithm/string.hpp>

#include "yb/cdc/cdc_service.h"
#include "yb/cdc/cdc_service.proxy.h"
#include "yb/client/client.h"
#include "yb/common/wire_protocol.h"
#include "yb/master/master_defaults.h"
#include "yb/rpc/messenger.h"
#include "yb/util/cast.h"
#include "yb/util/env.h"
#include "yb/util/pb_util.h"
#include "yb/util/protobuf_util.h"
#include "yb/util/string_util.h"
#include "yb/util/encryption_util.h"

DECLARE_string(certs_dir);
DECLARE_bool(use_client_to_server_encryption);

namespace yb {

using enterprise::EncryptionParams;

namespace tools {
namespace enterprise {

using std::cout;
using std::endl;
using std::string;
using std::vector;

using google::protobuf::RepeatedPtrField;

using client::YBTableName;
using rpc::RpcController;

using master::ChangeEncryptionInfoRequestPB;
using master::ChangeEncryptionInfoResponsePB;
using master::CreateSnapshotRequestPB;
using master::CreateSnapshotResponsePB;
using master::DeleteSnapshotRequestPB;
using master::DeleteSnapshotResponsePB;
using master::IdPairPB;
using master::ImportSnapshotMetaRequestPB;
using master::ImportSnapshotMetaResponsePB;
using master::ImportSnapshotMetaResponsePB_TableMetaPB;
using master::IsCreateTableDoneRequestPB;
using master::IsCreateTableDoneResponsePB;
using master::ListSnapshotsRequestPB;
using master::ListSnapshotsResponsePB;
using master::ListTabletServersRequestPB;
using master::ListTabletServersResponsePB;
using master::RestoreSnapshotRequestPB;
using master::RestoreSnapshotResponsePB;
using master::SnapshotInfoPB;
using master::SysNamespaceEntryPB;
using master::SysRowEntry;
using master::SysTablesEntryPB;

using yb::util::to_uchar_ptr;

using namespace std::literals;

PB_ENUM_FORMATTERS(yb::master::SysSnapshotEntryPB::State);

Status ClusterAdminClient::Init() {
  RETURN_NOT_OK(super::Init());
  DCHECK(initted_);

  if (!certs_dir_.empty()) {
    rpc::MessengerBuilder messenger_builder("yb-admin");
    FLAGS_use_client_to_server_encryption = true;
    FLAGS_certs_dir = certs_dir_;
    secure_context_ = VERIFY_RESULT(server::SetupSecureContext(
        "", "", server::SecureContextType::kClientToServer, &messenger_builder));
    messenger_ = VERIFY_RESULT(messenger_builder.Build());
    client::YBClientBuilder yb_builder;
    yb_client_ = CHECK_RESULT(yb_builder
        .add_master_server_addr(master_addr_list_)
        .default_admin_operation_timeout(timeout_)
        .Build(messenger_.get()));
    proxy_cache_ = std::make_unique<rpc::ProxyCache>(messenger_.get());

    // Find the leader master's socket info to set up the proxy
    leader_addr_ = yb_client_->GetMasterLeaderAddress();
    master_proxy_.reset(new master::MasterServiceProxy(proxy_cache_.get(), leader_addr_));

    LOG(INFO) << "Built secure client using certs dir " << certs_dir_;;
  }

  rpc::ProxyCache proxy_cache(messenger_.get());
  master_backup_proxy_.reset(new master::MasterBackupServiceProxy(&proxy_cache, leader_addr_));
  return Status::OK();
}

Status ClusterAdminClient::ListSnapshots() {
  RpcController rpc;
  rpc.set_timeout(timeout_);
  ListSnapshotsRequestPB req;
  ListSnapshotsResponsePB resp;
  RETURN_NOT_OK(master_backup_proxy_->ListSnapshots(req, &resp, &rpc));

  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  if (resp.has_current_snapshot_id()) {
    cout << "Current snapshot id: " << resp.current_snapshot_id() << endl;
  }

  if (resp.snapshots_size()) {
    cout << RightPadToUuidWidth("Snapshot UUID") << kColumnSep
         << "State" << endl;
  } else {
    cout << "No snapshots" << endl;
  }

  for (int i = 0; i < resp.snapshots_size(); ++i) {
    cout << resp.snapshots(i).id() << kColumnSep
         << resp.snapshots(i).entry().state() << endl;
  }

  return Status::OK();
}

Status ClusterAdminClient::CreateSnapshot(const vector<YBTableName>& tables,
                                          int flush_timeout_secs) {
  if (flush_timeout_secs > 0) {
    for (const YBTableName& table_name : tables) {
      // Flush table before the snapshot creation.
      const Status s = FlushTable(table_name, flush_timeout_secs, false /* is_compaction */);
      // Expected statuses:
      //   OK - table was successfully flushed
      //   NotFound - flush request was finished & deleted
      //   TimedOut - flush request failed by timeout
      if (s.IsTimedOut()) {
        cout << s.ToString(false) << " (ignored)" << endl;
      } else if (!s.ok() && !s.IsNotFound()) {
        return s;
      }
    }
  }

  RpcController rpc;
  rpc.set_timeout(timeout_);
  CreateSnapshotRequestPB req;
  CreateSnapshotResponsePB resp;

  for (const YBTableName& table_name : tables) {
    table_name.SetIntoTableIdentifierPB(req.add_tables());
  }

  RETURN_NOT_OK(master_backup_proxy_->CreateSnapshot(req, &resp, &rpc));

  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  cout << "Started snapshot creation: " << resp.snapshot_id() << endl;
  return Status::OK();
}

Status ClusterAdminClient::RestoreSnapshot(const string& snapshot_id) {
  RpcController rpc;
  rpc.set_timeout(timeout_);

  RestoreSnapshotRequestPB req;
  RestoreSnapshotResponsePB resp;
  req.set_snapshot_id(snapshot_id);
  RETURN_NOT_OK(master_backup_proxy_->RestoreSnapshot(req, &resp, &rpc));

  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  cout << "Started restoring snapshot: " << snapshot_id << endl;
  return Status::OK();
}

Status ClusterAdminClient::DeleteSnapshot(const std::string& snapshot_id) {
  RpcController rpc;
  rpc.set_timeout(timeout_);

  DeleteSnapshotRequestPB req;
  DeleteSnapshotResponsePB resp;
  req.set_snapshot_id(snapshot_id);
  RETURN_NOT_OK(master_backup_proxy_->DeleteSnapshot(req, &resp, &rpc));

  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  cout << "Deleted snapshot: " << snapshot_id << endl;
  return Status::OK();
}

Status ClusterAdminClient::CreateSnapshotMetaFile(const string& snapshot_id,
                                                  const string& file_name) {
  RpcController rpc;
  rpc.set_timeout(timeout_);
  ListSnapshotsRequestPB req;
  ListSnapshotsResponsePB resp;
  req.set_snapshot_id(snapshot_id);
  RETURN_NOT_OK(master_backup_proxy_->ListSnapshots(req, &resp, &rpc));

  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  const SnapshotInfoPB* snapshot = nullptr;
  for (const auto& snapshot_entry : resp.snapshots()) {
    if (snapshot_entry.id() == snapshot_id) {
      snapshot = &snapshot_entry;
      break;
    }
  }
  if (!snapshot) {
    return STATUS_FORMAT(
        InternalError, "Response contained $0 entries but no entry for snapshot '$1'",
        resp.snapshots_size(), snapshot_id);
  }
  if (resp.snapshots_size() > 1) {
    LOG(WARNING) << "Requested snapshot metadata for snapshot '" << snapshot_id << "', but got "
                 << resp.snapshots_size() << " snapshots in the response";
  }

  cout << "Exporting snapshot " << snapshot_id << " ("
       << snapshot->entry().state() << ") to file " << file_name << endl;

  // Serialize snapshot protobuf to given path.
  RETURN_NOT_OK(pb_util::WritePBContainerToPath(
      Env::Default(), file_name, *snapshot, pb_util::OVERWRITE, pb_util::SYNC));

  cout << "Snapshot meta data was saved into file: " << file_name << endl;
  return Status::OK();
}

Status ClusterAdminClient::ImportSnapshotMetaFile(const string& file_name,
                                                  const vector<YBTableName>& tables) {
  cout << "Read snapshot meta file " << file_name << endl;

  ImportSnapshotMetaRequestPB req;
  ImportSnapshotMetaResponsePB resp;

  SnapshotInfoPB* const snapshot_info = req.mutable_snapshot();

  // Read snapshot protobuf from given path.
  RETURN_NOT_OK(pb_util::ReadPBContainerFromPath(Env::Default(), file_name, snapshot_info));

  cout << "Importing snapshot " << snapshot_info->id()
       << " (" << snapshot_info->entry().state() << ")" << endl;

  YBTableName orig_table_name;
  int table_index = 0;
  for (SysRowEntry& entry : *snapshot_info->mutable_entry()->mutable_entries()) {
    const YBTableName table_name = table_index < tables.size()
        ? tables[table_index] : YBTableName();

    switch (entry.type()) {
      case SysRowEntry::NAMESPACE: {
        SysNamespaceEntryPB meta;
        const string &data = entry.data();
        RETURN_NOT_OK(pb_util::ParseFromArray(&meta, to_uchar_ptr(data.data()), data.size()));
        orig_table_name.set_namespace_name(meta.name());

        if (!table_name.empty() &&
            table_name.namespace_name() != orig_table_name.namespace_name()) {
          meta.set_name(table_name.namespace_name());
          entry.set_data(meta.SerializeAsString());
        }
        break;
      }
      case SysRowEntry::TABLE: {
        SysTablesEntryPB meta;
        const string &data = entry.data();
        RETURN_NOT_OK(pb_util::ParseFromArray(&meta, to_uchar_ptr(data.data()), data.size()));
        orig_table_name.set_table_name(meta.name());

        if (!table_name.empty() && table_name.table_name() != orig_table_name.table_name()) {
          meta.set_name(table_name.table_name());
          entry.set_data(meta.SerializeAsString());
        }

        if (!orig_table_name.has_namespace() || !orig_table_name.has_table()) {
          return STATUS(IllegalState,
                        "Could not find table name or keyspace name from snapshot metadata");
        }

        if (!table_name.empty()) {
          DCHECK(table_name.has_namespace());
          DCHECK(table_name.has_table());
          cout << "Target imported table name: " << table_name.ToString() << endl;
        }

        cout << "Table being imported: " << orig_table_name.ToString() << endl;
        ++table_index;
        orig_table_name = YBTableName();
        break;
      }
      default:
        break;
    }
  }

  RpcController rpc;
  rpc.set_timeout(timeout_);
  RETURN_NOT_OK(master_backup_proxy_->ImportSnapshotMeta(req, &resp, &rpc));

  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  const int kObjectColumnWidth = 16;
  const auto pad_object_type = [](const string& s) {
    return RightPadToWidth(s, kObjectColumnWidth);
  };

  cout << "Successfully applied snapshot." << endl
       << pad_object_type("Object") << kColumnSep
       << RightPadToUuidWidth("Old ID") << kColumnSep
       << RightPadToUuidWidth("New ID") << endl;

  const RepeatedPtrField<ImportSnapshotMetaResponsePB_TableMetaPB>& tables_meta =
      resp.tables_meta();
  IsCreateTableDoneRequestPB wait_req;
  IsCreateTableDoneResponsePB wait_resp;
  CreateSnapshotRequestPB snapshot_req;
  CreateSnapshotResponsePB snapshot_resp;

  for (int i = 0; i < tables_meta.size(); ++i) {
    const ImportSnapshotMetaResponsePB_TableMetaPB& table_meta = tables_meta.Get(i);
    const string& new_table_id = table_meta.table_ids().new_id();

    cout << pad_object_type("Keyspace") << kColumnSep
         << table_meta.namespace_ids().old_id() << kColumnSep
         << table_meta.namespace_ids().new_id() << endl;

    cout << pad_object_type("Table") << kColumnSep
         << table_meta.table_ids().old_id() << kColumnSep
         << new_table_id << endl;

    const RepeatedPtrField<IdPairPB>& tablets_map = table_meta.tablets_ids();
    for (int j = 0; j < tablets_map.size(); ++j) {
      const IdPairPB& pair = tablets_map.Get(j);
      cout << pad_object_type(Format("Tablet $0", j)) << kColumnSep
           << pair.old_id() << kColumnSep
           << pair.new_id() << endl;
    }

    // Wait for table creation.
    wait_req.mutable_table()->set_table_id(new_table_id);

    for (int k = 0; k < 20; ++k) {
      rpc.Reset();
      RETURN_NOT_OK(master_proxy_->IsCreateTableDone(wait_req, &wait_resp, &rpc));

      if (wait_resp.done()) {
        break;
      } else {
        cout << "Waiting for table " << new_table_id << "..." << endl;
        std::this_thread::sleep_for(1s);
      }
    }

    if (!wait_resp.done()) {
      return STATUS_FORMAT(
          TimedOut, "Table creation timeout: table id $0", new_table_id);
    }

    snapshot_req.mutable_tables()->Add()->set_table_id(new_table_id);
  }

  // Create new snapshot.
  rpc.Reset();
  RETURN_NOT_OK(master_backup_proxy_->CreateSnapshot(snapshot_req, &snapshot_resp, &rpc));
  cout << pad_object_type("Snapshot") << kColumnSep
       << snapshot_info->id() << kColumnSep
       << snapshot_resp.snapshot_id() << endl;

  return Status::OK();
}

Status ClusterAdminClient::ListReplicaTypeCounts(const YBTableName& table_name) {
  vector<string> tablet_ids, ranges;
  RETURN_NOT_OK(yb_client_->GetTablets(table_name, 0, &tablet_ids, &ranges));
  rpc::RpcController rpc;
  master::GetTabletLocationsRequestPB req;
  master::GetTabletLocationsResponsePB resp;
  rpc.set_timeout(timeout_);
  for (const auto& tablet_id : tablet_ids) {
    req.add_tablet_ids(tablet_id);
  }
  RETURN_NOT_OK(master_proxy_->GetTabletLocations(req, &resp, &rpc));
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  struct ReplicaCounts {
    int live_count;
    int read_only_count;
    string placement_uuid;
  };
  std::map<TabletServerId, ReplicaCounts> replica_map;

  std::cout << "Tserver ID\t\tPlacement ID\t\tLive count\t\tRead only count\n";

  for (int tablet_idx = 0; tablet_idx < resp.tablet_locations_size(); tablet_idx++) {
    const master::TabletLocationsPB& locs = resp.tablet_locations(tablet_idx);
    for (int replica_idx = 0; replica_idx < locs.replicas_size(); replica_idx++) {
      const auto& replica = locs.replicas(replica_idx);
      const string& ts_uuid = replica.ts_info().permanent_uuid();
      const string& placement_uuid =
          replica.ts_info().has_placement_uuid() ? replica.ts_info().placement_uuid() : "";
      bool is_replica_read_only =
          replica.member_type() == consensus::RaftPeerPB::PRE_OBSERVER ||
          replica.member_type() == consensus::RaftPeerPB::OBSERVER;
      int live_count = is_replica_read_only ? 0 : 1;
      int read_only_count = 1 - live_count;
      if (replica_map.count(ts_uuid) == 0) {
        replica_map[ts_uuid].live_count = live_count;
        replica_map[ts_uuid].read_only_count = read_only_count;
        replica_map[ts_uuid].placement_uuid = placement_uuid;
      } else {
        ReplicaCounts* counts = &replica_map[ts_uuid];
        counts->live_count += live_count;
        counts->read_only_count += read_only_count;
      }
    }
  }

  for (auto const& tserver : replica_map) {
    std::cout << tserver.first << "\t\t" << tserver.second.placement_uuid << "\t\t"
              << tserver.second.live_count << "\t\t" << tserver.second.read_only_count << std::endl;
  }

  return Status::OK();
}

Status ClusterAdminClient::SetPreferredZones(const std::vector<string>& preferred_zones) {
  rpc::RpcController rpc;
  master::SetPreferredZonesRequestPB req;
  master::SetPreferredZonesResponsePB resp;
  rpc.set_timeout(timeout_);

  std::set<string> zones;
  for (const string& zone : preferred_zones) {
    if (std::find(zones.begin(), zones.end(), zone) != zones.end()) {
      continue;
    }
    size_t last_pos = 0;
    size_t next_pos;
    std::vector<string> tokens;
    while ((next_pos = zone.find(".", last_pos)) != string::npos) {
      tokens.push_back(zone.substr(last_pos, next_pos - last_pos));
      last_pos = next_pos + 1;
    }
    tokens.push_back(zone.substr(last_pos, zone.size() - last_pos));
    if (tokens.size() != 3) {
      return STATUS_SUBSTITUTE(InvalidArgument, "Invalid argument for preferred zone $0, should "
          "have format cloud.region.zone", zone);
    }

    CloudInfoPB* cloud_info = req.add_preferred_zones();
    cloud_info->set_placement_cloud(tokens[0]);
    cloud_info->set_placement_region(tokens[1]);
    cloud_info->set_placement_zone(tokens[2]);

    zones.emplace(zone);
  }

  master_proxy_->SetPreferredZones(req, &resp, &rpc);

  if (resp.has_error()) {
    return STATUS(ServiceUnavailable, resp.error().status().message());
  }

  return Status::OK();
}



Status ClusterAdminClient::RotateUniverseKey(const std::string& key_path) {
  return SendEncryptionRequest(key_path, true);
}

Status ClusterAdminClient::DisableEncryption() {
  return SendEncryptionRequest("", false);
}

Status ClusterAdminClient::SendEncryptionRequest(
    const std::string& key_path, bool enable_encryption) {
  RETURN_NOT_OK_PREPEND(WaitUntilMasterLeaderReady(), "Wait for master leader failed!");
  rpc::RpcController rpc;
  rpc.set_timeout(timeout_);

  // Get the cluster config from the master leader.
  master::ChangeEncryptionInfoRequestPB encryption_info_req;
  master::ChangeEncryptionInfoResponsePB encryption_info_resp;
  encryption_info_req.set_encryption_enabled(enable_encryption);
  if (key_path != "") {
    encryption_info_req.set_key_path(key_path);
  }
  RETURN_NOT_OK_PREPEND(master_proxy_->
      ChangeEncryptionInfo(encryption_info_req, &encryption_info_resp, &rpc),
                        "MasterServiceImpl::ChangeEncryptionInfo call fails.")

  if (encryption_info_resp.has_error()) {
    return StatusFromPB(encryption_info_resp.error().status());
  }
  return Status::OK();
}

Status ClusterAdminClient::IsEncryptionEnabled() {
  RETURN_NOT_OK_PREPEND(WaitUntilMasterLeaderReady(), "Wait for master leader failed!");
  rpc::RpcController rpc;
  rpc.set_timeout(timeout_);

  master::IsEncryptionEnabledRequestPB req;
  master::IsEncryptionEnabledResponsePB resp;
  RETURN_NOT_OK_PREPEND(master_proxy_->
      IsEncryptionEnabled(req, &resp, &rpc),
      "MasterServiceImpl::IsEncryptionEnabled call fails.");
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  std::cout << "Encryption status: " << (resp.encryption_enabled() ?
      Format("ENABLED with key id $0", resp.key_id()) : "DISABLED" ) << std::endl;
  return Status::OK();
}

Status ClusterAdminClient::AddUniverseKeyToAllMasters(
    const std::string& key_id, const std::string& universe_key) {

  RETURN_NOT_OK(EncryptionParams::IsValidKeySize(
      universe_key.size() - EncryptionParams::kBlockSize));


  master::AddUniverseKeysRequestPB req;
  master::AddUniverseKeysResponsePB resp;
  auto* universe_keys = req.mutable_universe_keys();
  (*universe_keys->mutable_map())[key_id] = universe_key;

  for (auto hp : VERIFY_RESULT(HostPort::ParseStrings(master_addr_list_, 7100))) {
    rpc::RpcController rpc;
    rpc.set_timeout(timeout_);
    master::MasterServiceProxy proxy(proxy_cache_.get(), hp);
    RETURN_NOT_OK_PREPEND(proxy.AddUniverseKeys(req, &resp, &rpc),
                          Format("MasterServiceImpl::AddUniverseKeys call fails on host $0.",
                                 hp.ToString()));
    if (resp.has_error()) {
      return StatusFromPB(resp.error().status());
    }
    std::cout << Format("Successfully added key to the node $0.\n", hp.ToString());
  }

  return Status::OK();
}

Status ClusterAdminClient::AllMastersHaveUniverseKeyInMemory(const std::string& key_id) {
  master::HasUniverseKeyInMemoryRequestPB req;
  master::HasUniverseKeyInMemoryResponsePB resp;
  req.set_version_id(key_id);

  for (auto hp : VERIFY_RESULT(HostPort::ParseStrings(master_addr_list_, 7100))) {
    rpc::RpcController rpc;
    rpc.set_timeout(timeout_);
    master::MasterServiceProxy proxy(proxy_cache_.get(), hp);
    RETURN_NOT_OK_PREPEND(proxy.HasUniverseKeyInMemory(req, &resp, &rpc),
                          "MasterServiceImpl::ChangeEncryptionInfo call fails.");

    if (resp.has_error()) {
      return StatusFromPB(resp.error().status());
    }

    std::cout << Format("Node $0 has universe key in memory: $1\n", hp.ToString(), resp.has_key());
  }

  return Status::OK();
}

Status ClusterAdminClient::RotateUniverseKeyInMemory(const std::string& key_id) {
  RETURN_NOT_OK_PREPEND(WaitUntilMasterLeaderReady(), "Wait for master leader failed!");
  rpc::RpcController rpc;
  rpc.set_timeout(timeout_);

  master::ChangeEncryptionInfoRequestPB req;
  master::ChangeEncryptionInfoResponsePB resp;
  req.set_encryption_enabled(true);
  req.set_in_memory(true);
  req.set_version_id(key_id);
  RETURN_NOT_OK_PREPEND(master_proxy_->ChangeEncryptionInfo(req, &resp, &rpc),
                        "MasterServiceImpl::ChangeEncryptionInfo call fails.");
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  std::cout << "Rotated universe key in memory\n";
  return Status::OK();
}

Status ClusterAdminClient::DisableEncryptionInMemory() {
  RETURN_NOT_OK_PREPEND(WaitUntilMasterLeaderReady(), "Wait for master leader failed!");
  rpc::RpcController rpc;
  rpc.set_timeout(timeout_);

  master::ChangeEncryptionInfoRequestPB req;
  master::ChangeEncryptionInfoResponsePB resp;
  req.set_encryption_enabled(false);
  RETURN_NOT_OK_PREPEND(master_proxy_->ChangeEncryptionInfo(req, &resp, &rpc),
                        "MasterServiceImpl::ChangeEncryptionInfo call fails.");
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  std::cout << "Encryption disabled\n";
  return Status::OK();
}

Status ClusterAdminClient::WriteUniverseKeyToFile(
    const std::string& key_id, const std::string& file_name) {
  RETURN_NOT_OK_PREPEND(WaitUntilMasterLeaderReady(), "Wait for master leader failed!");
  rpc::RpcController rpc;
  rpc.set_timeout(timeout_);

  master::GetUniverseKeyRegistryRequestPB req;
  master::GetUniverseKeyRegistryResponsePB resp;
  RETURN_NOT_OK_PREPEND(master_proxy_->GetUniverseKeyRegistry(req, &resp, &rpc),
                        "MasterServiceImpl::ChangeEncryptionInfo call fails.");
  if (resp.has_error()) {
    return StatusFromPB(resp.error().status());
  }

  auto universe_keys = resp.universe_keys();
  const auto& it = universe_keys.map().find(key_id);
  if (it == universe_keys.map().end()) {
    return STATUS_FORMAT(NotFound, "Could not find key with id $0", key_id);
  }

  RETURN_NOT_OK(WriteStringToFile(Env::Default(), Slice(it->second), file_name));

  std::cout << "Finished writing to file\n";
  return Status::OK();
}

Status ClusterAdminClient::CreateCDCStream(const TableId& table_id) {
  master::CreateCDCStreamRequestPB req;
  master::CreateCDCStreamResponsePB resp;
  req.set_table_id(table_id);
  req.mutable_options()->Reserve(2);

  auto record_type_option = req.add_options();
  record_type_option->set_key(cdc::kRecordType);
  record_type_option->set_value(CDCRecordType_Name(cdc::CDCRecordType::CHANGE));

  auto record_format_option = req.add_options();
  record_format_option->set_key(cdc::kRecordFormat);
  record_format_option->set_value(CDCRecordFormat_Name(cdc::CDCRecordFormat::JSON));

  RpcController rpc;
  rpc.set_timeout(timeout_);
  master_proxy_->CreateCDCStream(req, &resp, &rpc);

  if (resp.has_error()) {
    cout << "Error creating stream: " << resp.error().status().message() << endl;
    return StatusFromPB(resp.error().status());
  }

  cout << "CDC Stream ID: " << resp.stream_id() << endl;
  return Status::OK();
}

Status ClusterAdminClient::DeleteCDCStream(const std::string& stream_id) {
  master::DeleteCDCStreamRequestPB req;
  master::DeleteCDCStreamResponsePB resp;
  req.add_stream_id(stream_id);

  RpcController rpc;
  rpc.set_timeout(timeout_);
  master_proxy_->DeleteCDCStream(req, &resp, &rpc);

  if (resp.has_error()) {
    cout << "Error deleting stream: " << resp.error().status().message() << endl;
    return StatusFromPB(resp.error().status());
  }

  cout << "Successfully deleted CDC Stream ID: " << stream_id << endl;
  return Status::OK();
}

Status ClusterAdminClient::ListCDCStreams(const TableId& table_id) {
  master::ListCDCStreamsRequestPB req;
  master::ListCDCStreamsResponsePB resp;
  if (!table_id.empty()) {
    req.set_table_id(table_id);
  }

  RpcController rpc;
  rpc.set_timeout(timeout_);
  master_proxy_->ListCDCStreams(req, &resp, &rpc);

  if (resp.has_error()) {
    cout << "Error getting CDC stream list: " << resp.error().status().message() << endl;
    return StatusFromPB(resp.error().status());
  }

  cout << "CDC Streams: \r\n" << resp.DebugString();
  return Status::OK();
}


Status ClusterAdminClient::SetupUniverseReplication(
    const string& producer_uuid, const vector<string>& producer_addresses,
    const vector<TableId>& tables,
    const vector<string>& producer_bootstrap_ids) {
  master::SetupUniverseReplicationRequestPB req;
  master::SetupUniverseReplicationResponsePB resp;
  req.set_producer_id(producer_uuid);

  req.mutable_producer_master_addresses()->Reserve(producer_addresses.size());
  for (const auto& addr : producer_addresses) {
    // HostPort::FromString() expects a default port.
    auto hp = VERIFY_RESULT(HostPort::FromString(addr, master::kMasterDefaultPort));
    HostPortToPB(hp, req.add_producer_master_addresses());
  }

  req.mutable_producer_table_ids()->Reserve(tables.size());
  for (const auto& table : tables) {
    req.add_producer_table_ids(table);
  }


  for (const auto& producer_bootstrap_id : producer_bootstrap_ids) {
    req.add_producer_bootstrap_ids(producer_bootstrap_id);
  }

  RpcController rpc;
  rpc.set_timeout(timeout_);
  master_proxy_->SetupUniverseReplication(req, &resp, &rpc);

  if (resp.has_error()) {
    cout << "Error setting up universe replication: " << resp.error().status().message() << endl;
    return StatusFromPB(resp.error().status());
  }

  cout << "Replication setup successfully" << endl;
  return Status::OK();
}

Status ClusterAdminClient::DeleteUniverseReplication(const std::string& producer_id) {
  master::DeleteUniverseReplicationRequestPB req;
  master::DeleteUniverseReplicationResponsePB resp;
  req.set_producer_id(producer_id);

  RpcController rpc;
  rpc.set_timeout(timeout_);
  master_proxy_->DeleteUniverseReplication(req, &resp, &rpc);

  if (resp.has_error()) {
    cout << "Error deleting universe replication: " << resp.error().status().message() << endl;
    return StatusFromPB(resp.error().status());
  }

  cout << "Replication deleted successfully" << endl;
  return Status::OK();
}

Status ClusterAdminClient::AlterUniverseReplication(const std::string& producer_uuid,
    const std::vector<std::string>& producer_addresses,
    const std::vector<TableId>& add_tables,
    const std::vector<TableId>& remove_tables) {
  master::AlterUniverseReplicationRequestPB req;
  master::AlterUniverseReplicationResponsePB resp;
  req.set_producer_id(producer_uuid);

  if (!producer_addresses.empty()) {
    req.mutable_producer_master_addresses()->Reserve(producer_addresses.size());
    for (const auto& addr : producer_addresses) {
      // HostPort::FromString() expects a default port.
      auto hp = VERIFY_RESULT(HostPort::FromString(addr, master::kMasterDefaultPort));
      HostPortToPB(hp, req.add_producer_master_addresses());
    }
  }

  if (!add_tables.empty()) {
    req.mutable_producer_table_ids_to_add()->Reserve(add_tables.size());
    for (const auto& table : add_tables) {
      req.add_producer_table_ids_to_add(table);
    }
  }

  if (!remove_tables.empty()) {
    req.mutable_producer_table_ids_to_remove()->Reserve(remove_tables.size());
    for (const auto& table : remove_tables) {
      req.add_producer_table_ids_to_remove(table);
    }
  }

  RpcController rpc;
  rpc.set_timeout(timeout_);
  master_proxy_->AlterUniverseReplication(req, &resp, &rpc);

  if (resp.has_error()) {
    cout << "Error altering universe replication: " << resp.error().status().message() << endl;
    return StatusFromPB(resp.error().status());
  }

  cout << "Replication altered successfully" << endl;
  return Status::OK();
}

CHECKED_STATUS ClusterAdminClient::SetUniverseReplicationEnabled(const std::string& producer_id,
                                                                 bool is_enabled) {
  master::SetUniverseReplicationEnabledRequestPB req;
  master::SetUniverseReplicationEnabledResponsePB resp;
  req.set_producer_id(producer_id);
  req.set_is_enabled(is_enabled);
  const string toggle = (is_enabled ? "enabl" : "disabl");

  RpcController rpc;
  rpc.set_timeout(timeout_);
  master_proxy_->SetUniverseReplicationEnabled(req, &resp, &rpc);

  if (resp.has_error()) {
    cout << "Error " << toggle << "ing "
         << "universe replication: " << resp.error().status().message() << endl;
    return StatusFromPB(resp.error().status());
  }

  cout << "Replication " << toggle << "ed successfully" << endl;
  return Status::OK();
}

Result<HostPort> ClusterAdminClient::GetFirstRpcAddressForTS() {
  RepeatedPtrField<ListTabletServersResponsePB::Entry> servers;
  RETURN_NOT_OK(ListTabletServers(&servers));
  for (const ListTabletServersResponsePB::Entry& server : servers) {
    if (server.has_registration() &&
        !server.registration().common().private_rpc_addresses().empty()) {
      return HostPortFromPB(server.registration().common().private_rpc_addresses(0));
    }
  }

  return STATUS(NotFound, "Didn't find a server registered with the Master");
}

Status ClusterAdminClient::BootstrapProducer(const vector<TableId>& table_ids) {

  HostPort ts_addr = VERIFY_RESULT(GetFirstRpcAddressForTS());
  auto cdc_proxy = std::make_unique<cdc::CDCServiceProxy>(proxy_cache_.get(), ts_addr);

  cdc::BootstrapProducerRequestPB bootstrap_req;
  cdc::BootstrapProducerResponsePB bootstrap_resp;
  for (const auto& table_id : table_ids) {
    bootstrap_req.add_table_ids(table_id);
  }
  RpcController rpc;
  rpc.set_timeout(MonoDelta::FromSeconds(std::max(timeout_.ToSeconds(), 120.0)));
  cdc_proxy->BootstrapProducer(bootstrap_req, &bootstrap_resp, &rpc);

  if (bootstrap_resp.has_error()) {
    cout << "Error bootstrapping consumer: " << bootstrap_resp.error().status().message() << endl;
    return StatusFromPB(bootstrap_resp.error().status());
  }

  if (bootstrap_resp.cdc_bootstrap_ids().size() != table_ids.size()) {
    cout << "Received invalid number of bootstrap ids: " << bootstrap_resp.ShortDebugString();
    return STATUS(InternalError, "Invalid number of bootstrap ids");
  }

  int i = 0;
  for (const auto& bootstrap_id : bootstrap_resp.cdc_bootstrap_ids()) {
    cout << "table id: " << table_ids[i++] << ", CDC bootstrap id: " << bootstrap_id << endl;
  }
  return Status::OK();
}

}  // namespace enterprise
}  // namespace tools
}  // namespace yb
