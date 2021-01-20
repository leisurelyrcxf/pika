// Copyright (c) 2015-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#include "set"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <glog/logging.h>
#include <fstream>

#include "pink/include/pink_cli.h"

#include "include/pika_rm.h"
#include "include/pika_conf.h"
#include "include/pika_server.h"
#include "include/pika_repl_client.h"
#include "include/pika_repl_server.h"


extern PikaConf *g_pika_conf;
extern PikaReplicaManager* g_pika_rm;
extern PikaServer *g_pika_server;

/* BinlogReaderManager */

BinlogReaderManager::~BinlogReaderManager() {
}

Status BinlogReaderManager::FetchBinlogReader(const RmNode& rm_node, std::shared_ptr<PikaBinlogReader>* reader) {
  slash::MutexLock l(&reader_mu_);
  if (occupied_.find(rm_node) != occupied_.end()) {
    return Status::Corruption(rm_node.ToString() + " exist");
  }
  if (vacant_.empty()) {
    *reader = std::make_shared<PikaBinlogReader>();
  } else {
    *reader = *(vacant_.begin());
    vacant_.erase(vacant_.begin());
  }
  occupied_[rm_node] = *reader;
  return Status::OK();
}

Status BinlogReaderManager::ReleaseBinlogReader(const RmNode& rm_node) {
  slash::MutexLock l(&reader_mu_);
  if (occupied_.find(rm_node) == occupied_.end()) {
    return Status::NotFound(rm_node.ToString());
  }
  std::shared_ptr<PikaBinlogReader> reader = occupied_[rm_node];
  occupied_.erase(rm_node);
  vacant_.push_back(reader);
  return Status::OK();
}

/* SlaveNode */

SlaveNode::SlaveNode(const std::string& ip, int port,
                     const std::string& table_name,
                     uint32_t partition_id, int session_id, uint32_t master_term)
  : RmNode(ip, port, table_name, partition_id, session_id),
  slave_state(kSlaveNotSync),
  b_state(kNotSync), sent_offset(), acked_offset(), master_term_(master_term) {
}

SlaveNode::~SlaveNode() {
  if (b_state == kReadFromFile && binlog_reader != nullptr) {
    RmNode rm_node(Ip(), Port(), TableName(), PartitionId());
    ReleaseBinlogFileReader();
  }
}

Status SlaveNode::InitBinlogFileReader(const std::shared_ptr<Binlog>& binlog,
                                       const BinlogOffset& offset) {
  Status s = g_pika_rm->binlog_reader_mgr.FetchBinlogReader(
      RmNode(Ip(), Port(), NodePartitionInfo()), &binlog_reader);
  if (!s.ok()) {
    return s;
  }
  int res = binlog_reader->Seek(binlog, offset.filenum, offset.offset);
  if (res) {
    g_pika_rm->binlog_reader_mgr.ReleaseBinlogReader(
        RmNode(Ip(), Port(), NodePartitionInfo()));
    return Status::Corruption(ToString() + "  binlog reader init failed");
  }
  return Status::OK();
}

void SlaveNode::ReleaseBinlogFileReader() {
  g_pika_rm->binlog_reader_mgr.ReleaseBinlogReader(
      RmNode(Ip(), Port(), NodePartitionInfo()));
  binlog_reader = nullptr;
}

std::string SlaveNode::ToStringStatus() {
  std::stringstream tmp_stream;
  tmp_stream << "    Slave_state: " << SlaveStateMsg[slave_state] << "\r\n";
  tmp_stream << "    Binlog_sync_state: " << BinlogSyncStateMsg[b_state] << "\r\n";
  tmp_stream << "    Sync_window: " << "\r\n" << sync_win.ToStringStatus();
  tmp_stream << "    Sent_offset: " << sent_offset.ToString() << "\r\n";
  tmp_stream << "    Acked_offset: " << acked_offset.ToString() << "\r\n";
  tmp_stream << "    Binlog_reader activated: " << (binlog_reader != nullptr) << "\r\n";
  return tmp_stream.str();
}

/* SyncPartition */

SyncPartition::SyncPartition(const std::string& table_name, uint32_t partition_id)
  : partition_info_(table_name, partition_id) {
}

/* SyncMasterPartition*/

SyncMasterPartition::SyncMasterPartition(const std::string& table_name, uint32_t partition_id)
    : SyncPartition(table_name, partition_id),
      session_id_(0) {}

bool SyncMasterPartition::CheckReadBinlogFromCache() {
  return false;
}

int SyncMasterPartition::GetNumberOfSlaveNode() {
  slash::MutexLock l(&partition_mu_);
  return slaves_.size();
}

bool SyncMasterPartition::CheckSlaveNodeExist(const std::string& ip, int port) {
  slash::MutexLock l(&partition_mu_);
  for (auto& slave : slaves_) {
    if (ip == slave->Ip() && port == slave->Port()) {
      return true;
    }
  }
  return false;
}

Status SyncMasterPartition::GetSlaveNodeSession(
    const std::string& ip, int port, int32_t* session) {
  slash::MutexLock l(&partition_mu_);
  for (auto& slave : slaves_) {
    if (ip == slave->Ip() && port == slave->Port()) {
      *session = slave->SessionId();
      return Status::OK();
    }
  }
  return Status::NotFound("slave " + ip + ":" + std::to_string(port) + " not found");
}

// In resharding mode, partition_id may be different from SyncMasterPartition's partition_id
Status SyncMasterPartition::AddSlaveNode(const std::string& ip, int port, uint32_t partition_id, int session_id, uint32_t master_term) {
  slash::MutexLock l(&partition_mu_);
  for (auto& slave : slaves_) {
    if (ip == slave->Ip() && port == slave->Port()) {
      if (partition_id == slave->PartitionId()) {
        slave->SetSessionId(session_id);
        return Status::OK();
      }
      std::stringstream ss;
      ss << "multi partitions from same slave pika, wanna add " << partition_id << " but already exist " << slave->PartitionId();
      return Status::Corruption(ss.str());
    }
  }
  std::shared_ptr<SlaveNode> slave_ptr =
    std::make_shared<SlaveNode>(ip, port, SyncPartitionInfo().table_name_, partition_id, session_id, master_term);
  slave_ptr->SetLastSendTime(slash::NowMicros());
  slave_ptr->SetLastRecvTime(slash::NowMicros());
  slaves_.push_back(slave_ptr);
  LOG(INFO) << "Add Slave Node, partition: " << slave_ptr->NodePartitionInfo().ToString() << ", ip_port: "<< ip << ":" << port;
  return Status::OK();
}

Status SyncMasterPartition::RemoveSlaveNode(const std::string& ip, int port) {
  slash::MutexLock l(&partition_mu_);
  for (size_t i = 0; i < slaves_.size(); ++i) {
    std::shared_ptr<SlaveNode> slave = slaves_[i];
    if (ip == slave->Ip() && port == slave->Port()) {
      slaves_.erase(slaves_.begin() + i);
      LOG(INFO) << "Remove Slave Node, Partition: " <<  slave->NodePartitionInfo().ToString()
        << ", ip_port: "<< ip << ":" << port;
      return Status::OK();
    }
  }
  return Status::NotFound("RemoveSlaveNode" + ip + std::to_string(port));
}

Status SyncMasterPartition::ActivateSlaveBinlogSync(const std::string& ip,
                                                    int port,
                                                    const std::shared_ptr<Binlog> binlog,
                                                    const BinlogOffset& offset) {
  {
  slash::MutexLock l(&partition_mu_);
  std::shared_ptr<SlaveNode> slave_ptr = nullptr;
  Status s = GetSlaveNode(ip, port, &slave_ptr);
  if (!s.ok()) {
    return s;
  }
  bool read_cache = CheckReadBinlogFromCache();

  slave_ptr->Lock();
  slave_ptr->slave_state = kSlaveBinlogSync;
  slave_ptr->sent_offset = offset;
  slave_ptr->acked_offset = offset;
  if (read_cache) {
    slave_ptr->Unlock();
    // RegistToBinlogCacheWindow(ip, port, offset);
    slave_ptr->Lock();
    slave_ptr->b_state = kReadFromCache;
  } else {
    // read binlog file from file
    s = slave_ptr->InitBinlogFileReader(binlog, offset);
    if (!s.ok()) {
      slave_ptr->Unlock();
      return Status::Corruption("Init binlog file reader failed" + s.ToString());
    }
    slave_ptr->b_state = kReadFromFile;
  }
  slave_ptr->Unlock();
  }

  Status s = SyncBinlogToWq(ip, port);
  if (!s.ok()) {
    return s;
  }
  return Status::OK();
}

Status SyncMasterPartition::SyncBinlogToWq(const std::string& ip, int port) {
  slash::MutexLock l(&partition_mu_);
  std::shared_ptr<SlaveNode> slave_ptr = nullptr;
  Status s = GetSlaveNode(ip, port, &slave_ptr);
  if (!s.ok()) {
    return s;
  }

  {
  slash::MutexLock l(&slave_ptr->slave_mu);
  if (slave_ptr->b_state == kReadFromFile) {
    ReadBinlogFileToWq(slave_ptr);
  } else if (slave_ptr->b_state == kReadFromCache) {
    ReadCachedBinlogToWq(slave_ptr);
  }
  }
  return Status::OK();
}

Status SyncMasterPartition::ActivateSlaveDbSync(const std::string& ip, int port) {
  slash::MutexLock l(&partition_mu_);
  std::shared_ptr<SlaveNode> slave_ptr = nullptr;
  Status s = GetSlaveNode(ip, port, &slave_ptr);
  if (!s.ok()) {
    return s;
  }

  {
  slash::MutexLock l(&slave_ptr->slave_mu);
  slave_ptr->slave_state = kSlaveDbSync;
  // invoke db sync
  }
  return Status::OK();
}

Status SyncMasterPartition::ReadCachedBinlogToWq(const std::shared_ptr<SlaveNode>& slave_ptr) {
  return Status::OK();
}

Status SyncMasterPartition::ReadBinlogFileToWq(const std::shared_ptr<SlaveNode>& slave_ptr) {
  int cnt = slave_ptr->sync_win.Remainings();
  std::shared_ptr<PikaBinlogReader> reader = slave_ptr->binlog_reader;
  std::vector<WriteTask> tasks;
  for (int i = 0; i < cnt; ++i) {
    std::string msg;
    uint32_t filenum;
    uint64_t offset;
    Status s = reader->Get(&msg, &filenum, &offset);
    if (s.IsEndFile()) {
      break;
    } else if (s.IsCorruption() || s.IsIOError()) {
      LOG(WARNING) << SyncPartitionInfo().ToString()
        << " Read Binlog error : " << s.ToString();
      return s;
    }
    slave_ptr->sync_win.Push(SyncWinItem(filenum, offset));

    BinlogOffset sent_offset = BinlogOffset(filenum, offset);
    slave_ptr->sent_offset = sent_offset;
    slave_ptr->SetLastSendTime(slash::NowMicros());
    RmNode rm_node(slave_ptr->Ip(), slave_ptr->Port(), slave_ptr->TableName(), slave_ptr->PartitionId(), slave_ptr->SessionId());
    WriteTask task(rm_node, slave_ptr->master_term_, BinlogChip(sent_offset, msg));
    tasks.push_back(task);
  }

  if (!tasks.empty()) {
    g_pika_rm->ProduceWriteQueue(slave_ptr->Ip(), slave_ptr->Port(), tasks);
  }
  return Status::OK();
}

Status SyncMasterPartition::GetSlaveNode(const std::string& ip, int port, std::shared_ptr<SlaveNode>* slave_node) {
  for (size_t i  = 0; i < slaves_.size(); ++i) {
    std::shared_ptr<SlaveNode> tmp_slave = slaves_[i];
    if (ip == tmp_slave->Ip() && port == tmp_slave->Port()) {
      *slave_node = tmp_slave;
      return Status::OK();
    }
  }
  return Status::NotFound("ip " + ip  + " port " + std::to_string(port));
}

Status SyncMasterPartition::UpdateSlaveBinlogAckInfo(const std::string& ip, int port, const BinlogOffset& start, const BinlogOffset& end) {
  slash::MutexLock l(&partition_mu_);
  std::shared_ptr<SlaveNode> slave_ptr = nullptr;
  Status s = GetSlaveNode(ip, port, &slave_ptr);
  if (!s.ok()) {
    return s;
  }

  {
  slash::MutexLock l(&slave_ptr->slave_mu);
  if (slave_ptr->slave_state != kSlaveBinlogSync) {
    return Status::Corruption(ip + std::to_string(port) + "state not BinlogSync");
  }
  bool res = slave_ptr->sync_win.Update(SyncWinItem(start), SyncWinItem(end), &(slave_ptr->acked_offset));
  if (!res) {
    return Status::Corruption("UpdateAckedInfo failed");
  }
  }
  return Status::OK();
}

Status SyncMasterPartition::GetSlaveSyncBinlogInfo(const std::string& ip,
                                                   int port,
                                                   BinlogOffset* sent_offset,
                                                   BinlogOffset* acked_offset) {
  slash::MutexLock l(&partition_mu_);
  std::shared_ptr<SlaveNode> slave_ptr = nullptr;
  Status s = GetSlaveNode(ip, port, &slave_ptr);
  if (!s.ok()) {
    return s;
  }

  {
  slash::MutexLock l(&slave_ptr->slave_mu);
  *sent_offset = slave_ptr->sent_offset;
  *acked_offset = slave_ptr->acked_offset;
  }
  return Status::OK();
}

Status SyncMasterPartition::GetSlaveState(const std::string& ip,
                                          int port,
                                          SlaveState* const slave_state) {
  slash::MutexLock l(&partition_mu_);
  std::shared_ptr<SlaveNode> slave_ptr = nullptr;
  Status s = GetSlaveNode(ip, port, &slave_ptr);
  if (!s.ok()) {
    return s;
  }

  {
  slash::MutexLock l(&slave_ptr->slave_mu);
  *slave_state = slave_ptr->slave_state;
  }
  return Status::OK();
}

Status SyncMasterPartition::WakeUpSlaveBinlogSync() {
  slash::MutexLock l(&partition_mu_);
  for (auto& slave_ptr : slaves_) {
    {
    slash::MutexLock l(&slave_ptr->slave_mu);
    if (slave_ptr->sent_offset == slave_ptr->acked_offset) {
      if (slave_ptr->b_state == kReadFromFile) {
        ReadBinlogFileToWq(slave_ptr);
      } else if (slave_ptr->b_state == kReadFromCache) {
        ReadCachedBinlogToWq(slave_ptr);
      }
    }
    }
  }
  return Status::OK();
}

Status SyncMasterPartition::SetLastSendTime(const std::string& ip, int port, uint64_t time) {
  slash::MutexLock l(&partition_mu_);

  std::shared_ptr<SlaveNode> slave_ptr = nullptr;
  Status s = GetSlaveNode(ip, port, &slave_ptr);
  if (!s.ok()) {
    return s;
  }

  {
  slash::MutexLock l(&slave_ptr->slave_mu);
  slave_ptr->SetLastSendTime(time);
  }

  return Status::OK();
}

Status SyncMasterPartition::GetLastSendTime(const std::string& ip, int port, uint64_t* time) {
  slash::MutexLock l(&partition_mu_);

  std::shared_ptr<SlaveNode> slave_ptr = nullptr;
  Status s = GetSlaveNode(ip, port, &slave_ptr);
  if (!s.ok()) {
    return s;
  }

  {
  slash::MutexLock l(&slave_ptr->slave_mu);
  *time = slave_ptr->LastSendTime();
  }

  return Status::OK();
}

Status SyncMasterPartition::SetLastRecvTime(const std::string& ip, int port, uint64_t time) {
  slash::MutexLock l(&partition_mu_);

  std::shared_ptr<SlaveNode> slave_ptr = nullptr;
  Status s = GetSlaveNode(ip, port, &slave_ptr);
  if (!s.ok()) {
    return s;
  }

  {
  slash::MutexLock l(&slave_ptr->slave_mu);
  slave_ptr->SetLastRecvTime(time);
  }

  return Status::OK();
}

Status SyncMasterPartition::GetLastRecvTime(const std::string& ip, int port, uint64_t* time) {
  slash::MutexLock l(&partition_mu_);

  std::shared_ptr<SlaveNode> slave_ptr = nullptr;
  Status s = GetSlaveNode(ip, port, &slave_ptr);
  if (!s.ok()) {
    return s;
  }

  {
  slash::MutexLock l(&slave_ptr->slave_mu);
  *time = slave_ptr->LastRecvTime();
  }

  return Status::OK();
}

Status SyncMasterPartition::GetSafetyPurgeBinlog(std::string* safety_purge) {
  BinlogOffset boffset;
  std::string table_name = partition_info_.table_name_;
  uint32_t partition_id = partition_info_.partition_id_;
  std::shared_ptr<Partition> partition =
      g_pika_server->GetTablePartitionById(table_name, partition_id);
  if (!partition || !partition->GetBinlogOffset(&boffset)) {
    return Status::NotFound("Partition NotFound");
  } else {
    bool success = false;
    uint32_t purge_max = boffset.filenum;
    if (purge_max >= 10) {
      success = true;
      purge_max -= 10;
      slash::MutexLock l(&partition_mu_);
      for (const auto& slave : slaves_) {
        if (slave->slave_state == SlaveState::kSlaveBinlogSync
          && slave->acked_offset.filenum > 0) {
          purge_max = std::min(slave->acked_offset.filenum - 1, purge_max);
        } else {
          success = false;
          break;
        }
      }
    }
    *safety_purge = (success ? kBinlogPrefix + std::to_string(static_cast<int32_t>(purge_max)) : "none");
  }
  return Status::OK();
}

bool SyncMasterPartition::BinlogCloudPurge(uint32_t index) {
  BinlogOffset boffset;
  std::string table_name = partition_info_.table_name_;
  uint32_t partition_id = partition_info_.partition_id_;
  std::shared_ptr<Partition> partition =
      g_pika_server->GetTablePartitionById(table_name, partition_id);
  if (!partition || !partition->GetBinlogOffset(&boffset)) {
    return false;
  } else {
    if (index > boffset.filenum - 10) {  // remain some more
      return false;
    } else {
      slash::MutexLock l(&partition_mu_);
      for (const auto& slave : slaves_) {
        if (slave->slave_state == SlaveState::kSlaveDbSync) {
          return false;
        } else if (slave->slave_state == SlaveState::kSlaveBinlogSync) {
          if (index >= slave->acked_offset.filenum) {
            return false;
          }
        }
      }
    }
  }
  return true;
}

Status SyncMasterPartition::CheckSyncTimeout(uint64_t now) {
  slash::MutexLock pl(&partition_mu_);

  std::vector<Node> to_del;

  for (auto& slave_ptr : slaves_) {
    slash::MutexLock l(&slave_ptr->slave_mu);
    if (slave_ptr->LastRecvTime() + kRecvKeepAliveTimeout < now) {
      to_del.push_back(Node(slave_ptr->Ip(), slave_ptr->Port()));
    } else if (slave_ptr->LastSendTime() + kSendKeepAliveTimeout < now) {
      std::vector<WriteTask> task;
      RmNode rm_node(slave_ptr->Ip(), slave_ptr->Port(), slave_ptr->TableName(), slave_ptr->PartitionId(), slave_ptr->SessionId());
      WriteTask empty_task(rm_node, slave_ptr->master_term_, BinlogChip(BinlogOffset(0, 0), ""));
      task.push_back(empty_task);
      Status s = g_pika_rm->SendSlaveBinlogChipsRequest(slave_ptr->Ip(), slave_ptr->Port(), task);
      slave_ptr->SetLastSendTime(now);
      if (!s.ok()) {
        LOG(INFO)<< "Send ping failed: " << s.ToString();
        return Status::Corruption("Send ping failed: " + slave_ptr->Ip() + ":" + std::to_string(slave_ptr->Port()));
      }
    }
  }
  for (auto& node : to_del) {
    for (size_t i = 0; i < slaves_.size(); ++i) {
      auto slave = slaves_[i];
      if (node.Ip() == slave->Ip() && node.Port() == slave->Port()) {
        slaves_.erase(slaves_.begin() + i);
        LOG(WARNING) << slave->NodePartitionInfo().ToString() << " Master del Recv Timeout slave success " << node.ToString();
        break;
      }
    }
  }
  return Status::OK();
}

std::string SyncMasterPartition::ToStringStatus() {
  std::stringstream tmp_stream;
  tmp_stream << " Current Master Session: " << session_id_ << "\r\n";
  slash::MutexLock l(&partition_mu_);
  for (size_t i = 0; i < slaves_.size(); ++i) {
    std::shared_ptr<SlaveNode> slave_ptr = slaves_[i];
    slash::MutexLock l(&slave_ptr->slave_mu);
    tmp_stream << "  slave[" << i << "]: "  << slave_ptr->ToString() <<
      "\r\n" << slave_ptr->ToStringStatus();
  }
  return tmp_stream.str();
}

void SyncMasterPartition::GetValidSlaveNames(std::vector<std::string>* slavenames) {
  slash::MutexLock l(&partition_mu_);
  for (auto ptr : slaves_) {
    if (ptr->slave_state != kSlaveBinlogSync) {
      continue;
    }
    std::string name = ptr->Ip() + ":" + std::to_string(ptr->Port());
    slavenames->push_back(name);
  }
}

Status SyncMasterPartition::GetInfo(std::string* info) {
  std::stringstream tmp_stream;
  slash::MutexLock l(&partition_mu_);
  tmp_stream << "  Role: Master" << "\r\n";
  tmp_stream << "  connected_slaves: " << slaves_.size() << "\r\n";
  for (size_t i = 0; i < slaves_.size(); ++i) {
    std::shared_ptr<SlaveNode> slave_ptr = slaves_[i];
    slash::MutexLock l(&slave_ptr->slave_mu);
    tmp_stream << "  slave[" << i << "]: "
      << slave_ptr->Ip()  << ":" << std::to_string(slave_ptr->Port()) << "\r\n";
    tmp_stream << " partition_id: " << slave_ptr->PartitionId() << "\r\n";
    tmp_stream << "  replication_status: " << SlaveStateMsg[slave_ptr->slave_state] << "\r\n";
    if (slave_ptr->slave_state == kSlaveBinlogSync) {
      std::shared_ptr<Partition> partition = g_pika_server->GetTablePartitionById(slave_ptr->TableName(), slave_ptr->PartitionId());
      BinlogOffset binlog_offset;
      if (!partition || !partition->GetBinlogOffset(&binlog_offset)) {
        return Status::Corruption("Get Info failed.");
      }
      uint64_t lag = (binlog_offset.filenum - slave_ptr->acked_offset.filenum) *
        g_pika_conf->binlog_file_size()
        + (binlog_offset.offset - slave_ptr->acked_offset.offset);
      tmp_stream << "  lag: " << lag << "\r\n";
    }
  }
  info->append(tmp_stream.str());
  return Status::OK();
}

int32_t SyncMasterPartition::GenSessionId() {
  slash::MutexLock ml(&session_mu_);
  return session_id_++;
}

bool SyncMasterPartition::CheckSessionId(const std::string& ip, int port,
                                         const std::string& table_name,
                                         uint64_t partition_id, int session_id) {
  slash::MutexLock l(&partition_mu_);
  std::shared_ptr<SlaveNode> slave_ptr = nullptr;
  Status s = GetSlaveNode(ip, port, &slave_ptr);
  if (!s.ok()) {
    LOG(WARNING)<< "Check SessionId Get Slave Node Error: "
        << ip << ":" << port << "," << table_name << "_" << partition_id;
    return false;
  }
  if (session_id != slave_ptr->SessionId()) {
    LOG(WARNING)<< "Check SessionId Mismatch: " << ip << ":" << port << ", "
        << table_name << "_" << partition_id << " expected_session: " << session_id
        << ", actual_session:" << slave_ptr->SessionId();
    return false;
  }
  return true;
}

/* SyncSlavePartition */

const std::vector<ReplState> SyncSlavePartition::NEEDS_CHECK_SYNC_TIMEOUT_STATES{ReplState::kWaitDBSync, ReplState::kWaitReply, ReplState::kConnected};

SyncSlavePartition::SyncSlavePartition(const std::string& table_name,
                                       uint32_t partition_id)
  : SyncPartition(table_name, partition_id),
    m_info_(),
    m_term_(0),
    repl_state_(kNoConnect),
    local_ip_(""),
    resharding_(false) {
  m_info_.SetLastRecvTime(slash::NowMicros());
  pthread_rwlock_init(&partition_mu_, NULL);
}

Status SyncSlavePartition::InitMasterTerm() {
  std::string table_name = partition_info_.table_name_;
  uint32_t partition_id = partition_info_.partition_id_;
  std::shared_ptr<Partition> partition = g_pika_server->GetTablePartitionById(table_name, partition_id);
  if (!partition) {
    return Status::Corruption("can't find table partition " + table_name + ":" + std::to_string(partition_id));
  }
  uint32_t master_term;
  Status s = partition->GetMasterTerm(&master_term);
  if (!s.ok()) {
    return Status::Corruption("can't get largest term for partition" + table_name + ":" + std::to_string(partition_id) + ", error: " + s.ToString());
  }

  slash::RWLock l(&partition_mu_, true);
  m_term_ = master_term;
  LOG(INFO) << "Initialize master term of slave partition " << table_name << ":" << std::to_string(partition_id) << " to " << m_term_;
  return Status::OK();
}

void SyncSlavePartition::SetReplState(const ReplState& repl_state) {
  slash::RWLock l(&partition_mu_, true);
  SetReplStateUnsafe(repl_state);
}

void SyncSlavePartition::SetReplStateUnsafe(const ReplState& repl_state) {
  if (repl_state == ReplState::kNoConnect) {
    // deactivate
    (void) SetMasterUnsafe(RmNode(), "", "state reset to ReplState::kNoConnect");
    repl_state_ = ReplState::kNoConnect;
    return;
  }
  repl_state_ = repl_state;
}

Status SyncSlavePartition::CASReplState(const ReplState& exp_state,
                                        const uint32_t exp_master_term,
                                        const ReplState& new_state,
                                        const std::string& reason) {
 return CASReplState(std::vector<ReplState>{exp_state},
                     exp_master_term,
                     []()->Status{ return Status::OK(); },
                     new_state,
                     reason);
}

Status SyncSlavePartition::CASReplState(const std::vector<ReplState>& allowed_states,
                                        uint32_t exp_master_term,
                                        const std::function<Status()>& action,
                                        const ReplState& new_state,
                                        const std::string& reason) {
  slash::RWLock l(&partition_mu_, true);
  if (!matchStates(allowed_states)) {
    auto err = CASStateCheckFailed(allowed_states, new_state);
    LOG(WARNING) << err.ToString().data() << ", cas reason: " << reason;
    return err;
  }
  // Avoid ABA problem
  if (exp_master_term != m_term_) {
    auto err = CASTermCheckFailed(exp_master_term, allowed_states, new_state);
    LOG(WARNING) << err.ToString().data() << ", cas reason: " << reason;
    return err;
  }
  Status ret = action();
  if (ret.ok()) {
    if (new_state != ReplState::kError && new_state != ReplState::kTryConnect && new_state != ReplState::kNoConnect && new_state != ReplState::kDBNoConnect) {
      LOG(INFO) << "CAS partition " << this->partition_info_.ToString() << " "
                << "state from '" << ReplStateMsg[repl_state_] << "' term " << exp_master_term << " "
                << "to '" << ReplStateMsg[new_state] << "' successfully" << ", cas reason: " << reason;
    } else {
      LOG(WARNING) << "CAS partition " << this->partition_info_.ToString() << " "
                   << "state from '" << ReplStateMsg[repl_state_] << "' term " << exp_master_term << " "
                   << "to '" << ReplStateMsg[new_state] << "' successfully" << ", cas reason: " << reason;
    }
    SetReplStateUnsafe(new_state);
    if (NeedsCheckSyncTimeout(repl_state_)) {
      m_info_.SetLastRecvTime(slash::NowMicros());
    }
  }
  return ret;
}

bool SyncSlavePartition::matchStates(const std::vector<ReplState>& allowed_current_states) {
  for (auto& allowed_state : allowed_current_states) {
    if (allowed_state == repl_state_) {
      return true;
    }
  }
  return false;
}

Status SyncSlavePartition::ResetMasterUnsafe(const std::string& info_file_path, const std::string& reason) {
  auto old = this->m_info_;
//  (void) this->SetMasterUnsafe(RmNode(), info_file_path);
  old.SetSessionId(0);
  return this->SetMasterUnsafe(old, info_file_path, reason);
}

Status SyncSlavePartition::SetMasterUnsafe(const RmNode& newMaster, const std::string& info_file_path, const std::string& reason) {
  auto old_master = m_info_;

  if (newMaster.Ip().empty()) {
    m_info_ = newMaster;
    if (!(old_master==newMaster)) {
      LOG(INFO) << "Change master of partition " << partition_info_.table_name_ << ":" << partition_info_.partition_id_ << " "
                << "from '" << old_master.GetAddr() << "' "
                << "to '" << m_info_.GetAddr() << "', "
                << "Unchanged master term: " << m_term_ << ", "
                << "Set Master Reason: " << reason;
    }
    return Status::OK();
  }

  size_t last_slash_index = info_file_path.rfind('/');
  if (last_slash_index == std::string::npos) {
    std::stringstream ss;
    ss << "can't find '/' in '" << info_file_path << "', Set Master Reason: " << reason;
    return Status::Corruption(ss.str());
  }
  std::string info_file_dir = info_file_path.substr(0, last_slash_index);
  if (!slash::FileExists(info_file_dir)) {
    if (slash::CreatePath(info_file_dir)) {
      std::stringstream ss;
      ss << "can't create info file dir '" << info_file_dir << "', Set Master Reason: " << reason;
      return Status::Corruption(ss.str());
    }
  }

  std::ofstream fix;
  fix.open(info_file_path, std::ios::in | std::ios::trunc);
  if (!fix.is_open()) {
    std::stringstream ss;
    ss << "can't open term info file '" << info_file_path << "', Set Master Reason: " << reason;
    return Status::Corruption(ss.str());
  }
  m_info_ = newMaster;
  m_info_.SetLastRecvTime(slash::NowMicros());
  fix << ++m_term_;
  fix.close();
  LOG(INFO) << "Change master of partition " << partition_info_.table_name_ << ":" << partition_info_.partition_id_
            << " from '" << old_master.GetAddr() << "', "
            << " to '" << m_info_.GetAddr() << "'. "
            << "New master term: " << m_term_  << ", Set Master Reason: " << reason;
  return Status::OK();
}

Status SyncSlavePartition::CheckSyncTimeout(uint64_t now) {
  uint32_t master_term;
  {
    slash::RWLock l(&partition_mu_, false);
    // no need to do session keepalive return ok
    if (!NeedsCheckSyncTimeout(repl_state_)) {
      return Status::OK();
    }
    if (m_info_.LastRecvTime() + kRecvKeepAliveTimeout >= now) {
      return Status::OK();
    }
    master_term = this->m_term_;
  }

  std::string info_file_path;
  auto s = GetInfoFilePath(&info_file_path);
  if (!s.ok()) {
    return s;
  }

  (void) CASReplState(NEEDS_CHECK_SYNC_TIMEOUT_STATES,
                      master_term,
                      [this, now, info_file_path]()->Status{
                        if (m_info_.LastRecvTime() + kRecvKeepAliveTimeout < now) {
                          auto s = this->ResetMasterUnsafe(info_file_path, "SyncSlavePartition::CheckSyncTimeout");
                          if (s.ok()) {
                            g_pika_server->SetLoopPartitionStateMachine(true);
                          }
                          return s;
                        }
                        return Status::Incomplete("sync not timeout, skip...");
                      },
                      ReplState::kTryConnect,
                      "SyncSlavePartition::CheckSyncTimeout");
  return Status::OK();
}

Status SyncSlavePartition::ResetReplication(uint32_t master_term, const std::string& reason) {
  std::string info_file_path;
  auto s = GetInfoFilePath(&info_file_path);
  if (!s.ok()) {
    LOG(WARNING) << reason << ", can't get info_file_path, err: " << s.ToString();
    return s;
  }
  return CASReplState(std::vector<ReplState>{ReplState::kConnected, ReplState::kWaitDBSync},
                      master_term,
                      [this, info_file_path, reason]()->Status{ return this->ResetMasterUnsafe(info_file_path, reason); },
                      ReplState::kTryConnect,
                      reason);
}

Status SyncSlavePartition::GetInfoFilePath(std::string* info_file_path) {
  std::shared_ptr<Partition> table_partition = g_pika_server->GetTablePartitionById(partition_info_.table_name_, partition_info_.partition_id_);
  if (!table_partition) {
    std::stringstream ss;
    ss << "Partition " << partition_info_.ToString() << " not found";
    return Status::Corruption(ss.str());
  }
  *info_file_path = table_partition->GetDBSyncTermInfoFile();
  return Status::OK();
}

Status SyncSlavePartition::GetInfo(std::string* info) {
  std::string tmp_str = "  Role: Slave\r\n";
  tmp_str += "  master: " + MasterIp() + ":" + std::to_string(MasterPort()) + "\r\n";
  info->append(tmp_str);
  return Status::OK();
}

Status SyncSlavePartition::Activate(const RmNode& master, const ReplState& repl_state, const std::string& info_file_path) {
  slash::RWLock l(&partition_mu_, true);
  if (master.Ip().empty() || master.Port() <= 0 || master.Port() >= 65536) {
    std::stringstream ss;
    ss << "invalid master addr '" << master.GetAddr() << "'";
    return Status::Corruption(ss.str());
  }

  if (master.Ip() == m_info_.Ip() && master.Port() == m_info_.Port()) {
    std::stringstream ss;
    ss << "same master '" << master.GetAddr() << "' as previous one";
    return Status::Corruption(ss.str());
  }

  Status ret = SetMasterUnsafe(master, info_file_path, "Activate Replication");
  if (!ret.ok()) {
    return ret;
  }
  SetReplStateUnsafe(repl_state);
  return Status::OK();
}

void SyncSlavePartition::Deactivate() {
  slash::RWLock l(&partition_mu_, true);
  SetReplStateUnsafe(ReplState::kNoConnect);
}

std::string SyncSlavePartition::ToStringStatus() {
  return "  Master: " + MasterIp() + ":" + std::to_string(MasterPort()) + "\r\n" +
    "  SessionId: " + std::to_string(MasterSessionId()) + "\r\n" +
    "  SyncStatus " + ReplStateMsg[repl_state_] + "\r\n";
}

/* SyncWindow */

void SyncWindow::Push(const SyncWinItem& item) {
  win_.push_back(item);
}

bool SyncWindow::Update(const SyncWinItem& start_item,
    const SyncWinItem& end_item, BinlogOffset* acked_offset) {
  size_t start_pos = win_.size(), end_pos = win_.size();
  for (size_t i = 0; i < win_.size(); ++i) {
    if (win_[i] == start_item) {
      start_pos = i;
    }
    if (win_[i] == end_item) {
      end_pos = i;
      break;
    }
  }
  if (start_pos == win_.size() || end_pos == win_.size()) {
    LOG(WARNING) << "Ack offset Start: " <<
      start_item.ToString() << "End: " << end_item.ToString() <<
      " not found in binlog controller window." <<
      std::endl << "window status "<< std::endl << ToStringStatus();
    return false;
  }
  for (size_t i = start_pos; i <= end_pos; ++i) {
    win_[i].acked_ = true;
  }
  while (!win_.empty()) {
    if (win_[0].acked_) {
      *acked_offset = win_[0].offset_;
      win_.pop_front();
    } else {
      break;
    }
  }
  return true;
}

int SyncWindow::Remainings() {
  std::size_t remaining_size = g_pika_conf->sync_window_size() - win_.size();
  return remaining_size > 0? remaining_size:0 ;
}

/* PikaReplicaManger */

PikaReplicaManager::PikaReplicaManager()
    : last_meta_sync_timestamp_(0) {
  std::set<std::string> ips;
  ips.insert("0.0.0.0");
  int port = g_pika_conf->port() + kPortShiftReplServer;
  pika_repl_client_ = new PikaReplClient(3000, 60);
  pika_repl_server_ = new PikaReplServer(ips, port, 3000);
  InitPartition();
  pthread_rwlock_init(&partitions_rw_, NULL);
}

PikaReplicaManager::~PikaReplicaManager() {
  delete pika_repl_client_;
  delete pika_repl_server_;
  pthread_rwlock_destroy(&partitions_rw_);
}

void PikaReplicaManager::Start() {
  int ret = 0;
  ret = pika_repl_client_->Start();
  if (ret != pink::kSuccess) {
    LOG(FATAL) << "Start Repl Client Error: " << ret << (ret == pink::kCreateThreadError ? ": create thread error " : ": other error");
  }

  ret = pika_repl_server_->Start();
  if (ret != pink::kSuccess) {
    LOG(FATAL) << "Start Repl Server Error: " << ret << (ret == pink::kCreateThreadError ? ": create thread error " : ": other error");
  }
}

void PikaReplicaManager::Stop() {
  pika_repl_client_->Stop();
  pika_repl_server_->Stop();
}

void PikaReplicaManager::InitPartition() {
  std::vector<TableStruct> table_structs = g_pika_conf->table_structs();
  for (const auto& table : table_structs) {
    const std::string& table_name = table.table_name;
    for (const auto& partition_id : table.partition_ids) {
      sync_master_partitions_[PartitionInfo(table_name, partition_id)]
        = std::make_shared<SyncMasterPartition>(table_name, partition_id);
      sync_slave_partitions_[PartitionInfo(table_name, partition_id)]
        = std::make_shared<SyncSlavePartition>(table_name, partition_id);
    }
  }
}

void PikaReplicaManager::ProduceWriteQueue(const std::string& ip, int port, const std::vector<WriteTask>& tasks) {
  slash::MutexLock l(&write_queue_mu_);
  std::string index = ip + ":" + std::to_string(port);
  for (auto& task : tasks) {
    write_queues_[index].push(task);
  }
}

int PikaReplicaManager::ConsumeWriteQueue() {
  std::vector<std::string> to_delete;
  std::unordered_map<std::string, std::vector<std::vector<WriteTask>>> to_send_map;
  int counter = 0;
  {
    slash::MutexLock l(&write_queue_mu_);
    std::vector<std::string> to_delete;
    for (auto& iter : write_queues_) {
      std::queue<WriteTask>& queue = iter.second;
      for (int i = 0; i < kBinlogSendPacketNum; ++i) {
        if (queue.empty()) {
          break;
        }
        size_t batch_index = queue.size() > kBinlogSendBatchNum ? kBinlogSendBatchNum : queue.size();
        std::vector<WriteTask> to_send;
        int batch_size = 0;
        for (size_t i = 0; i < batch_index; ++i) {
          WriteTask& task = queue.front();
          batch_size +=  task.binlog_chip_.binlog_.size();
          // make sure SerializeToString will not over 2G
          if (batch_size > PIKA_MAX_CONN_RBUF_HB) {
            break;
          }
          to_send.push_back(queue.front());
          queue.pop();
          counter++;
        }
        if (!to_send.empty()) {
          to_send_map[iter.first].push_back(std::move(to_send));
        }
      }
    }
  }

  for (auto& iter : to_send_map) {
    std::string ip;
    int port = 0;
    if (!slash::ParseIpPortString(iter.first, ip, port)) {
      LOG(WARNING) << "Parse ip_port error " << iter.first;
      continue;
    }
    for (auto& to_send : iter.second) {
      Status s = pika_repl_server_->SendSlaveBinlogChips(ip, port, to_send);
      if (!s.ok()) {
        LOG(WARNING) << "send binlog to " << ip << ":" << port << " failed, " << s.ToString();
        to_delete.push_back(iter.first);
        continue;
      }
    }
  }

  if (!to_delete.empty()) {
    {
      slash::MutexLock l(&write_queue_mu_);
      for (auto& del_queue : to_delete) {
        write_queues_.erase(del_queue);
      }
    }
  }
  return counter;
}

void PikaReplicaManager::DropItemInWriteQueue(const std::string& ip, int port) {
  slash::MutexLock l(&write_queue_mu_);
  std::string index = ip + ":" + std::to_string(port);
  write_queues_.erase(index);
}

void PikaReplicaManager::ScheduleReplServerBGTask(pink::TaskFunc func, void* arg) {
  pika_repl_server_->Schedule(func, arg);
}

void PikaReplicaManager::ScheduleReplClientBGTask(pink::TaskFunc func, void* arg) {
  pika_repl_client_->Schedule(func, arg);
}

void PikaReplicaManager::ScheduleWriteBinlogTask(const std::string& table_partition,
        const std::shared_ptr<InnerMessage::InnerResponse> res,
        std::shared_ptr<pink::PbConn> conn,
        void* res_private_data) {
  pika_repl_client_->ScheduleWriteBinlogTask(table_partition, res, conn, res_private_data);
}

void PikaReplicaManager::ScheduleWriteDBTask(const std::string& dispatch_key,
        PikaCmdArgsType* argv, BinlogItem* binlog_item,
        const std::string& table_name, uint32_t partition_id) {
  pika_repl_client_->ScheduleWriteDBTask(dispatch_key, argv, binlog_item, table_name, partition_id);
}

void PikaReplicaManager::ReplServerRemoveClientConn(int fd) {
  pika_repl_server_->RemoveClientConn(fd);
}

void PikaReplicaManager::ReplServerUpdateClientConnMap(const std::string& ip_port,
                                                       int fd) {
  pika_repl_server_->UpdateClientConnMap(ip_port, fd);
}

Status PikaReplicaManager::UpdateSyncBinlogStatus(const RmNode& slave, const BinlogOffset& range_start, const BinlogOffset& range_end) {
  slash::RWLock l(&partitions_rw_, false);
  std::shared_ptr<SyncMasterPartition> partition = getSyncMasterPartitionByNameLocked(slave.NodePartitionInfo());
  if (!partition) {
    return Status::NotFound(slave.ToString() + " not found");
  }
  Status s = partition->UpdateSlaveBinlogAckInfo(slave.Ip(), slave.Port(), range_start, range_end);
  if (!s.ok()) {
    return s;
  }
  s = partition->SyncBinlogToWq(slave.Ip(), slave.Port());
  if (!s.ok()) {
    return s;
  }
  return Status::OK();
}

Status PikaReplicaManager::GetSyncBinlogStatus(const RmNode& slave, BinlogOffset*  sent_offset, BinlogOffset* acked_offset) {
  slash::RWLock l(&partitions_rw_, false);
  std::shared_ptr<SyncMasterPartition> partition = this->getSyncMasterPartitionByNameLocked(slave.NodePartitionInfo());
  if (!partition) {
    return Status::NotFound(slave.ToString() + " not found");
  }
  Status s = partition->GetSlaveSyncBinlogInfo(slave.Ip(), slave.Port(), sent_offset, acked_offset);
  if (!s.ok()) {
    return s;
  }
  return Status::OK();
}

Status PikaReplicaManager::GetSyncMasterPartitionSlaveState(const RmNode& slave,
                                                            SlaveState* const slave_state) {
  slash::RWLock l(&partitions_rw_, false);
  std::shared_ptr<SyncMasterPartition> partition = this->getSyncMasterPartitionByNameLocked(slave.NodePartitionInfo());
  if (!partition) {
    return Status::NotFound(slave.ToString() + " not found");
  }
  Status s = partition->GetSlaveState(slave.Ip(), slave.Port(), slave_state);
  if (!s.ok()) {
    return s;
  }
  return Status::OK();
}

bool PikaReplicaManager::CheckPartitionSlaveExist(const RmNode& slave) {
  slash::RWLock l(&partitions_rw_, false);
  std::shared_ptr<SyncMasterPartition> partition = this->getSyncMasterPartitionByNameLocked(slave.NodePartitionInfo());
  if (!partition) {
    return false;
  }
  return partition->CheckSlaveNodeExist(slave.Ip(), slave.Port());
}

bool PikaReplicaManager::CheckSlaveDBConnect() {
  std::shared_ptr<SyncSlavePartition> partition = nullptr;
  for (auto iter : g_pika_rm->sync_slave_partitions_) {
    partition = iter.second;
    if (partition->State() == ReplState::kDBNoConnect) {
      LOG(INFO) << "DB: " << partition->SyncPartitionInfo().ToString()
        << " has been dbslaveof no one, then will not try reconnect.";
      return false;
    }
  }
  return true;
}

Status PikaReplicaManager::GetPartitionSlaveSession(const RmNode& slave, int32_t* session) {
  slash::RWLock l(&partitions_rw_, false);
  std::shared_ptr<SyncMasterPartition> partition = this->getSyncMasterPartitionByNameLocked(slave.NodePartitionInfo());
  if (!partition) {
    return Status::NotFound(slave.ToString() + " not found");
  }
  return partition->GetSlaveNodeSession(slave.Ip(), slave.Port(), session);
}

Status PikaReplicaManager::AddPartitionSlave(const RmNode& slave, uint32_t master_term) {
  slash::RWLock l(&partitions_rw_, false);
  std::shared_ptr<SyncMasterPartition> partition = this->getSyncMasterPartitionByNameLocked(slave.NodePartitionInfo());
  if (!partition) {
    return Status::NotFound(slave.ToString() + " not found");
  }
  Status s= partition->RemoveSlaveNode(slave.Ip(), slave.Port());
  if (!s.ok() && !s.IsNotFound()) {
    return s;
  }
  s = partition->AddSlaveNode(slave.Ip(), slave.Port(), slave.PartitionId(), slave.SessionId(), master_term);
  if (!s.ok()) {
    return s;
  }
  return Status::OK();
}

Status PikaReplicaManager::RemovePartitionSlave(const RmNode& slave) {
  slash::RWLock l(&partitions_rw_, false);
  std::shared_ptr<SyncMasterPartition> partition = this->getSyncMasterPartitionByNameLocked(slave.NodePartitionInfo());
  if (!partition) {
    return Status::NotFound(slave.ToString() + " not found");
  }
  Status s = partition->RemoveSlaveNode(slave.Ip(), slave.Port());
  if (!s.ok()) {
    return s;
  }
  return Status::OK();
}

Status PikaReplicaManager::LostConnection(const std::string& ip, int port) {
  slash::RWLock l(&partitions_rw_, false);
  for (auto& iter : sync_master_partitions_) {
    std::shared_ptr<SyncMasterPartition> partition = iter.second;
    Status s = partition->RemoveSlaveNode(ip, port);
    if (!s.ok() && !s.IsNotFound()) {
      LOG(WARNING) << "Lost Connection failed " << s.ToString();
    }
  }

  for (auto& iter : sync_slave_partitions_) {
    std::shared_ptr<SyncSlavePartition> partition = iter.second;
    if (partition->MasterIp() == ip && partition->MasterPort() == port) {
      partition->Deactivate();
    }
  }
  return Status::OK();
}

Status PikaReplicaManager::ActivateBinlogSync(const RmNode& slave, const BinlogOffset& offset) {
  slash::RWLock l(&partitions_rw_, false);
  std::shared_ptr<SyncMasterPartition> sync_partition = this->getSyncMasterPartitionByNameLocked(slave.NodePartitionInfo());
  if (!sync_partition) {
    return Status::NotFound(slave.ToString() + " not found");
  }

  std::shared_ptr<Partition> partition = g_pika_server->GetTablePartitionById(slave.TableName(), slave.PartitionId());
  if (!partition) {
    return Status::Corruption("Found Binlog failed");
  }

  Status s = sync_partition->ActivateSlaveBinlogSync(slave.Ip(), slave.Port(), partition->logger(), offset);
  if (!s.ok()) {
    return s;
  }
  return Status::OK();
}

Status PikaReplicaManager::ActivateDbSync(const RmNode& slave) {
  slash::RWLock l(&partitions_rw_, false);
  std::shared_ptr<SyncMasterPartition> partition = this->getSyncMasterPartitionByNameLocked(slave.NodePartitionInfo());
  if (!partition) {
    return Status::NotFound(slave.ToString() + " not found");
  }
  Status s = partition->ActivateSlaveDbSync(slave.Ip(), slave.Port());
  if (!s.ok()) {
    return s;
  }
  return Status::OK();
}

Status PikaReplicaManager::SetMasterLastRecvTime(const RmNode& node, uint64_t time) {
  slash::RWLock l(&partitions_rw_, false);
  std::shared_ptr<SyncMasterPartition> partition = this->getSyncMasterPartitionByNameLocked(node.NodePartitionInfo());
  if (!partition) {
    return Status::NotFound(node.ToString() + " not found");
  }
  partition->SetLastRecvTime(node.Ip(), node.Port(), time);
  return Status::OK();
}

Status PikaReplicaManager::SetSlaveLastRecvTime(const RmNode& node, uint64_t time) {
  slash::RWLock l(&partitions_rw_, false);
  if (sync_slave_partitions_.find(node.NodePartitionInfo()) == sync_slave_partitions_.end()) {
    return Status::NotFound(node.ToString() + " not found");
  }
  std::shared_ptr<SyncSlavePartition> partition = sync_slave_partitions_[node.NodePartitionInfo()];
  partition->SetLastRecvTime(time);
  return Status::OK();
}

Status PikaReplicaManager::WakeUpBinlogSync() {
  slash::RWLock l(&partitions_rw_, false);
  for (auto& iter : sync_master_partitions_) {
    std::shared_ptr<SyncMasterPartition> partition = iter.second;
    Status s = partition->WakeUpSlaveBinlogSync();
    if (!s.ok()) {
      return s;
    }
  }
  return Status::OK();
}

int32_t PikaReplicaManager::GenPartitionSessionId(const std::string& table_name,
                                                  uint32_t partition_id) {
  slash::RWLock l(&partitions_rw_, false);
  PartitionInfo p_info(table_name, partition_id);
  std::shared_ptr<SyncMasterPartition> sync_master_partition = this->getSyncMasterPartitionByNameLocked(p_info);
  if (!sync_master_partition) {
    return -1;
  }
  return sync_master_partition->GenSessionId();
}

int32_t PikaReplicaManager::GetSlavePartitionSessionId(const std::string& table_name,
                                                       uint32_t partition_id) {
  slash::RWLock l(&partitions_rw_, false);
  PartitionInfo p_info(table_name, partition_id);
  if (sync_slave_partitions_.find(p_info) == sync_slave_partitions_.end()) {
    return -1;
  } else {
    std::shared_ptr<SyncSlavePartition> sync_slave_partition = sync_slave_partitions_[p_info];
    return sync_slave_partition->MasterSessionId();
  }
}

bool PikaReplicaManager::CheckSlavePartitionSessionId(const std::string& table_name,
                                                      uint32_t partition_id,
                                                      int session_id) {
  slash::RWLock l(&partitions_rw_, false);
  PartitionInfo p_info(table_name, partition_id);
  if (sync_slave_partitions_.find(p_info) == sync_slave_partitions_.end()) {
    LOG(WARNING)<< "Slave Partition Not Found: " << p_info.ToString().data();
    return false;
  } else {
    std::shared_ptr<SyncSlavePartition> sync_slave_partition = sync_slave_partitions_[p_info];
    if (sync_slave_partition->MasterSessionId() != session_id) {
      LOG(WARNING)<< "Check SessionId Mismatch: " << sync_slave_partition->MasterIp()
        << ":" << sync_slave_partition->MasterPort() << ", "
        << sync_slave_partition->SyncPartitionInfo().ToString()
        << " expected_session: " << session_id << ", actual_session:"
        << sync_slave_partition->MasterSessionId();
      return false;
    }
  }
  return true;
}

bool PikaReplicaManager::CheckMasterPartitionSessionId(const std::string& ip, int port,
                                                       const std::string& table_name,
                                                       uint32_t partition_id, int session_id) {
  slash::RWLock l(&partitions_rw_, false);
  PartitionInfo p_info(table_name, partition_id);
  std::shared_ptr<SyncMasterPartition> sync_master_partition = this->getSyncMasterPartitionByNameLocked(p_info);
  if (!sync_master_partition) {
    return false;
  }
  return sync_master_partition->CheckSessionId(ip, port, table_name, partition_id, session_id);
}

Status PikaReplicaManager::CheckSyncTimeout(uint64_t now) {
  slash::RWLock l(&partitions_rw_, false);

  for (auto& iter : sync_master_partitions_) {
    std::shared_ptr<SyncMasterPartition> partition = iter.second;
    Status s = partition->CheckSyncTimeout(now);
    if (!s.ok()) {
      LOG(WARNING) << "CheckSyncTimeout Failed " << s.ToString();
    }
  }
  for (auto& iter : sync_slave_partitions_) {
    std::shared_ptr<SyncSlavePartition> partition = iter.second;
    Status s = partition->CheckSyncTimeout(now);
    if (!s.ok()) {
      LOG(WARNING) << "CheckSyncTimeout Failed " << s.ToString();
    }
  }
  return Status::OK();
}

Status PikaReplicaManager::CheckPartitionRole(
    const std::string& table, uint32_t partition_id, int* role) {
  slash::RWLock l(&partitions_rw_, false);
  *role = 0;
  PartitionInfo p_info(table, partition_id);
  std::shared_ptr<SyncMasterPartition> sync_master_partition = this->getSyncMasterPartitionByNameLocked(p_info);
  if (!sync_master_partition) {
    return Status::NotFound(table + std::to_string(partition_id) + " not found");
  }
  if (sync_slave_partitions_.find(p_info) == sync_slave_partitions_.end()) {
    return Status::NotFound(table + std::to_string(partition_id) + " not found");
  }
  if (sync_master_partition->GetNumberOfSlaveNode() != 0) {
    *role |= PIKA_ROLE_MASTER;
  }
  if (sync_slave_partitions_[p_info]->State() == ReplState::kConnected) {
    *role |= PIKA_ROLE_SLAVE;
  }
  // if role is not master or slave, the rest situations are all single
  return Status::OK();
}

Status PikaReplicaManager::GetPartitionInfo(
    const std::string& table, uint32_t partition_id, std::string* info) {
  int role = 0;
  std::string tmp_res;
  Status s = CheckPartitionRole(table, partition_id, &role);
  if (!s.ok()) {
    return s;
  }

  bool add_divider_line = ((role & PIKA_ROLE_MASTER) && (role & PIKA_ROLE_SLAVE));
  slash::RWLock l(&partitions_rw_, false);
  PartitionInfo p_info(table, partition_id);
  if (role & PIKA_ROLE_MASTER) {
    std::shared_ptr<SyncMasterPartition> sync_master_partition = this->getSyncMasterPartitionByNameLocked(p_info);
    if (!sync_master_partition) {
      return Status::NotFound(table + std::to_string(partition_id) + " not found");
    }
    Status s = sync_master_partition->GetInfo(info);
    if (!s.ok()) {
      return s;
    }
  }
  if (add_divider_line) {
    info->append("  -----------\r\n");
  }
  if (role & PIKA_ROLE_SLAVE) {
    if (sync_slave_partitions_.find(p_info) == sync_slave_partitions_.end()) {
      return Status::NotFound(table + std::to_string(partition_id) + " not found");
    }
    Status s = sync_slave_partitions_[p_info]->GetInfo(info);
    if (!s.ok()) {
      return s;
    }
  }
  info->append("\r\n");
  return Status::OK();
}

Status PikaReplicaManager::SelectLocalIp(const std::string& remote_ip,
                                         const int remote_port,
                                         std::string* const local_ip) {
  pink::PinkCli* cli = pink::NewRedisCli();
  cli->set_connect_timeout(1500);
  if ((cli->Connect(remote_ip, remote_port, "")).ok()) {
    struct sockaddr_in laddr;
    socklen_t llen = sizeof(laddr);
    getsockname(cli->fd(), (struct sockaddr*) &laddr, &llen);
    std::string tmp_ip(inet_ntoa(laddr.sin_addr));
    *local_ip = tmp_ip;
    cli->Close();
    delete cli;
  } else {
    LOG(WARNING) << "Failed to connect remote node("
      << remote_ip << ":" << remote_port << ")";
    delete cli;
    return Status::Corruption("connect remote node error");
  }
  return Status::OK();
}

Status PikaReplicaManager::ActivateSyncSlavePartition(const RmNode& node,
                                                      const ReplState& repl_state,
                                                      bool resharding) {
  slash::RWLock l(&partitions_rw_, false);
  const PartitionInfo& p_info = node.NodePartitionInfo();
  if (sync_slave_partitions_.find(p_info) == sync_slave_partitions_.end()) {
    return Status::NotFound("Sync Slave partition " + node.ToString() + " not found");
  }
  ReplState ssp_state  = sync_slave_partitions_[p_info]->State();
  if (ssp_state != ReplState::kNoConnect && ssp_state != ReplState::kDBNoConnect) {
    return Status::Corruption("Sync Slave partition in " + ReplStateMsg[ssp_state]);
  }
  std::string local_ip;
  std::shared_ptr<Partition> table_partition = g_pika_server->GetTablePartitionById(node.TableName(), node.PartitionId());
  if (!table_partition) {
    std::stringstream ss;
    ss << "Partition " << node.PartitionId() << " not found";
    return Status::Corruption(ss.str());
  }
  Status s = SelectLocalIp(node.Ip(), node.Port(), &local_ip);
  if (s.ok()) {
    s = sync_slave_partitions_[p_info]->Activate(node, repl_state, table_partition->GetDBSyncTermInfoFile());
    if (s.ok()) {
      sync_slave_partitions_[p_info]->SetLocalIp(local_ip);
      sync_slave_partitions_[p_info]->SetResharding(resharding);
    }
  }
  return s;
}

Status PikaReplicaManager::UpdateSyncSlavePartitionSessionId(const PartitionInfo& p_info,
                                                             int32_t session_id) {
  slash::RWLock l(&partitions_rw_, false);
  if (sync_slave_partitions_.find(p_info) == sync_slave_partitions_.end()) {
    return Status::NotFound("Sync Slave partition " + p_info.ToString());
  }
  sync_slave_partitions_[p_info]->SetMasterSessionId(session_id);
  return Status::OK();
}

Status PikaReplicaManager::DeactivateSyncSlavePartition(const PartitionInfo& p_info) {
  slash::RWLock l(&partitions_rw_, false);
  if (sync_slave_partitions_.find(p_info) == sync_slave_partitions_.end()) {
    return Status::NotFound("Sync Slave partition " + p_info.ToString());
  }
  sync_slave_partitions_[p_info]->Deactivate();
  return Status::OK();
}

Status PikaReplicaManager::SetSlaveReplState(const PartitionInfo& p_info,
                                             const ReplState& repl_state) {
  slash::RWLock l(&partitions_rw_, false);
  if (sync_slave_partitions_.find(p_info) == sync_slave_partitions_.end()) {
    return Status::NotFound("Sync Slave partition " + p_info.ToString());
  }
  sync_slave_partitions_[p_info]->SetReplState(repl_state);
  return Status::OK();
}

Status PikaReplicaManager::CASSlaveReplState(const PartitionInfo& p_info,
                                             const ReplState& current_state, uint32_t current_term,
                                             const ReplState& new_state, const std::string& reason) {
  slash::RWLock l(&partitions_rw_, false);
  if (sync_slave_partitions_.find(p_info) == sync_slave_partitions_.end()) {
    LOG(WARNING) << "Sync Slave partition " << p_info.ToString();
    return Status::NotFound("Sync Slave partition " + p_info.ToString());
  }
  return sync_slave_partitions_[p_info]->CASReplState(current_state, current_term, new_state, reason);
}

Status PikaReplicaManager::GetSlaveReplState(const PartitionInfo& p_info,
                                             ReplState* repl_state) {
  slash::RWLock l(&partitions_rw_, false);
  if (sync_slave_partitions_.find(p_info) == sync_slave_partitions_.end()) {
    return Status::NotFound("Sync Slave partition " + p_info.ToString());
  }
  *repl_state = sync_slave_partitions_[p_info]->State();
  return Status::OK();
}

Status PikaReplicaManager::SendMetaSyncRequest() {
  Status s;
  int now = time(NULL);
  if (now - last_meta_sync_timestamp_ >= PIKA_META_SYNC_MAX_WAIT_TIME) {
    s = pika_repl_client_->SendMetaSync();
    if (s.ok()) {
      last_meta_sync_timestamp_ = now;
    }
  }
  return s;
}

Status PikaReplicaManager::SendRemoveSlaveNodeRequest(const std::string& table,
                                                      uint32_t partition_id) {
  slash::RWLock l(&partitions_rw_, false);
  slash::Status s;
  PartitionInfo p_info(table, partition_id);
  if (sync_slave_partitions_.find(p_info) == sync_slave_partitions_.end()) {
    return Status::NotFound("Sync Slave partition " + p_info.ToString());
  } else {
    std::shared_ptr<SyncSlavePartition> s_partition = sync_slave_partitions_[p_info];
    s = pika_repl_client_->SendRemoveSlaveNode(s_partition->MasterIp(),
        s_partition->MasterPort(), table, partition_id, s_partition->LocalIp(), s_partition->MasterTerm());
    if (s.ok()) {
      s_partition->SetReplState(ReplState::kDBNoConnect);
    }
  }

  if (s.ok()) {
    LOG(INFO) << "SlaveNode (" << table << ":" << partition_id
      << "), stop sync success";
  } else {
    LOG(WARNING) << "SlaveNode (" << table << ":" << partition_id
      << "), stop sync faild, " << s.ToString();
  }
  return s;
}

Status PikaReplicaManager::SendPartitionTrySyncRequest(
        const std::string& table_name, size_t partition_id) {
  BinlogOffset boffset;
  if (!g_pika_server->GetTablePartitionBinlogOffset(
              table_name, partition_id, &boffset)) {
    LOG(WARNING) << "Partition: " << table_name << ":" << partition_id
        << ",  Get partition binlog offset failed";
    return Status::Corruption("Partition get binlog offset error");
  }

  std::shared_ptr<SyncSlavePartition> slave_partition =
      GetSyncSlavePartitionByName(PartitionInfo(table_name, partition_id));
  if (!slave_partition) {
    LOG(WARNING) << "Slave Partition: " << table_name << ":" << partition_id
        << ", NotFound";
    return Status::Corruption("Slave Partition not found");
  }

  uint32_t master_term = slave_partition->MasterTerm();
  Status status = pika_repl_client_->SendPartitionTrySync(slave_partition->MasterIp(),
                                                          slave_partition->MasterPort(),
                                                          table_name, partition_id, boffset,
                                                          slave_partition->LocalIp(),
                                                          master_term);

  if (status.ok()) {
    return g_pika_rm->CASSlaveReplState(PartitionInfo(table_name, partition_id), ReplState::kTryConnect, master_term, ReplState::kWaitReply, "SendPartitionTrySyncRequest successfully");
  } else {
    std::stringstream ss;
    ss << "SendPartitionTrySync failed " << status.ToString();
    (void) g_pika_rm->CASSlaveReplState(PartitionInfo(table_name, partition_id), ReplState::kTryConnect, master_term, ReplState::kError, ss.str());
    return status;
  }
}

Status PikaReplicaManager::SendPartitionDBSyncRequest(
        const std::string& table_name, size_t partition_id) {
  BinlogOffset boffset;
  if (!g_pika_server->GetTablePartitionBinlogOffset(
              table_name, partition_id, &boffset)) {
    LOG(WARNING) << "Partition: " << table_name << ":" << partition_id
        << ",  Get partition binlog offset failed";
    return Status::Corruption("Partition get binlog offset error");
  }

  std::shared_ptr<Partition> partition =
    g_pika_server->GetTablePartitionById(table_name, partition_id);
  if (!partition) {
    LOG(WARNING) << "Partition: " << table_name << ":" << partition_id
        << ", NotFound";
    return Status::Corruption("Partition not found");
  }

  std::shared_ptr<SyncSlavePartition> slave_partition =
      GetSyncSlavePartitionByName(PartitionInfo(table_name, partition_id));
  if (!slave_partition) {
    LOG(WARNING) << "Slave Partition: " << table_name << ":" << partition_id
                 << ", NotFound";
    return Status::Corruption("Slave Partition not found");
  }

  uint32_t master_term = slave_partition->MasterTerm();
  if (!partition->PrepareRsync(master_term)) {
    std::stringstream ss;
    ss << "Prepare rsync " << table_name << ":" << partition_id << " failed";
    (void) g_pika_rm->CASSlaveReplState(PartitionInfo(table_name, partition_id), ReplState::kTryDBSync, master_term, ReplState::kError, ss.str());
    return Status::Corruption("Prepare rsync failed");
  }
  if (master_term != slave_partition->MasterTerm()) {
    return Status::Corruption("master term changed");
  }
  Status status = pika_repl_client_->SendPartitionDBSync(slave_partition->MasterIp(),
                                                         slave_partition->MasterPort(),
                                                         table_name, partition_id, boffset,
                                                         slave_partition->LocalIp(),
                                                         master_term);
  if (status.ok()) {
    return g_pika_rm->CASSlaveReplState(PartitionInfo(table_name, partition_id), ReplState::kTryDBSync, master_term, ReplState::kWaitReply, "SendPartitionDbSync successfully");
  } else {
    std::stringstream ss;
    ss << "SendPartitionDbSync failed " << status.ToString();
    (void) g_pika_rm->CASSlaveReplState(PartitionInfo(table_name, partition_id), ReplState::kTryDBSync, master_term, ReplState::kError, ss.str());
    return status;
  }
}

Status PikaReplicaManager::SendPartitionBinlogSyncAckRequest(
        const std::string& table, uint32_t partition_id,
        const BinlogOffset& ack_start, const BinlogOffset& ack_end,
        bool is_first_send) {
  std::shared_ptr<SyncSlavePartition> slave_partition =
      GetSyncSlavePartitionByName(PartitionInfo(table, partition_id));
  if (!slave_partition) {
    LOG(WARNING) << "Slave Partition: " << table << ":" << partition_id
        << ", NotFound";
    return Status::Corruption("Slave Partition not found");
  }
  return pika_repl_client_->SendPartitionBinlogSync(
          slave_partition->MasterIp(), slave_partition->MasterPort(),
          table, partition_id, ack_start, ack_end, slave_partition->LocalIp(),
          is_first_send);
}

Status PikaReplicaManager::CloseReplClientConn(const std::string& ip, int32_t port) {
  return pika_repl_client_->Close(ip, port);
}

Status PikaReplicaManager::SendSlaveBinlogChipsRequest(const std::string& ip,
                                                       int port,
                                                       const std::vector<WriteTask>& tasks) {
  return pika_repl_server_->SendSlaveBinlogChips(ip, port, tasks);
}

std::shared_ptr<SyncMasterPartition>
PikaReplicaManager::GetSyncMasterPartitionByName(const PartitionInfo& p_info) {
  slash::RWLock l(&partitions_rw_, false);
  return getSyncMasterPartitionByNameLocked(p_info);
}

std::shared_ptr<SyncMasterPartition>
PikaReplicaManager::getSyncMasterPartitionByNameLocked(const PartitionInfo& p_info) {
  auto tb = g_pika_server->GetTable(p_info.table_name_);
  if (!tb) {
    return nullptr;
  }
  PartitionInfo adjusted_p_info = p_info.Adjust(tb->PartitionNum());
  if (sync_master_partitions_.find(adjusted_p_info) == sync_master_partitions_.end()) {
    return nullptr;
  }
  return sync_master_partitions_[adjusted_p_info];
}

Status PikaReplicaManager::GetSafetyPurgeBinlogFromSMP(const std::string& table_name,
                                                       uint32_t partition_id,
                                                       std::string* safety_purge) {
  std::shared_ptr<SyncMasterPartition> master_partition =
      GetSyncMasterPartitionByName(PartitionInfo(table_name, partition_id));
  if (!master_partition) {
    LOG(WARNING) << "Sync Master Partition: " << table_name << ":" << partition_id
        << ", NotFound";
    return Status::NotFound("SyncMasterPartition NotFound");
  } else {
    return master_partition->GetSafetyPurgeBinlog(safety_purge);
  }
}

bool PikaReplicaManager::BinlogCloudPurgeFromSMP(const std::string& table_name,
                                                 uint32_t partition_id, uint32_t index) {
  std::shared_ptr<SyncMasterPartition> master_partition =
      GetSyncMasterPartitionByName(PartitionInfo(table_name, partition_id));
  if (!master_partition) {
    LOG(WARNING) << "Sync Master Partition: " << table_name << ":" << partition_id
        << ", NotFound";
    return false;
  } else {
    return master_partition->BinlogCloudPurge(index);
  }
}

std::shared_ptr<SyncSlavePartition>
PikaReplicaManager::GetSyncSlavePartitionByName(const PartitionInfo& p_info) {
  slash::RWLock l(&partitions_rw_, false);
  if (sync_slave_partitions_.find(p_info) == sync_slave_partitions_.end()) {
    return nullptr;
  }
  return sync_slave_partitions_[p_info];
}

Status PikaReplicaManager::RunSyncSlavePartitionStateMachine() {
  slash::RWLock l(&partitions_rw_, false);
  for (const auto& item : sync_slave_partitions_) {
    PartitionInfo p_info = item.first;
    std::shared_ptr<SyncSlavePartition> s_partition = item.second;
    if (s_partition->State() == ReplState::kTryConnect) {
      SendPartitionTrySyncRequest(p_info.table_name_, p_info.partition_id_);
    } else if (s_partition->State() == ReplState::kTryDBSync) {
      SendPartitionDBSyncRequest(p_info.table_name_, p_info.partition_id_);
    } else if (s_partition->State() == ReplState::kWaitReply) {
      continue;
    } else if (s_partition->State() == ReplState::kWaitDBSync) {
      std::shared_ptr<Partition> partition =
          g_pika_server->GetTablePartitionById(p_info.table_name_, p_info.partition_id_);
      if (partition) {
        if (!s_partition->Resharding()) {
          (void) partition->TryUpdateMasterOffset([](std::shared_ptr<blackwidow::BlackWidow>)->rocksdb::Status {
            return rocksdb::Status::OK();
          });
        } else {
          (void) partition->TryUpdateMasterOffset([partition](std::shared_ptr<blackwidow::BlackWidow> db)->rocksdb::Status {
            return db->RemoveKeys(blackwidow::DataType::kAll, [partition](const std::string& pika_key)->bool {
              return partition != g_pika_server->GetTablePartitionByKey(partition->GetTableName(), pika_key);
            });
          });
        }
      } else {
        LOG(WARNING) << "Partition not found, Table Name: "
                     << p_info.table_name_ << " Partition Id: " << p_info.partition_id_;
      }
    } else if (s_partition->State() == ReplState::kConnected
      || s_partition->State() == ReplState::kNoConnect
      || s_partition->State() == ReplState::kDBNoConnect) {
      continue;
    }
  }
  return Status::OK();
}

Status PikaReplicaManager::InitSlaveSyncPartitionsMasterTerm() {
  slash::RWLock l(&partitions_rw_, false);

  for (const auto& iter : sync_slave_partitions_) {
    auto s = iter.second->InitMasterTerm();
    if (!s.ok()) {
      return s;
    }
  }
  return Status::OK();
}

Status PikaReplicaManager::AddSyncPartitionSanityCheck(const std::set<PartitionInfo>& p_infos) {
  slash::RWLock l(&partitions_rw_, false);
  for (const auto& p_info : p_infos) {
    if (this->getSyncMasterPartitionByNameLocked(p_info) != nullptr
      || sync_slave_partitions_.find(p_info) != sync_slave_partitions_.end()) {
      LOG(WARNING) << "sync partition: " << p_info.ToString() << " exist";
      return Status::Corruption("sync partition " + p_info.ToString()
          + " exist");
    }
  }
  return Status::OK();
}

Status PikaReplicaManager::AddSyncPartition(
        const std::set<PartitionInfo>& p_infos) {
  Status s = AddSyncPartitionSanityCheck(p_infos);
  if (!s.ok()) {
    return s;
  }

  slash::RWLock l(&partitions_rw_, true);
  for (const auto& p_info : p_infos) {
    std::shared_ptr<SyncSlavePartition> sp =
      std::make_shared<SyncSlavePartition>(p_info.table_name_, p_info.partition_id_);
    s = sp->InitMasterTerm();
    if (!s.ok()) {
      return s;
    }
//    auto tb = g_pika_server->GetTable(p_info.table_name_);
//    if (!tb) {
//      return Status::NotFound("can't find table " + p_info.table_name_);
//    }
    sync_master_partitions_[p_info] = // TODO p_info.Adjust(tb->PartitionNum()) verify here
      std::make_shared<SyncMasterPartition>(p_info.table_name_,
          p_info.partition_id_);
    sync_slave_partitions_[p_info] = sp;
  }
  return Status::OK();
}

Status PikaReplicaManager::RemoveSyncPartitionSanityCheck(
    const std::set<PartitionInfo>& p_infos) {
  slash::RWLock l(&partitions_rw_, false);
  for (const auto& p_info : p_infos) {
    auto sync_master_partition = this->getSyncMasterPartitionByNameLocked(p_info);
    if (sync_master_partition == nullptr
      || sync_slave_partitions_.find(p_info) == sync_slave_partitions_.end()) {
      LOG(WARNING) << "sync partition: " << p_info.ToString() << " not found";
      return Status::Corruption("sync partition " + p_info.ToString()
              + " not found");
    }

    if (sync_master_partition->GetNumberOfSlaveNode() != 0) {
      LOG(WARNING) << "sync master partition: " << p_info.ToString()
          << " in syncing";
      return Status::Corruption("sync master partition " + p_info.ToString()
              + " in syncing");
    }

    ReplState state = sync_slave_partitions_[p_info]->State();
    if (state != kNoConnect && state != kError) {
      LOG(WARNING) << "sync slave partition: " << p_info.ToString()
          << " in " << ReplStateMsg[state] + " state";
      return Status::Corruption("sync slave partition " + p_info.ToString()
              + " in " + ReplStateMsg[state] + " state");
    }
  }
  return Status::OK();
}

Status PikaReplicaManager::RemoveSyncPartition(
        const std::set<PartitionInfo>& p_infos) {
  Status s = RemoveSyncPartitionSanityCheck(p_infos);
  if (!s.ok()) {
    return s;
  }

  slash::RWLock l(&partitions_rw_, true);
  for (const auto& p_info : p_infos) {
    auto tb = g_pika_server->GetTable(p_info.table_name_);
    if (!tb) {
      return Status::NotFound("can't find table " + p_info.table_name_);
    }
    sync_master_partitions_.erase(p_info.Adjust(tb->PartitionNum()));
    sync_slave_partitions_.erase(p_info);
  }
  return Status::OK();
}

void PikaReplicaManager::FindCompleteReplica(std::vector<std::string>* replica) {
  std::unordered_map<std::string, size_t> replica_slotnum;
  slash::RWLock l(&partitions_rw_, false);
  for (auto& iter : sync_master_partitions_) {
    std::vector<std::string> names;
    iter.second->GetValidSlaveNames(&names);
    for (auto& name : names) {
      if (replica_slotnum.find(name) == replica_slotnum.end()) {
        replica_slotnum[name] = 0;
      }
      replica_slotnum[name]++;
    }
  }
  for (auto item : replica_slotnum) {
    if (item.second == sync_master_partitions_.size()) {
      replica->push_back(item.first);
    }
  }
}

void PikaReplicaManager::FindCommonMaster(std::string* master) {
  slash::RWLock l(&partitions_rw_, false);
  std::string common_master_ip;
  int common_master_port = 0;
  for (auto& iter : sync_slave_partitions_) {
    if (iter.second->State() != kConnected) {
      return;
    }
    std::string tmp_ip = iter.second->MasterIp();
    int tmp_port = iter.second->MasterPort();
    if (common_master_ip.empty() && common_master_port == 0) {
      common_master_ip = tmp_ip;
      common_master_port = tmp_port;
    }
    if (tmp_ip != common_master_ip || tmp_port != common_master_port) {
      return;
    }
  }
  if (!common_master_ip.empty() && common_master_port != 0) {
    *master = common_master_ip + ":" + std::to_string(common_master_port);
  }
}

void PikaReplicaManager::RmStatus(std::string* info) {
  slash::RWLock l(&partitions_rw_, false);
  std::stringstream tmp_stream;
  tmp_stream << "Master partition(" << sync_master_partitions_.size() << "):" << "\r\n";
  for (auto& iter : sync_master_partitions_) {
    tmp_stream << " Partition " << iter.second->SyncPartitionInfo().ToString()
      << "\r\n" << iter.second->ToStringStatus() << "\r\n";
  }
  tmp_stream << "Slave partition(" << sync_slave_partitions_.size() << "):" << "\r\n";
  for (auto& iter : sync_slave_partitions_) {
    tmp_stream << " Partition " << iter.second->SyncPartitionInfo().ToString()
      << "\r\n" << iter.second->ToStringStatus() << "\r\n";
  }
  info->append(tmp_stream.str());
}
