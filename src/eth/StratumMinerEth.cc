/*
 The MIT License (MIT)

 Copyright (c) [2016] [BTC.COM]

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
 */
#include "StratumMinerEth.h"

#include "StratumSessionEth.h"
#include "StratumServerEth.h"
#include "DiffController.h"

#include "CommonEth.h"

///////////////////////////////// StratumSessionEth ////////////////////////////////
StratumMinerEth::StratumMinerEth(StratumSessionEth &session,
                                 const DiffController &diffController,
                                 const std::string &clientAgent,
                                 const std::string &workerName,
                                 int64_t workerId,
                                 StratumProtocolEth ethProtocol)
    : StratumMinerBase(session, diffController, clientAgent, workerName, workerId), ethProtocol_(ethProtocol) {
}

void StratumMinerEth::handleRequest(const std::string &idStr,
                                    const std::string &method,
                                    const JsonNode &jparams,
                                    const JsonNode &jroot) {
  if (method == "eth_getWork") {
    handleRequest_GetWork(idStr, jparams);
  } else if (method == "eth_submitHashrate") {
    handleRequest_SubmitHashrate(idStr, jparams);
  } else if (method == "mining.submit" || method == "eth_submitWork") {
    handleRequest_Submit(idStr, jparams);
  }
}

void StratumMinerEth::handleRequest_GetWork(const string &idStr, const JsonNode &jparams) {
  getSession().sendMiningNotifyWithId(getSession().getServer().GetJobRepository(getSession().getChainId())->getLatestStratumJobEx(), idStr);
}

void StratumMinerEth::handleRequest_SubmitHashrate(const string &idStr, const JsonNode &jparams) {
  getSession().responseTrue(idStr);
}

void StratumMinerEth::handleRequest_Submit(const string &idStr, const JsonNode &jparams) {

  auto &session = getSession();
  if (session.getState() != StratumSession::AUTHENTICATED) {
    session.responseError(idStr, StratumStatus::UNAUTHORIZED);

    // there must be something wrong, send reconnect command
    const string s = "{\"id\":null,\"method\":\"client.reconnect\",\"params\":[]}\n";
    session.sendData(s);
    return;
  }

  //etherminer (STRATUM)
  // {"id": 4, "method": "mining.submit",
  // "params": ["0x7b9d694c26a210b9f0d35bb9bfdd70a413351111.fatrat1117",
  // "ae778d304393d441bf8e1c47237261675caa3827997f671d8e5ec3bd5d862503",
  // "0x4cc7c01bfbe51c67",
  // "0xae778d304393d441bf8e1c47237261675caa3827997f671d8e5ec3bd5d862503",
  // "0x52fdd9e9a796903c6b88af4192717e77d9a9c6fa6a1366540b65e6bcfa9069aa"]}

  //Claymore (ETHPROXY)
  //{"id":4,"method":"eth_submitWork",
  //"params":["0x17a0eae8082fb64c","0x94a789fba387d454312db3287f8440f841de762522da8ba620b7fcf34a80330c",
  //"0x2cc7dad9f2f92519891a2d5f67378e646571b89e5994fe9290d6d669e480fdff"]}

  //NICEHASH_STRATUM
  // {"id": 244,
  //  "method": "mining.submit",
  //  "params": [ "username", "bf0488aa", "6a909d9bbc0f" ]
  // }
  //Note in above example that minernonce is 6 bytes, because provided extranonce was 2 bytes.
  //If pool provides 3 bytes extranonce, then minernonce must be 5 bytes.
  auto params = (const_cast<JsonNode &>(jparams)).array();

  if (StratumProtocolEth::STRATUM == ethProtocol_ && params.size() < 5) {
    session.responseError(idStr, StratumStatus::ILLEGAL_PARARMS);
    return;
  } else if (StratumProtocolEth::ETHPROXY == ethProtocol_ && params.size() < 3) {
    session.responseError(idStr, StratumStatus::ILLEGAL_PARARMS);
    return;
  } else if (StratumProtocolEth::NICEHASH_STRATUM == ethProtocol_ && params.size() < 3) {
    session.responseError(idStr, StratumStatus::ILLEGAL_PARARMS);
    return;
  }

  string jobId, sNonce, sHeader;
  switch (ethProtocol_) {
  case StratumProtocolEth::STRATUM: {
    jobId = params[1].str();
    sNonce = params[2].str();
    sHeader = params[3].str();
  }
    break;
  case StratumProtocolEth::ETHPROXY: {
    sNonce = params[0].str();
    sHeader = params[1].str();
    jobId = sHeader;
  }
    break;
  case StratumProtocolEth::NICEHASH_STRATUM: {
    jobId = params[1].str();
    sNonce = params[2].str();
    sHeader = jobId;
  }
    break;
  }

  // Claymore's jobId starting with "0x"
  // Remove it here to avoid compatibility issues with Claymore or other miners
  if (jobId.size() >= 66) {
    jobId = jobId.substr(2, 64);
  }

  DLOG(INFO) << "submit: " << jobId << ", " << sNonce << ", " << sHeader;

  auto localJob = session.findLocalJob(jobId);
  // can't find local job
  if (localJob == nullptr) {
    session.responseFalse(idStr, StratumStatus::JOB_NOT_FOUND);
    return;
  }

  // can't find stratum job
  auto &server = session.getServer();
  auto &worker = session.getWorker();
  auto extraNonce1 = session.getSessionId();

  shared_ptr<StratumJobEx> exjob = server.GetJobRepository(localJob->chainId_)->getStratumJobEx(localJob->jobId_);
  if (exjob.get() == nullptr) {
    session.responseFalse(idStr, StratumStatus::JOB_NOT_FOUND);
    return;
  }
  auto sjob = std::static_pointer_cast<StratumJobEth>(exjob->sjob_);

  if (StratumProtocolEth::NICEHASH_STRATUM == ethProtocol_) {
    if (sNonce.size() != 16) {
      string noncePrefix = Strings::Format("%06x", extraNonce1);
      if (isNiceHashClient_) {
        noncePrefix = noncePrefix.substr(0, 4);
      }
      sNonce = noncePrefix + sNonce;
    }
  }

  uint64_t nonce = stoull(sNonce, nullptr, 16);
  uint32_t height = sjob->height_;
  uint64_t networkDiff = Eth_TargetToDifficulty(sjob->networkTarget_.GetHex());
  // Used to prevent duplicate shares. (sHeader has a prefix "0x")
  uint64_t headerPrefix = stoull(sHeader.substr(2, 16), nullptr, 16);
  EthConsensus::Chain chain = sjob->chain_;

  auto iter = jobDiffs_.find(localJob);
  if (iter == jobDiffs_.end()) {
    LOG(ERROR) << "can't find session's diff, worker: " << worker.fullName_;
    return;
  }
  auto &jobDiff = iter->second;

  ShareEth share;
  share.set_version(ShareEth::getVersion(chain));
  share.set_headerhash(headerPrefix);
  share.set_workerhashid(workerId_);
  share.set_userid(worker.userId(exjob->chainId_));
  share.set_sharediff(jobDiff.currentJobDiff_);
  share.set_networkdiff(networkDiff);
  share.set_timestamp((uint64_t) time(nullptr));
  share.set_status(StratumStatus::REJECT_NO_REASON);
  share.set_height(height);
  share.set_nonce(nonce);
  share.set_sessionid(extraNonce1); // TODO: fix it, set as real session id.
  IpAddress ip;
  ip.fromIpv4Int(session.getClientIp());
  share.set_ip(ip.toString());

  LocalShare localShare(nonce, 0, 0);
  // can't add local share
  if (!localJob->addLocalShare(localShare)) {
    session.responseFalse(idStr, StratumStatus::DUPLICATE_SHARE);
    // add invalid share to counter
    invalidSharesCounter_.insert((int64_t) time(nullptr), 1);
    return;
  }


  // The mixHash is used to submit the work to the Ethereum node.
  // We don't need to pay attention to whether the mixHash submitted
  // by the miner is correct, because we recalculated it.
  // SolvedShare will be accepted correctly by the ETH node if
  // the difficulty is reached in our calculations.
  uint256 shareMixHash;
  share.set_status(server.checkShareAndUpdateDiff(
    localJob->chainId_,
    share, localJob->jobId_, nonce, uint256S(sHeader),
    jobDiff.jobDiffs_, shareMixHash, worker.fullName_
  ));

  if (StratumStatus::isAccepted(share.status())) {
    DLOG(INFO) << "share reached the diff: " << share.sharediff();
  } else {
    DLOG(INFO) << "share not reached the diff: " << share.sharediff();
  }

  // we send share to kafka by default, but if there are lots of invalid
  // shares in a short time, we just drop them.
  if (handleShare(idStr, share.status(), share.sharediff())) {
    if (StratumStatus::isSolved(share.status())) {
      server.sendSolvedShare2Kafka(localJob->chainId_, sNonce, sHeader, shareMixHash.GetHex(), height, networkDiff, worker, chain);
      // mark jobs as stale
      server.GetJobRepository(exjob->chainId_)->markAllJobsAsStale();
    }
  } else {
    // check if there is invalid share spamming
    int64_t invalidSharesNum = invalidSharesCounter_.sum(time(nullptr), INVALID_SHARE_SLIDING_WINDOWS_SIZE);
    // too much invalid shares, don't send them to kafka
    if (invalidSharesNum >= INVALID_SHARE_SLIDING_WINDOWS_MAX_LIMIT) {
      LOG(WARNING) << "invalid share spamming, worker: " << worker.fullName_
                   << ", " << share.toString();
      return;
    }
  }

  DLOG(INFO) << share.toString();

  std::string message;
  uint32_t size = 0;
  if (!share.SerializeToArrayWithVersion(message, size)) {
    LOG(ERROR) << "share SerializeToBuffer failed!"<< share.toString();
    return;
  }

  server.sendShare2Kafka(localJob->chainId_, message.data(), size);
}
