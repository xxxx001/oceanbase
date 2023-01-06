/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

int create_ls(const share::ObLSID &ls_id,
              ObLS &ls,
              ObITxLogParam *param,
              ObITxLogAdapter * log_adapter);
int remove_ls(const share::ObLSID &ls_id,
              const bool graceful = true);
/*
 * acquire a transaction descriptor by deserialize from buffer
 */
int acquire_tx(const char* buf, const int64_t len, int64_t &pos, ObTxDesc *&tx);
int release_tx_ref(ObTxDesc &tx);
/*
 * interrupt any work in progress thread
 */
int interrupt(ObTxDesc &tx, int cause);

/*
 * create an implicit savepoint when txn is active
 */
int create_in_txn_implicit_savepoint(ObTxDesc &tx, int64_t &savepoint);

/*
 * prepare a transaction
 * if transaction not started return READ_ONLY
 * and commit phase should not be called
 */
int prepare_tx(ObTxDesc &tx_desc,
               const int64_t timeout_us,
               ObITxCallback &cb);
/*
 * commit or abort a prepared transaction
 */
int end_two_phase_tx(const ObTransID &tx_id,
                     const ObXATransID &xid,
                     const share::ObLSID &ls_id,
                     const int64_t timeout_us,
                     const bool is_rollback,
                     ObITxCallback &cb,
                     ObTxDesc *&tx_desc);

/*
 * acquire transaction's coordinator
 * return READ_ONLY if transaction not contain any participants
 */
int prepare_tx_coord(ObTxDesc &tx_desc,
                     share::ObLSID &coord_id);

/*
 * collect transaction participant info after transactional data access
 */
int collect_tx_exec_result(ObTxDesc &tx,
                           ObTxExecResult &result);
/*********************************************************************
 *
 * get_store_ctx / revert_store_ctx
 *
 * pre-hook in Data Access to prepare transaction relative work
 *
 ********************************************************************/
int get_read_store_ctx(const ObTxReadSnapshot &snapshot,
                       const bool read_latest,
                       const int64_t lock_timeout,
                       ObStoreCtx &store_ctx);
int get_read_store_ctx(const share::SCN snapshot_version,
                       const int64_t lock_timeout,
                       ObStoreCtx &store_ctx);
int get_write_store_ctx(ObTxDesc &tx,
                        const ObTxReadSnapshot &snapshot,
                        const concurrent_control::ObWriteFlag write_flag,
                        storage::ObStoreCtx &store_ctx);
int revert_store_ctx(storage::ObStoreCtx &store_ctx);

int acquire_tx_ctx(const share::ObLSID &ls_id,
                   const ObTxDesc &tx,
                   ObPartTransCtx *&ctx,
                   ObLS *ls);
//handle msg
int handle_trans_commit_request(ObTxCommitMsg &commit_req, obrpc::ObTransRpcResult &result);
int handle_trans_commit_response(ObTxCommitRespMsg &commit_resp, obrpc::ObTransRpcResult &result);
int handle_trans_abort_request(ObTxAbortMsg &abort_req, obrpc::ObTransRpcResult &result);
int handle_sp_rollback_request(ObTxRollbackSPMsg &sp_rollbacck_req, obrpc::ObTxRpcRollbackSPResult &result);
int handle_sp_rollback_resp(const share::ObLSID &ls_id,
                            const int64_t epoch,
                            const transaction::ObTransID &tx_id,
                            const int status,
                            const ObAddr &addr,
                            const int64_t request_id,
                            const obrpc::ObTxRpcRollbackSPResult &result);
int handle_trans_msg_callback(const share::ObLSID &sender_ls_id,
                              const share::ObLSID &receiver_ls_id,
                              const ObTransID &tx_id,
                              const int16_t msg_type,
                              const int status,
                              const ObAddr &receiver_addr,
                              const int64_t request_id,
                              const share::SCN &private_data);
int handle_trans_keepalive(const ObTxKeepaliveMsg &msg, obrpc::ObTransRpcResult &result);
int handle_trans_keepalive_response(const ObTxKeepaliveRespMsg &msg, obrpc::ObTransRpcResult &result);
int handle_tx_batch_req(int type, const char* buf, int32_t size, const bool need_check_leader = true);
int refresh_location_cache(const share::ObLSID ls);
int handle_tx_commit_timeout(ObTxDesc &tx, const int64_t delay);
int handle_tx_commit_result(const ObTransID &tx_id,
                            const int result,
                            const share::SCN commit_version = share::SCN());

ObTxCtxMgr &get_tx_ctx_mgr() { return tx_ctx_mgr_; }

int get_min_uncommit_tx_prepare_version(const share::ObLSID& ls_id, share::SCN &min_prepare_version);

int kill_all_tx(const share::ObLSID &ls_id, const KillTransArg &arg,
    bool &is_all_tx_cleaned_up);

int block_ls(const share::ObLSID &ls_id, bool &is_all_tx_cleaned_up);

int iterate_ls_id(ObLSIDIterator &ls_id_iter);

int iterate_tx_ctx_mgr_stat(ObTxCtxMgrStatIterator &tx_ctx_mgr_stat_iter);

int iterate_tx_lock_stat(const share::ObLSID& ls_id,
    ObTxLockStatIterator &tx_lock_stat_iter);

int iterate_all_observer_tx_stat(ObTxStatIterator &tx_stat_iter);

// for xa
/*
 * recover transaction descriptor with tx info
 */
int recover_tx(const ObTxInfo &tx_info, ObTxDesc *&tx);
int get_tx_info(ObTxDesc &tx, ObTxInfo &tx_info);
int get_tx_stmt_info(ObTxDesc &tx, ObTxStmtInfo &stmt_info);
int update_tx_with_stmt_info(const ObTxStmtInfo &tx_info, ObTxDesc *&tx);
int handle_timeout_for_xa(ObTxDesc &tx, const int64_t delay);
int handle_sub_prepare_request(const ObTxSubPrepareMsg &msg, obrpc::ObTransRpcResult &result);
int handle_sub_prepare_response(const ObTxSubPrepareRespMsg &msg, obrpc::ObTransRpcResult &result);
int handle_sub_prepare_result(const ObTransID &tx_id, const int result);
int handle_sub_commit_request(const ObTxSubCommitMsg &msg, obrpc::ObTransRpcResult &result);
int handle_sub_commit_response(const ObTxSubCommitRespMsg &msg, obrpc::ObTransRpcResult &result);
int handle_sub_commit_result(const ObTransID &tx_id, const int result);
int handle_sub_rollback_request(const ObTxSubRollbackMsg &msg, obrpc::ObTransRpcResult &result);
int handle_sub_rollback_response(const ObTxSubRollbackRespMsg &msg, obrpc::ObTransRpcResult &result);
int handle_sub_rollback_result(const ObTransID &tx_id, const int result);
int check_scheduler_status(const share::ObLSID &ls_id);

TO_STRING_KV(K(is_inited_), K(tenant_id_), KP(this));

private:
int check_ls_status_(const share::ObLSID &ls_id, bool &leader);
void init_tx_(ObTxDesc &tx, const uint32_t session_id);
int start_tx_(ObTxDesc &tx);
int abort_tx_(ObTxDesc &tx, const int cause, bool cleanup = true);
int finalize_tx_(ObTxDesc &tx);
int find_parts_after_sp_(ObTxDesc &tx,
                         ObTxPartRefList &parts,
                         const int64_t scn);
int rollback_savepoint_(ObTxDesc &tx,
                        ObTxPartRefList &parts,
                        const int64_t savepoint,
                        int64_t expire_ts);
int rollback_savepoint_slowpath_(ObTxDesc &tx,
                                 const ObTxPartRefList &parts,
                                 const int64_t scn,
                                 const int64_t expire_ts);
int create_tx_ctx_(const share::ObLSID &ls_id,
                   const ObTxDesc &tx,
                   ObPartTransCtx *&ctx);

int create_tx_ctx_(const share::ObLSID &ls_id,
                   ObLS *ls,
                   const ObTxDesc &tx,
                   ObPartTransCtx *&ctx);

int get_tx_ctx_(const share::ObLSID &ls_id,
                ObLS *ls,
                const ObTransID &tx_id,
                ObPartTransCtx *&ctx);

int get_tx_ctx_(const share::ObLSID &ls_id,
                const ObTransID &tx_id,
                ObPartTransCtx *&ctx);

int revert_tx_ctx_(ObLS *ls, ObPartTransCtx *ctx);
int revert_tx_ctx_(ObPartTransCtx *ctx);
int validate_snapshot_version_(const share::SCN snapshot,
                               const int64_t expire_ts,
                               ObLS &ls);
int check_replica_readable_(const share::SCN snapshot,
                            const bool elr,
                            const ObTxReadSnapshot::SRC src,
                            const share::ObLSID &ls_id,
                            const int64_t expired_ts,
                            ObLS &ls);
int build_tx_commit_msg_(const ObTxDesc &tx, ObTxCommitMsg &msg);
int abort_participants_(const ObTxDesc &tx_desc);
int acquire_local_snapshot_(const share::ObLSID &ls_id, share::SCN &snapshot);
int sync_acquire_global_snapshot_(ObTxDesc &tx,
                                  const int64_t expire_ts,
                                  share::SCN &snapshot,
                                  int64_t &uncertain_bound);
int acquire_global_snapshot__(const int64_t expire_ts,
                              const int64_t gts_ahead,
                              share::SCN &snapshot,
                              int64_t &uncertain_bound,
                              ObFunction<bool()> interrupt_checker);
int batch_post_tx_msg_(ObTxRollbackSPMsg &msg, const ObIArray<ObTxLSEpochPair> &pl);
int post_tx_commit_msg_(ObTxDesc &tx_desc,
                        ObTxCommitMsg &msg,
                        ObITxCallback *cb);
int post_tx_abort_part_msg_(const ObTxDesc &tx_desc,
                            const ObTxPart &p);
bool is_sync_replica_(const share::ObLSID &ls_id);

void handle_orphan_2pc_msg_(const ObTxMsg &msg, const bool need_check_leader);

int update_max_read_ts_(const uint64_t tenant_id,
                        const share::ObLSID &lsid,
                        const share::SCN ts);
int do_commit_tx_(ObTxDesc &tx,
                  const int64_t expire_ts,
                  ObITxCallback &cb,
                  share::SCN &commit_version);
int do_commit_tx_slowpath_(ObTxDesc &tx, const int64_t expire_ts);
int register_commit_retry_task_(ObTxDesc &tx, const int64_t max_delay = INT64_MAX);
int unregister_commit_retry_task_(ObTxDesc &tx);
int handle_tx_commit_result_(ObTxDesc &tx,
                             const int result,
                             const share::SCN commit_version = share::SCN());
int decide_tx_commit_info_(ObTxDesc &tx, ObTxPart *&coord);
int local_ls_commit_tx_(const ObTransID &tx_id,
                        const share::ObLSID &coord,
                        const share::ObLSArray &parts,
                        const int64_t &expire_ts,
                        const common::ObString &app_trace_info,
                        const int64_t &request_id,
                        share::SCN &commit_version);
int get_tx_state_from_tx_table_(const share::ObLSID &lsid,
                                const ObTransID &tx_id,
                                int &state,
                                share::SCN &commit_version);
int gen_trans_id_(ObTransID &trans_id);
bool commit_need_retry_(const int ret);
// for xa
int build_tx_sub_prepare_msg_(const ObTxDesc &tx, ObTxSubPrepareMsg &msg);
int build_tx_sub_commit_msg_(const ObTxDesc &tx, ObTxSubCommitMsg &msg);
int build_tx_sub_rollback_msg_(const ObTxDesc &tx, ObTxSubRollbackMsg &msg);
int sub_prepare_local_ls_(const ObTransID &tx_id,
                          const share::ObLSID &coord,
                          const share::ObLSArray &parts,
                          const int64_t &expire_ts,
                          const common::ObString & app_trace_info,
                          const int64_t &request_id,
                          const ObXATransID &xid);
int handle_sub_prepare_timeout_(ObTxDesc &tx, const int64_t delay);
int handle_sub_rollback_timeout_(ObTxDesc &tx, const int64_t delay);
int handle_sub_commit_timeout_(ObTxDesc &tx, const int64_t delay);
int handle_sub_prepare_result_(ObTxDesc &tx, const int result);
int handle_sub_end_tx_result_(ObTxDesc &tx, const bool is_rollback, const int result);
int sub_end_tx_local_ls_(const ObTransID &tx_id,
                         const share::ObLSID &coord,
                         const int64_t &request_id,
                         const ObXATransID &xid,
                         const common::ObAddr &sender_addr,
                         const bool is_rollback);

private:
ObTxCtxMgr tx_ctx_mgr_;
void invalid_registered_snapshot_(ObTxDesc &tx);
void registered_snapshot_clear_part_(ObTxDesc &tx);
int ls_rollback_to_savepoint_(const ObTransID &tx_id,
                              const share::ObLSID &ls,
                              const int64_t verify_epoch,
                              const int64_t op_sn,
                              const int64_t savepoint,
                              int64_t &ctx_born_epoch,
                              const ObTxDesc *tx,
                              int64_t expire_ts = -1);
int sync_rollback_savepoint__(ObTxDesc &tx,
                              ObTxRollbackSPMsg &msg,
                              const ObTxDesc::MaskSet &mask_set,
                              int64_t expire_ts,
                              const int64_t max_retry_interval,
                              int &retries);
int create_local_implicit_savepoint_(ObTxDesc &tx,
                                     int64_t &savepoint);
int create_global_implicit_savepoint_(ObTxDesc &tx,
                                      const ObTxParam &tx_param,
                                      int64_t &savepoint,
                                      const bool release);
int rollback_to_local_implicit_savepoint_(ObTxDesc &tx,
                                          const int64_t savepoint,
                                          const int64_t expire_ts);
int rollback_to_global_implicit_savepoint_(ObTxDesc &tx,
                                           const int64_t savepoint,
                                           const int64_t expire_ts,
                                           const share::ObLSArray *extra_touched_ls);
int ls_sync_rollback_savepoint__(ObPartTransCtx *part_ctx,
                                 const int64_t savepoint,
                                 const int64_t op_sn,
                                 const int64_t expire_ts);
void tx_post_terminate_(ObTxDesc &tx);
int start_epoch_(ObTxDesc &tx);
int tx_sanity_check_(const ObTxDesc &tx);
int get_tx_table_guard_(ObLS *ls,
                        const share::ObLSID &ls_id,
                        ObTxTableGuard &guard);
void fetch_cflict_tx_ids_from_mem_ctx_to_desc_(memtable::ObMvccAccessCtx &acc_ctx);
int wait_follower_readable_(ObLS &ls,
                            const int64_t expire_ts,
                            const share::SCN snapshot);
// include tx api refacored for future
public:
#include "ob_tx_api.h"
