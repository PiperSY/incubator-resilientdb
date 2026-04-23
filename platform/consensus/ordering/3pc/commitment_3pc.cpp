#include "platform/consensus/ordering/three_pc/commitment_3pc.h"

namespace resdb {

int Commitment3PC::ProcessNewRequest(std::unique_ptr<Context> context,
                                     std::unique_ptr<Request> user_request) {
  // Keep the existing PBFT ingress checks exactly the same.
  if (context == nullptr || context->signature.signature().empty()) {
    LOG(ERROR) << "user request doesn't contain signature, reject";
    return -2;
  }

  if (uint64_t seq = duplicate_manager_->CheckIfExecuted(user_request->hash())) {
    LOG(ERROR) << "This request is already executed with seq: " << seq;
    user_request->set_seq(seq);
    message_manager_->SendResponse(std::move(user_request));
    return -2;
  }

  // Under the assignment assumptions, the current primary is the single 3PC
  // coordinator. Non-coordinator replicas forward the request there.
  if (!IsCoordinator()) {
    LOG(INFO) << "NOT COORDINATOR, Coordinator is " << CoordinatorId();
    replica_communicator_->SendMessage(*user_request, CoordinatorId());
    {
      std::lock_guard<std::mutex> lk(rc_mutex_);
      request_complained_.push(
          std::make_pair(std::move(context), std::move(user_request)));
    }
    return -3;
  }

  // Verify the client payload signature before entering 3PC.
  bool valid = verifier_->VerifyMessage(user_request->data(),
                                        user_request->data_signature());
  if (!valid) {
    LOG(ERROR) << "request is not valid: "
               << user_request->data_signature().DebugString();
    LOG(ERROR) << "msg size: " << user_request->data().size();
    return -2;
  }

  if (pre_verify_func_ && !pre_verify_func_(*user_request)) {
    LOG(ERROR) << "check by the user func fail";
    return -2;
  }

  global_stats_->IncClientRequest();

  // Reuse the proposed-request duplicate suppression from PBFT.
  if (duplicate_manager_->CheckAndAddProposed(user_request->hash())) {
    LOG(INFO) << "request is already proposed, reject";
    return -2;
  }

  auto seq = message_manager_->AssignNextSeq();
  if (!seq.ok()) {
    LOG(ERROR) << "AssignNextSeq() failed";
    duplicate_manager_->EraseProposed(user_request->hash());
    global_stats_->SeqFail();

    Request response;
    response.set_type(Request::TYPE_RESPONSE);
    response.set_sender_id(config_.GetSelfInfo().id());
    response.set_proxy_id(user_request->proxy_id());
    response.set_ret(-2);
    response.set_hash(user_request->hash());
    replica_communicator_->SendMessage(response, response.proxy_id());
    return -2;
  }

  // Coordinator-side 3PC bookkeeping.
  {
    std::lock_guard<std::mutex> lk(txn_mu_);
    ThreePCTxnState& txn = txn_state_[*seq];
    txn.seq = *seq;
    txn.phase = ThreePCPhase::kReady;  // PREPARE sent, waiting on votes
    txn.hash = user_request->hash();
    txn.coordinator_id = config_.GetSelfInfo().id();
    txn.proxy_id = user_request->proxy_id();
    txn.original_request = *user_request;

    // Coordinator implicitly votes commit for its own local participant.
    txn.vote_commit_from.insert(config_.GetSelfInfo().id());
  }

  global_stats_->RecordStateTime("3pc_prepare");

  // Replace PBFT PRE_PREPARE with a 3PC PREPARE.
  user_request->set_type(Request::TYPE_3PC_PREPARE);
  user_request->set_current_view(message_manager_->GetCurrentView());
  user_request->set_seq(*seq);
  user_request->set_sender_id(config_.GetSelfInfo().id());
  user_request->set_primary_id(config_.GetSelfInfo().id());

  // Preserve:
  //   - data()            original batched client payload
  //   - data_signature()  client signature
  //   - hash()            duplicate tracking key
  //   - proxy_id()        return path to proxy/client

  replica_communicator_->BroadCast(*user_request);

  // This will be a no-op here unless you later change the coordinator to count
  // self + already-buffered votes. It is still useful because the exact same
  // helper is called again from the vote-commit handler.
  return MaybeBroadcastPreCommit(*seq);
}

int Commitment3PC::RecordVoteCommit(uint64_t seq, uint32_t voter_id) {
  {
    std::lock_guard<std::mutex> lk(txn_mu_);
    auto it = txn_state_.find(seq);
    if (it == txn_state_.end()) {
      LOG(ERROR) << "unknown 3PC txn seq: " << seq;
      return -2;
    }

    ThreePCTxnState& txn = it->second;
    if (txn.phase != ThreePCPhase::kReady) {
      // Late/duplicate vote or already advanced.
      return 0;
    }

    txn.vote_commit_from.insert(voter_id);
  }

  return MaybeBroadcastPreCommit(seq);
}

int Commitment3PC::MaybeBroadcastPreCommit(uint64_t seq) {
  std::unique_ptr<Request> precommit;

  {
    std::lock_guard<std::mutex> lk(txn_mu_);
    auto it = txn_state_.find(seq);
    if (it == txn_state_.end()) {
      return -2;
    }

    ThreePCTxnState& txn = it->second;
    if (txn.phase != ThreePCPhase::kReady) {
      return 0;
    }

    // Assignment assumption: all 4 replicas participate.
    // That means the coordinator waits for a commit vote from every replica,
    // including its own implicit local vote.
    const size_t expected_votes = static_cast<size_t>(config_.GetReplicaNum());
    if (txn.vote_commit_from.size() < expected_votes) {
      return 0;
    }

    txn.phase = ThreePCPhase::kPreCommit;

    precommit = std::make_unique<Request>();
    precommit->set_type(Request::TYPE_3PC_PRECOMMIT);
    precommit->set_seq(txn.seq);
    precommit->set_hash(txn.hash);
    precommit->set_proxy_id(txn.proxy_id);
    precommit->set_sender_id(config_.GetSelfInfo().id());
    precommit->set_primary_id(config_.GetSelfInfo().id());
    precommit->set_current_view(message_manager_->GetCurrentView());
  }

  global_stats_->RecordStateTime("3pc_precommit");
  replica_communicator_->BroadCast(*precommit);
  return 0;
}

}  // namespace resdb