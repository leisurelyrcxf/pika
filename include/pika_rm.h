// Copyright (c) 2015-present, Qihoo, Inc.  All rights reserved.
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree. An additional grant
// of patent rights can be found in the PATENTS file in the same directory.

#ifndef PIKA_RM_H_
#define PIKA_RM_H_

#include <string>
#include <memory>
#include <unordered_map>
#include <queue>
#include <vector>
#include <algorithm>

#include "slash/include/slash_status.h"

#include "include/pika_binlog_reader.h"
#include "include/pika_repl_client.h"
#include "include/pika_repl_server.h"

#define kBinlogSendPacketNum 40
#define kBinlogSendBatchNum 100

// unit seconds
#define kSendKeepAliveTimeout (2 * 1000000)
#define kRecvKeepAliveTimeout (20 * 1000000)

using slash::Status;

struct SyncWinItem {
  BinlogOffset offset_;
  bool acked_;
  bool operator==(const SyncWinItem& other) const {
    if (offset_.filenum == other.offset_.filenum && offset_.offset == other.offset_.offset) {
      return true;
    }
    return false;
  }
  explicit SyncWinItem(const BinlogOffset& offset) : offset_(offset), acked_(false) {
  }
  SyncWinItem(uint32_t filenum, uint64_t offset) : offset_(filenum, offset), acked_(false) {
  }
  std::string ToString() const {
    return offset_.ToString() + " acked: " + std::to_string(acked_);
  }
};

class SyncWindow {
 public:
  SyncWindow() {
  }
  void Push(const SyncWinItem& item);
  bool Update(const SyncWinItem& start_item, const SyncWinItem& end_item, BinlogOffset* acked_offset);
  int Remainings();
  std::string ToStringStatus() const {
    if (win_.empty()) {
      return "      Size: " + std::to_string(win_.size()) + "\r\n";
    } else {
      std::string res;
      res += "      Size: " + std::to_string(win_.size()) + "\r\n";
      res += ("      Begin_item: " + win_.begin()->ToString() + "\r\n");
      res += ("      End_item: " + win_.rbegin()->ToString() + "\r\n");
      return res;
    }
  }
 private:
  // TODO(whoiami) ring buffer maybe
  std::deque<SyncWinItem> win_;
};

// role master use
class SlaveNode : public RmNode {
 public:
  SlaveNode(const std::string& ip, int port, const std::string& table_name, uint32_t partition_id, int session_id, uint32_t master_term);
  ~SlaveNode();
  void Lock() {
    slave_mu.Lock();
  }
  void Unlock() {
    slave_mu.Unlock();
  }
  SlaveState slave_state;

  BinlogSyncState b_state;
  SyncWindow sync_win;
  BinlogOffset sent_offset;
  BinlogOffset acked_offset;
  uint32_t master_term_;

  std::string ToStringStatus();

  std::shared_ptr<PikaBinlogReader> binlog_reader;
  Status InitBinlogFileReader(const std::shared_ptr<Binlog>& binlog, const BinlogOffset& offset);
  void ReleaseBinlogFileReader();

  slash::Mutex slave_mu;
};

class SyncPartition {
 public:
  SyncPartition(const std::string& table_name, uint32_t partition_id);
  virtual ~SyncPartition() = default;

  PartitionInfo& SyncPartitionInfo() {
    return partition_info_;
  }
 protected:
  // std::shared_ptr<Binlog> binlog_;
  PartitionInfo partition_info_;
};

class SyncMasterPartition : public SyncPartition {
 public:
  SyncMasterPartition(const std::string& table_name, uint32_t partition_id);
  Status AddSlaveNode(const std::string& ip, int port, uint32_t partition_id, int session_id, uint32_t master_term);
  Status RemoveSlaveNode(const std::string& ip, int port);

  Status ActivateSlaveBinlogSync(const std::string& ip, int port, const std::shared_ptr<Binlog> binlog, const BinlogOffset& offset);
  Status ActivateSlaveDbSync(const std::string& ip, int port);

  Status SyncBinlogToWq(const std::string& ip, int port);
  Status UpdateSlaveBinlogAckInfo(const std::string& ip, int port, const BinlogOffset& start, const BinlogOffset& end);
  Status GetSlaveSyncBinlogInfo(const std::string& ip, int port, BinlogOffset* sent_offset, BinlogOffset* acked_offset);
  Status GetSlaveState(const std::string& ip, int port, SlaveState* const slave_state);

  Status SetLastSendTime(const std::string& ip, int port, uint64_t time);
  Status GetLastSendTime(const std::string& ip, int port, uint64_t* time);

  Status SetLastRecvTime(const std::string& ip, int port, uint64_t time);
  Status GetLastRecvTime(const std::string& ip, int port, uint64_t* time);

  Status GetSafetyPurgeBinlog(std::string* safety_purge);
  bool BinlogCloudPurge(uint32_t index);

  Status WakeUpSlaveBinlogSync();
  Status CheckSyncTimeout(uint64_t now);

  int GetNumberOfSlaveNode();
  bool CheckSlaveNodeExist(const std::string& ip, int port);
  Status GetSlaveNodeSession(const std::string& ip, int port, int32_t* session);

  void GetValidSlaveNames(std::vector<std::string>* slavenames);
  // display use
  Status GetInfo(std::string* info);
  // debug use
  std::string ToStringStatus();

  int32_t GenSessionId();
  bool    CheckSessionId(const std::string& ip, int port,
                         const std::string& table_name,
                         uint64_t partition_id, int session_id);

 private:
  bool CheckReadBinlogFromCache();
  // inovker need to hold partition_mu_
  void CleanMasterNode();
  void CleanSlaveNode();
  // invoker need to hold slave_mu_
  Status ReadCachedBinlogToWq(const std::shared_ptr<SlaveNode>& slave_ptr);
  Status ReadBinlogFileToWq(const std::shared_ptr<SlaveNode>& slave_ptr);
  // inovker need to hold partition_mu_
  Status GetSlaveNode(const std::string& ip, int port, std::shared_ptr<SlaveNode>* slave_node);

  slash::Mutex partition_mu_;
  std::vector<std::shared_ptr<SlaveNode>> slaves_;

  slash::Mutex session_mu_;
  int32_t session_id_;

  // BinlogCacheWindow win_;
};

class SyncSlavePartition : public SyncPartition {
 public:
  static const std::vector<ReplState> NEEDS_CHECK_SYNC_TIMEOUT_STATES;
  static bool NeedsCheckSyncTimeout(const ReplState& current) {
    return std::any_of(NEEDS_CHECK_SYNC_TIMEOUT_STATES.begin(), NEEDS_CHECK_SYNC_TIMEOUT_STATES.end(),[current](const ReplState& s) { return s == current; });
  }

  SyncSlavePartition(const std::string& table_name, uint32_t partition_id);
  Status InitMasterTerm();

 public:
  Status Activate(const RmNode& master, const ReplState& repl_state, const std::string& info_file_path);
  void Deactivate();

  void SetLastRecvTime(uint64_t time) {
    slash::RWLock l(&partition_mu_, true);
    m_info_.SetLastRecvTime(time);
  }
  uint64_t LastRecvTime() {
    slash::RWLock l(&partition_mu_, false);
    return m_info_.LastRecvTime();
  }

  void SetReplState(const ReplState& repl_state);
  Status CASReplState(const ReplState& exp_state,
                      uint32_t exp_master_term,
                      const ReplState& new_state,
                      const std::string& reason);
  Status CASReplState(const std::vector<ReplState>& allowed_states,
                      uint32_t exp_master_term,
                      const std::function<Status()>& action,
                      const ReplState& new_state,
                      const std::string& reason);

  Status CASStateCheckFailed(const std::vector<ReplState>& exps, const ReplState& new_state) {
    std::stringstream ss;
    ss << "CAS partition" << this->partition_info_.ToString() << " "
       <<"state to '" << ReplStateMsg[new_state] << "' state check failed, expected states: ";
    if (!exps.empty()) {
      ss << "'" << ReplStateMsg[exps[0]] << "'";
    }
    for (size_t i = 1; i < exps.size(); i++) {
      ss << " or ";
      ss << "'" << ReplStateMsg[exps[i]] << "'";
    }
    ss << ", but current state is '" << ReplStateMsg[repl_state_] << "'";
    return Status::Incomplete(ss.str());
  }
  Status CASTermCheckFailed(uint32_t exp_term, const std::vector<ReplState>& exp_states, const ReplState& new_state) {
    std::stringstream ss;
    ss << "CAS partition" << this->partition_info_.ToString() << " "
       << "state to '" << ReplStateMsg[new_state] << "' term check failed, "
       << "expected term " << exp_term << ", "
       << "but current term is " << m_term_ << ", "
       << "expected state ";
    if (!exp_states.empty()) {
      ss << "'" << ReplStateMsg[exp_states[0]] << "'";
    }
    for (size_t i = 1; i < exp_states.size(); i++) {
      ss << " or ";
      ss << "'" << ReplStateMsg[exp_states[i]] << "'";
    }
    return Status::Incomplete(ss.str());
  }
  bool matchStates(const std::vector<ReplState>& allowed_current_states);
  void SetReplStateUnsafe(const ReplState& repl_state);
  ReplState State() {
    slash::RWLock l(&partition_mu_, false);
    return repl_state_;
  }
  ReplState StateUnsafe() { return repl_state_; }

  Status ResetMasterUnsafe(const std::string& info_file_path, const std::string& reason);
  Status SetMasterUnsafe(const RmNode& newMaster, const std::string& info_file_path, const std::string& reason);
  Status CheckSyncTimeout(uint64_t now);
  Status ResetReplication(uint32_t master_term, const std::string &reason);

  // For display
  Status GetInfo(std::string* info);
  // For debug
  std::string ToStringStatus();

  const std::string& MasterIp() {
    slash::RWLock l(&partition_mu_, false);
    return m_info_.Ip();
  }
  const std::string& MasterIpUnsafe() { return m_info_.Ip(); }
  int MasterPort() {
    slash::RWLock l(&partition_mu_, false);
    return m_info_.Port();
  }
  int MasterPortUnsafe() { return m_info_.Port(); }
  std::string MasterAddr() {
    slash::RWLock l(&partition_mu_, false);
    return m_info_.Ip() + ":" + std::to_string(m_info_.Port());
  }
  void SetMasterSessionId(int32_t session_id) {
    slash::RWLock l(&partition_mu_, true);
    m_info_.SetSessionId(session_id);
  }
  int32_t MasterSessionId() {
    slash::RWLock l(&partition_mu_, false);
    return m_info_.SessionId();
  }
  void SetLocalIp(const std::string& local_ip) {
    slash::RWLock l(&partition_mu_, true);
    local_ip_ = local_ip;
  }
  std::string LocalIp() {
    slash::RWLock l(&partition_mu_, false);
    return local_ip_;
  }
  void SetResharding(bool resharding) {
    slash::RWLock l(&partition_mu_, true);
    this->resharding_ = resharding;
  }
  bool Resharding() {
    slash::RWLock l(&partition_mu_, false);
    return resharding_;
  }
  uint32_t MasterTerm() {
    slash::RWLock l(&partition_mu_, false);
    return m_term_;
  }
 private:
  Status GetInfoFilePath(std::string *info_file_path);

 private:
  pthread_rwlock_t partition_mu_;
  RmNode m_info_;
  uint32_t m_term_;
  ReplState repl_state_;
  std::string local_ip_;
  bool resharding_;
};

class BinlogReaderManager {
 public:
  ~BinlogReaderManager();
  Status FetchBinlogReader(const RmNode& rm_node, std::shared_ptr<PikaBinlogReader>* reader);
  Status ReleaseBinlogReader(const RmNode& rm_node);
  std::string ToStringStatus();
 private:
  slash::Mutex reader_mu_;
  std::unordered_map<RmNode, std::shared_ptr<PikaBinlogReader>, hash_rm_node> occupied_;
  std::vector<std::shared_ptr<PikaBinlogReader>> vacant_;
};

class PikaReplicaManager {
 public:
  PikaReplicaManager();
  ~PikaReplicaManager();

  void Start();
  void Stop();

  Status InitSlaveSyncPartitionsMasterTerm();
  Status AddSyncPartitionSanityCheck(const std::set<PartitionInfo>& p_infos);
  Status AddSyncPartition(const std::set<PartitionInfo>& p_infos);
  Status RemoveSyncPartitionSanityCheck(const std::set<PartitionInfo>& p_infos);
  Status RemoveSyncPartition(const std::set<PartitionInfo>& p_infos);
  Status SelectLocalIp(const std::string& remote_ip,
                       const int remote_port,
                       std::string* const local_ip);
  Status ActivateSyncSlavePartition(const RmNode& node, const ReplState& repl_state, bool resharding=false);
  Status UpdateSyncSlavePartitionSessionId(const PartitionInfo& p_info, int32_t session_id);
  Status DeactivateSyncSlavePartition(const PartitionInfo& p_info);
  Status SetSlaveReplState(const PartitionInfo& p_info, const ReplState& repl_state);
  Status CASSlaveReplState(const PartitionInfo& p_info,
                           const ReplState& current_state, uint32_t current_term,
                           const ReplState& new_state, const std::string& reason);
  Status GetSlaveReplState(const PartitionInfo& p_info, ReplState* repl_state);

  // For Pika Repl Client Thread
  Status SendMetaSyncRequest();
  Status SendRemoveSlaveNodeRequest(const std::string& table, uint32_t partition_id);
  Status SendPartitionTrySyncRequest(const std::string& table_name, size_t partition_id);
  Status SendPartitionDBSyncRequest(const std::string& table_name, size_t partition_id);
  Status SendPartitionBinlogSyncAckRequest(const std::string& table, uint32_t partition_id,
                                           const BinlogOffset& ack_start, const BinlogOffset& ack_end,
                                           bool is_first_send = false);
  Status CloseReplClientConn(const std::string& ip, int32_t port);

  // For Pika Repl Server Thread
  Status SendSlaveBinlogChipsRequest(const std::string& ip, int port, const std::vector<WriteTask>& tasks);

  // For SyncMasterPartition
  std::shared_ptr<SyncMasterPartition> GetSyncMasterPartitionByName(const PartitionInfo& p_info);
  Status GetSafetyPurgeBinlogFromSMP(const std::string& table_name,
                                     uint32_t partition_id, std::string* safety_purge);
  bool BinlogCloudPurgeFromSMP(const std::string& table_name,
                               uint32_t partition_id, uint32_t index);

  // For SyncSlavePartition
  std::shared_ptr<SyncSlavePartition> GetSyncSlavePartitionByName(const PartitionInfo& p_info);


  Status RunSyncSlavePartitionStateMachine();

  Status SetMasterLastRecvTime(const RmNode& slave, uint64_t time);
  Status SetSlaveLastRecvTime(const RmNode& slave, uint64_t time);

  Status CheckSyncTimeout(uint64_t now);

  // To check partition info
  // For pkcluster info command
  Status GetPartitionInfo(
      const std::string& table, uint32_t partition_id, std::string* info);

  void FindCompleteReplica(std::vector<std::string>* replica);
  void FindCommonMaster(std::string* master);

  Status CheckPartitionRole(
      const std::string& table, uint32_t partition_id, int* role);

  void RmStatus(std::string* debug_info);

  // following funcs invoked by master partition only

  Status AddPartitionSlave(const RmNode& slave, uint32_t master_term);
  Status RemovePartitionSlave(const RmNode& slave);
  bool CheckPartitionSlaveExist(const RmNode& slave);
  bool CheckSlaveDBConnect();
  Status GetPartitionSlaveSession(const RmNode& slave, int32_t* session);

  Status LostConnection(const std::string& ip, int port);

  Status ActivateBinlogSync(const RmNode& slave, const BinlogOffset& offset);
  Status ActivateDbSync(const RmNode& slave);

  // Update binlog win and try to send next binlog
  Status UpdateSyncBinlogStatus(const RmNode& slave, const BinlogOffset& offset_start, const BinlogOffset& offset_end);
  Status GetSyncBinlogStatus(const RmNode& slave, BinlogOffset* sent_boffset, BinlogOffset* acked_boffset);
  Status GetSyncMasterPartitionSlaveState(const RmNode& slave, SlaveState* const slave_state);

  Status WakeUpBinlogSync();

  // Session Id
  int32_t GenPartitionSessionId(const std::string& table_name, uint32_t partition_id);
  int32_t GetSlavePartitionSessionId(const std::string& table_name, uint32_t partition_id);
  bool CheckSlavePartitionSessionId(const std::string& table_name, uint32_t partition_id,
                                    int session_id);
  bool CheckMasterPartitionSessionId(const std::string& ip, int port,
                                     const std::string& table_name,
                                     uint32_t partition_id, int session_id);

  // write_queue related
  void ProduceWriteQueue(const std::string& ip, int port, const std::vector<WriteTask>& tasks);
  int ConsumeWriteQueue();
  void DropItemInWriteQueue(const std::string& ip, int port);

  // Schedule Task
  void ScheduleReplServerBGTask(pink::TaskFunc func, void* arg);
  void ScheduleReplClientBGTask(pink::TaskFunc func, void* arg);
  void ScheduleWriteBinlogTask(const std::string& table_partition,
                               const std::shared_ptr<InnerMessage::InnerResponse> res,
                               std::shared_ptr<pink::PbConn> conn, void* res_private_data);
  void ScheduleWriteDBTask(const std::string& dispatch_key,
                           PikaCmdArgsType* argv, BinlogItem* binlog_item,
                           const std::string& table_name, uint32_t partition_id);

  void ReplServerRemoveClientConn(int fd);
  void ReplServerUpdateClientConnMap(const std::string& ip_port, int fd);

  BinlogReaderManager binlog_reader_mgr;

 private:
  void InitPartition();
  std::shared_ptr<SyncMasterPartition> getSyncMasterPartitionByNameLocked(const PartitionInfo& p_info);

  pthread_rwlock_t partitions_rw_;
  std::unordered_map<PartitionInfo, std::shared_ptr<SyncMasterPartition>, hash_partition_info> sync_master_partitions_;
  std::unordered_map<PartitionInfo, std::shared_ptr<SyncSlavePartition>, hash_partition_info> sync_slave_partitions_;

  slash::Mutex  write_queue_mu_;
  // every host owns a queue
  std::unordered_map<std::string, std::queue<WriteTask>> write_queues_;  // ip+port, queue<WriteTask>

  PikaReplClient* pika_repl_client_;
  PikaReplServer* pika_repl_server_;
  int last_meta_sync_timestamp_;
};

#endif  //  PIKA_RM_H
