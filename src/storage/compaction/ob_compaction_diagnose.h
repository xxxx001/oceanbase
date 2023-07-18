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

#ifndef SRC_STORAGE_COMPACTION_OB_COMPACTION_DIAGNOSE_H_
#define SRC_STORAGE_COMPACTION_OB_COMPACTION_DIAGNOSE_H_

#include "storage/ob_i_store.h"
#include "ob_tablet_merge_task.h"
#include "lib/list/ob_dlist.h"
#include "share/scheduler/ob_diagnose_config.h"

namespace oceanbase
{
namespace storage
{
class ObTenantTabletIterator;
}
namespace rootserver
{
  class ObMajorFreezeService;
}
using namespace storage;
using namespace share;
namespace compaction
{
class ObIDiagnoseInfoMgr;
struct ObDiagnoseTabletCompProgress;

enum ObInfoParamStructType {
  SUSPECT_INFO_PARAM = 0,
  DAG_WARNING_INFO_PARAM,
  INFO_PARAM_TYPE_MAX
};

struct ObInfoParamStruct {
  int max_type_;
  const share::ObDiagnoseInfoStruct *info_type;
};

static constexpr ObInfoParamStruct OB_DIAGNOSE_INFO_PARAMS[] = {
  {share::ObSuspectInfoType::SUSPECT_INFO_TYPE_MAX, share::OB_SUSPECT_INFO_TYPES},
  {ObDagType::ObDagTypeEnum::DAG_TYPE_MAX, share::OB_DAG_WARNING_INFO_TYPES},
};

union ObInfoParamType{
  share::ObSuspectInfoType suspect_type_;
  ObDagType::ObDagTypeEnum dag_type_;
};

struct ObIBasicInfoParam
{
  ObIBasicInfoParam()
    : type_(),
      struct_type_(INFO_PARAM_TYPE_MAX)
  {}
  virtual void destroy() = 0;
  virtual int64_t get_deep_copy_size() const = 0;

  virtual int fill_comment(char *buf, const int64_t buf_len) const = 0;
  virtual int deep_copy(ObIAllocator &allocator, ObIBasicInfoParam *&out_param) const = 0;

  static const int64_t MAX_INFO_PARAM_SIZE = 256;

  ObInfoParamType type_;
  ObInfoParamStructType struct_type_;
};

static const int64_t OB_DIAGNOSE_INFO_PARAM_STR_LENGTH = 64;
template <int64_t int_size, int64_t str_size = OB_DIAGNOSE_INFO_PARAM_STR_LENGTH>
struct ObDiagnoseInfoParam : public ObIBasicInfoParam
{
  ObDiagnoseInfoParam()
    : ObIBasicInfoParam()
  {
    MEMSET(param_int_, 0, int_size * sizeof(int64_t));
    MEMSET(comment_, 0, str_size);
  }
  virtual void destroy() override;
  virtual int64_t get_deep_copy_size() const override;
  virtual int fill_comment(char *buf, const int64_t buf_len) const override;
  virtual int deep_copy(ObIAllocator &allocator, ObIBasicInfoParam *&out_param) const override;

  int64_t param_int_[int_size];
  char comment_[str_size];
};

struct ObIDiagnoseInfo : public common::ObDLinkBase<ObIDiagnoseInfo> {
  ObIDiagnoseInfo()
    : is_deleted_(false),
      seq_num_(0),
      tenant_id_(OB_INVALID_ID),
      info_param_(nullptr)
  {}
  virtual void destroy(ObIAllocator &allocator)
  {
    if (OB_NOT_NULL(info_param_)) {
      info_param_->destroy();
      allocator.free(info_param_);
      info_param_ = nullptr;
    }
    allocator.free(this);
  }
  virtual void shallow_copy(ObIDiagnoseInfo *other) = 0;
  virtual void update(ObIDiagnoseInfo *other) {}
  virtual int64_t get_add_time() const { return INT_MAX64; }
  virtual int64_t get_hash() const { return 0; }
  template<typename T>
  int deep_copy(ObIAllocator &allocator, T *&out_info);
  bool is_deleted() const { return ATOMIC_LOAD(&is_deleted_); }
  void set_deleted() { ATOMIC_SET(&is_deleted_, true); }
  // for iterator
  bool is_deleted_;
  uint64_t seq_num_;
  uint64_t tenant_id_;
  //
  ObIBasicInfoParam *info_param_;
};

/* ObIDiagnoseInfo func */
template<typename T>
int ObIDiagnoseInfo::deep_copy(ObIAllocator &allocator, T *&out_info)
{
  int ret = OB_SUCCESS;
  void *buf = nullptr;
  out_info = nullptr;
  if(OB_ISNULL(buf = allocator.alloc(sizeof(T)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    T *info = nullptr;
    if (OB_ISNULL(info = new (buf) T())) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN, "new diagnose info is nullptr", K(ret));
    } else if (OB_NOT_NULL(info_param_)) {
      if (OB_FAIL(info_param_->deep_copy(allocator, info->info_param_))){
        STORAGE_LOG(WARN, "fail to deep copy info param", K(ret));
      }
    }

    if (OB_SUCC(ret)) {
      info->shallow_copy(this);
      out_info = info;
    } else if (OB_NOT_NULL(info)) {
      info->destroy(allocator);
      info = nullptr;
    } else {
      allocator.free(buf);
    }
  }
  return ret;
}

struct ObScheduleSuspectInfo : public ObIDiagnoseInfo, public ObMergeDagHash
{
  ObScheduleSuspectInfo()
   : ObIDiagnoseInfo(),
     ObMergeDagHash(),
     add_time_(0),
     hash_(0)
  {}
  int64_t hash() const;
  bool is_valid() const;
  virtual void shallow_copy(ObIDiagnoseInfo *other) override;
  virtual int64_t get_add_time() const override;
  virtual int64_t get_hash() const override;

  static int64_t gen_hash(int64_t tenant_id, int64_t dag_hash);
  TO_STRING_KV(K_(tenant_id), K_(merge_type), K_(ls_id), K_(tablet_id), K_(add_time), K_(hash));

  int64_t add_time_;
  int64_t hash_;
};

class ObIDiagnoseInfoMgr {
public:
  struct Iterator {
    Iterator()
      : is_opened_(false),
        version_(0),
        seq_num_(0),
        current_info_(nullptr)
    {}
    virtual ~Iterator() { reset(); }
    void reset()
    {
      is_opened_ = false;
      version_ = 0;
      seq_num_ = 0;
      current_info_ = nullptr;
    }
    bool is_opened() const { return is_opened_; }

    int open(const uint64_t version, ObIDiagnoseInfo *current_info, ObIDiagnoseInfoMgr *info_pool);
    int get_next(ObIDiagnoseInfo *out_info, char *buf, const int64_t buf_len);

  private:
    int next();
  public:
    bool is_opened_;
    uint64_t version_;
    uint64_t seq_num_;
    ObIDiagnoseInfo *current_info_;
    ObIDiagnoseInfoMgr *info_pool_;
  };

  ObIDiagnoseInfoMgr()
    : is_inited_(false),
      page_size_(0),
      version_(0),
      seq_num_(0),
      pool_label_(),
      bucket_label_(),
      node_label_(),
      lock_(common::ObLatchIds::INFO_MGR_LOCK),
      rwlock_(common::ObLatchIds::INFO_MGR_LOCK),
      allocator_(),
      info_list_()
  {
    MEMSET(pool_label_, '\0', sizeof(pool_label_));
    MEMSET(bucket_label_, '\0', sizeof(bucket_label_));
    MEMSET(node_label_, '\0', sizeof(node_label_));
  }
  virtual ~ObIDiagnoseInfoMgr() { destroy(); }

  int init(bool with_map,
           const uint64_t tenant_id,
           const char* basic_label,
           const int64_t page_size=INFO_PAGE_SIZE,
           int64_t max_size=INFO_MAX_SIZE);

  void destroy();
  void clear();
  void clear_with_no_lock();
  int size();

  template<typename T>
  int alloc_and_add(const int64_t key, T *input_info);
  int get_with_param(const int64_t key, ObIDiagnoseInfo *out_info, ObIAllocator &allocator);
  int delete_info(const int64_t key);

  int set_max(const int64_t size);
  int gc_info();

  int open_iter(Iterator &iter);

private:
  int add_with_no_lock(const int64_t key, ObIDiagnoseInfo *info);
  // info may update based on old_info // allow info is null
  int del_with_no_lock(const int64_t key, ObIDiagnoseInfo *info);
  int get_with_no_lock(const int64_t key, ObIDiagnoseInfo *&info);
  int purge_with_rw_lock(bool batch_purge = false);

public:
  static const int64_t MAX_ALLOC_RETRY_TIMES = 10;
  static const int64_t GC_HIGH_PERCENTAGE = 80; // GC_HIGH_PERCENTAGE/100
  static const int64_t GC_LOW_PERCENTAGE = 40;  // GC_LOW_PERCENTAGE/100
  static const int64_t INFO_BUCKET_LIMIT = 1000;
  static const int64_t INFO_PAGE_SIZE = (1 << 16); // 64KB
  static const int64_t INFO_PAGE_SIZE_LIMIT = (1 << 11); // 2KB
  static const int64_t INFO_IDLE_SIZE = 16LL * 1024LL * 1024LL; // 16MB
  static const int64_t INFO_MAX_SIZE = 16LL * 1024LL * 1024LL; // 16MB // lowest
  typedef common::hash::ObHashMap<int64_t, ObIDiagnoseInfo *> InfoMap;
  typedef common::ObDList<ObIDiagnoseInfo> InfoList;



protected:
  bool is_inited_;
  int64_t page_size_;
  uint64_t version_; // locked by rwlock_
  uint64_t seq_num_; // locked by lock_
  char pool_label_[lib::AOBJECT_LABEL_SIZE + 1];
  char bucket_label_[lib::AOBJECT_LABEL_SIZE + 1];
  char node_label_[lib::AOBJECT_LABEL_SIZE + 1];
  common::SpinRWLock lock_;
  common::SpinRWLock rwlock_;
  ObFIFOAllocator allocator_;
  InfoList info_list_;
  InfoMap info_map_;
};

template<typename T>
int ObIDiagnoseInfoMgr::alloc_and_add(const int64_t key, T *input_info)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    STORAGE_LOG(WARN, "ObIDiagnoseInfoMgr is not init", K(ret));
  } else if (OB_ISNULL(input_info)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    T *info = NULL;
    common::SpinWLockGuard guard(lock_);
    if (info_map_.created()) {
      if (OB_FAIL(del_with_no_lock(key, input_info))) {
        if (OB_HASH_NOT_EXIST != ret) {
          STORAGE_LOG(WARN, "failed to del old info", K(ret), K(key));
        }
      }
    }

    if (OB_HASH_NOT_EXIST == ret || OB_SUCC(ret)) {
      ret = OB_SUCCESS;
      int64_t retry_nums = MAX_ALLOC_RETRY_TIMES;
      while (OB_SUCC(ret) && retry_nums-- &&
        OB_ALLOCATE_MEMORY_FAILED == input_info->deep_copy(allocator_, info)) {
        // retry
        ret = purge_with_rw_lock(true);
      }
      if (OB_FAIL(ret)) {
        STORAGE_LOG(WARN, "failed to add info into pool", K(ret), K(key));
      } else if (OB_FAIL(add_with_no_lock(key, info))) {
        STORAGE_LOG(WARN, "failed to add info into pool", K(ret), K(key));
      }
    }
  }
  return ret;
}

class ObScheduleSuspectInfoMgr : public ObIDiagnoseInfoMgr {
public:
  static int mtl_init(ObScheduleSuspectInfoMgr *&schedule_suspect_info);
  static int64_t cal_max();
  ObScheduleSuspectInfoMgr()
    : ObIDiagnoseInfoMgr()
  {}
  ~ObScheduleSuspectInfoMgr() { destroy(); }

  void destroy() {
    ObIDiagnoseInfoMgr::destroy();
    STORAGE_LOG(INFO, "ObScheduleSuspectInfoMgr destroy finish");
  }
  int add_suspect_info(const int64_t key_value, ObScheduleSuspectInfo &info);

public:
  static const int64_t EXTRA_INFO_LEN = 900;
  static constexpr double MEMORY_PERCENTAGE = 0.5;   // max size = tenant memory size * MEMORY_PERCENTAGE / 100
  static const int64_t POOL_MAX_SIZE = 48LL * 1024LL * 1024LL; // 48MB
};

class ObInfoParamBuffer : public ObDataBuffer
{
public:
  ObInfoParamBuffer()
    : ObDataBuffer(buff, sizeof(buff))
  {}
  virtual ~ObInfoParamBuffer() {}

  void reuse()
  {
    reset();
    (void)set_data(buff, sizeof(buff));
  }
protected:
  char buff[ObIBasicInfoParam::MAX_INFO_PARAM_SIZE];
};

struct ObCompactionDiagnoseInfo
{
  enum ObDiagnoseStatus
  {
    DIA_STATUS_NOT_SCHEDULE = 0,
    DIA_STATUS_RUNNING = 1,
    DIA_STATUS_FAILED = 2,
    DIA_STATUS_FINISH = 3,
    DIA_STATUS_RS_UNCOMPACTED = 4, // RS diagnose
    DIA_STATUS_MAX
  };
  const static char *ObDiagnoseStatusStr[DIA_STATUS_MAX];
  static const char * get_diagnose_status_str(ObDiagnoseStatus status);
  TO_STRING_KV(K_(merge_type), K_(tenant_id), K_(ls_id), K_(tablet_id), K_(status), K_(timestamp),
      K_(diagnose_info));

  storage::ObMergeType merge_type_;
  int64_t tenant_id_;
  int64_t ls_id_;
  int64_t tablet_id_;
  int64_t timestamp_;
  ObDiagnoseStatus status_;
  char diagnose_info_[common::OB_DIAGNOSE_INFO_LENGTH];
};

class ObCompactionDiagnoseMgr
{
public:
struct ObLSCheckStatus
  {
  public:
    ObLSCheckStatus() { reset(); }
    ObLSCheckStatus(bool weak_read_ts_ready, bool need_merge, bool is_leader)
      : weak_read_ts_ready_(weak_read_ts_ready),
        need_merge_(need_merge),
        is_leader_(is_leader)
    {}
    ~ObLSCheckStatus() {}
    OB_INLINE void reset() {
      weak_read_ts_ready_ = false;
      need_merge_ = false;
      is_leader_ = false;
    }

    TO_STRING_KV(K_(weak_read_ts_ready), K_(need_merge), K_(is_leader));
    bool weak_read_ts_ready_;
    bool need_merge_;
    bool is_leader_;
  };
public:
  ObCompactionDiagnoseMgr();
  ~ObCompactionDiagnoseMgr() { reset(); }
  void reset();
  int init(common::ObIAllocator *allocator, ObCompactionDiagnoseInfo *info_array, const int64_t max_cnt);
  int diagnose_all_tablets(const int64_t tenant_id);
  int diagnose_tenant_tablet();
  int diagnose_tenant_major_merge();
  int64_t get_cnt() { return idx_; }
  static int diagnose_dag(
      const storage::ObMergeType merge_type,
      const ObLSID ls_id,
      const ObTabletID tablet_id,
      const int64_t merge_version,
      ObTabletMergeDag &dag,
      ObDiagnoseTabletCompProgress &input_progress);
  static int check_system_compaction_config(char *tmp_str, const int64_t buf_len);
private:
  int get_next_tablet(ObLSID &ls_id);
  void release_last_tenant();
  int gen_ls_check_status(const ObLSID &ls_id, const int64_t compaction_scn, ObLSCheckStatus &ls_status);
  int diagnose_ls_merge(
      const ObMergeType merge_type,
      const ObLSID &ls_id);
  int diagnose_tablet_mini_merge(const ObLSID &ls_id, ObTablet &tablet);
  int diagnose_tablet_minor_merge(const ObLSID &ls_id, ObTablet &tablet);
  int diagnose_tablet_major_and_medium(
      const bool diagnose_major_flag,
      const bool weak_read_ts_ready,
      const int64_t compaction_scn,
      const ObLSID &ls_id,
      ObTablet &tablet,
      bool &tablet_major_finish);
  int diagnose_tablet_merge(
      ObTabletMergeDag &dag,
      const ObMergeType type,
      const ObLSID ls_id,
      const ObTabletID tablet_id,
      int64_t merge_version = ObVersionRange::MIN_VERSION);
  int diagnose_no_dag(
      ObTabletMergeDag &dag,
      const ObMergeType merge_type,
      const ObLSID ls_id,
      const ObTabletID tablet_id,
      const int64_t compaction_scn);
  int get_suspect_and_warning_info(
      ObTabletMergeDag &dag,
      const ObMergeType merge_type,
      const ObLSID ls_id,
      const ObTabletID tablet_id,
      ObScheduleSuspectInfo &info,
      char *buf,
      const int64_t buf_len);

  int diagnose_medium_scn_table(const int64_t compaction_scn);
  OB_INLINE bool can_add_diagnose_info() { return idx_ < max_cnt_; }
  int get_suspect_info_and_print(
      const ObMergeType merge_type,
      const ObLSID &ls_id,
      const ObTabletID &tablet_id);
  int check_if_need_diagnose(rootserver::ObMajorFreezeService *&major_freeze_service,
                             bool &need_diagnose) const;
  int do_tenant_major_merge_diagnose(rootserver::ObMajorFreezeService *major_freeze_service);

public:
  typedef common::hash::ObHashMap<ObLSID, ObLSCheckStatus> LSStatusMap;
private:
  static const int64_t WAIT_MEDIUM_SCHEDULE_INTERVAL = 1000L * 1000L * 1000L * 60L * 5; // 5 min // ns
  static const int64_t MAX_LS_TABLET_CNT = 10 * 10000; // TODO(@jingshui): tmp solution
  bool is_inited_;
  ObIAllocator *allocator_;
  storage::ObTenantTabletIterator *tablet_iter_;
  common::ObArenaAllocator tablet_allocator_;
  ObTabletHandle tablet_handle_;
  void *iter_buf_;
  ObCompactionDiagnoseInfo *info_array_;
  int64_t max_cnt_;
  int64_t idx_;
};

class ObCompactionDiagnoseIterator
{
public:
  ObCompactionDiagnoseIterator()
   : allocator_("CompDiagnose"),
     info_array_(nullptr),
     cnt_(0),
     cur_idx_(0),
     is_opened_(false)
  {
  }
  virtual ~ObCompactionDiagnoseIterator() { reset(); }
  int open(const int64_t tenant_id);
  int get_next_info(ObCompactionDiagnoseInfo &info);
  void reset();

private:
  int get_diagnose_info(const int64_t tenant_id);
private:
  const int64_t MAX_DIAGNOSE_INFO_CNT = 1000;
  ObArenaAllocator allocator_;
  ObCompactionDiagnoseInfo *info_array_;
  int64_t cnt_;
  int64_t cur_idx_;
  bool is_opened_;
};

#define DEL_SUSPECT_INFO(type, ls_id, tablet_id) \
{ \
      int tmp_ret = OB_SUCCESS;                                                                 \
      compaction::ObMergeDagHash dag_hash;                                                      \
      dag_hash.merge_type_ = type;                                                                     \
      dag_hash.ls_id_ = ls_id;                                                                           \
      dag_hash.tablet_id_ = tablet_id;                                                                   \
      int64_t tenant_id = MTL_ID();                                                                     \
      int64_t hash_value = ObScheduleSuspectInfo::gen_hash(tenant_id, dag_hash.inner_hash());          \
      if (OB_TMP_FAIL(MTL(ObScheduleSuspectInfoMgr *)->delete_info(hash_value))) { \
        if (OB_HASH_NOT_EXIST != tmp_ret) {                                                                \
          STORAGE_LOG(WARN, "failed to del suspect info", K(tmp_ret), K(dag_hash), K(tenant_id));         \
        } else {                                                                                      \
          tmp_ret = OB_SUCCESS;                                                                           \
        }                                                                                            \
      } else {                                                                                      \
        STORAGE_LOG(DEBUG, "success to del suspect info", K(tmp_ret), K(dag_hash), K(tenant_id));       \
      }                                                                                       \
}

#define DEFINE_DIAGNOSE_PRINT_KV(n)                                                               \
  template <LOG_TYPENAME_TN##n>                                                                  \
  int SET_DIAGNOSE_INFO(ObCompactionDiagnoseInfo &diagnose_info, storage::ObMergeType type,     \
                const int64_t tenant_id, const ObLSID ls_id, const ObTabletID tablet_id,          \
                ObCompactionDiagnoseInfo::ObDiagnoseStatus status,                               \
                const int64_t timestamp,                                                         \
                LOG_PARAMETER_KV##n)                                                             \
  {                                                                                              \
    int64_t __pos = 0;                                                                           \
    int ret = OB_SUCCESS;                                                                        \
    diagnose_info.merge_type_ = type;                                                            \
    diagnose_info.ls_id_ = ls_id.id();                                                     \
    diagnose_info.tenant_id_ = tenant_id;                                                    \
    diagnose_info.tablet_id_ = tablet_id.id();                                                \
    diagnose_info.status_ = status;                                                          \
    diagnose_info.timestamp_ = timestamp;                                                          \
    char *buf = diagnose_info.diagnose_info_;                                                    \
    const int64_t buf_size = ::oceanbase::common::OB_DIAGNOSE_INFO_LENGTH;                       \
    SIMPLE_TO_STRING_##n                                                                      \
    if (__pos < buf_size) {                                                                   \
      buf[__pos-1] = '\0';                                                                    \
    } else {                                                                                  \
      buf[__pos] = '\0';                                                                      \
    }                                                                                         \
    return ret;                                                                               \
  }

#define DEFINE_COMPACITON_INFO_ADD_KV(n)                                                               \
  template <LOG_TYPENAME_TN##n>                                                                  \
  void ADD_COMPACTION_INFO_PARAM(char *buf, const int64_t buf_size, LOG_PARAMETER_KV##n)                  \
  {                                                                                              \
    int64_t __pos = strlen(buf);                                                                  \
    int ret = OB_SUCCESS;                                                                        \
    SIMPLE_TO_STRING_##n                                                                        \
    if (__pos > 0) {                                                                            \
      buf[__pos - 1] = ';';                                                                      \
    }                                                                                             \
    buf[__pos] = '\0';                                                                             \
  }

#define SIMPLE_TO_STRING(n)                                                                       \
    if (OB_FAIL(ret)) {                                                                          \
    } else if (OB_FAIL(::oceanbase::common::logdata_print_key_obj(buf, buf_size - 1, __pos, key##n, false, obj##n))) { \
    } else if (__pos + 1 >= buf_size) {                                                          \
    } else {                                                                                     \
      buf[__pos++] = ',';                                                                        \
    }

#define SIMPLE_TO_STRING_1  SIMPLE_TO_STRING(1)

#define SIMPLE_TO_STRING_2                                                                    \
    SIMPLE_TO_STRING_1                                                                        \
    SIMPLE_TO_STRING(2)

#define SIMPLE_TO_STRING_3                                                                    \
    SIMPLE_TO_STRING_2                                                                        \
    SIMPLE_TO_STRING(3)

#define SIMPLE_TO_STRING_4                                                                    \
    SIMPLE_TO_STRING_3                                                                        \
    SIMPLE_TO_STRING(4)

#define SIMPLE_TO_STRING_5                                                                    \
    SIMPLE_TO_STRING_4                                                                        \
    SIMPLE_TO_STRING(5)

#define SIMPLE_TO_STRING_6                                                                   \
    SIMPLE_TO_STRING_5                                                                        \
    SIMPLE_TO_STRING(6)

#define SIMPLE_TO_STRING_7                                                                    \
    SIMPLE_TO_STRING_6                                                                        \
    SIMPLE_TO_STRING(7)

#define SIMPLE_TO_STRING_8                                                                    \
    SIMPLE_TO_STRING_7                                                                        \
    SIMPLE_TO_STRING(8)

DEFINE_DIAGNOSE_PRINT_KV(1)
DEFINE_DIAGNOSE_PRINT_KV(2)
DEFINE_DIAGNOSE_PRINT_KV(3)
DEFINE_DIAGNOSE_PRINT_KV(4)
DEFINE_DIAGNOSE_PRINT_KV(5)

DEFINE_COMPACITON_INFO_ADD_KV(1)
DEFINE_COMPACITON_INFO_ADD_KV(2)
DEFINE_COMPACITON_INFO_ADD_KV(3)
DEFINE_COMPACITON_INFO_ADD_KV(4)
DEFINE_COMPACITON_INFO_ADD_KV(5)
DEFINE_COMPACITON_INFO_ADD_KV(6)
DEFINE_COMPACITON_INFO_ADD_KV(7)
DEFINE_COMPACITON_INFO_ADD_KV(8)

#define INFO_PARAM_INT(n) T param_int##n
#define INFO_PARAM_INT0
#define INFO_PARAM_INT1 INFO_PARAM_INT(1)
#define INFO_PARAM_INT2 INFO_PARAM_INT1, INFO_PARAM_INT(2)
#define INFO_PARAM_INT3 INFO_PARAM_INT2, INFO_PARAM_INT(3)
#define INFO_PARAM_INT4 INFO_PARAM_INT3, INFO_PARAM_INT(4)
#define INFO_PARAM_INT5 INFO_PARAM_INT4, INFO_PARAM_INT(5)
#define INFO_PARAM_INT6 INFO_PARAM_INT5, INFO_PARAM_INT(6)
#define INFO_PARAM_INT7 INFO_PARAM_INT6, INFO_PARAM_INT(7)

#define INT_TO_PARAM_1 \
    info_param->param_int_[0] = param_int1;

#define INT_TO_PARAM_2 \
    INT_TO_PARAM_1 \
    info_param->param_int_[1] = param_int2;

#define INT_TO_PARAM_3 \
    INT_TO_PARAM_2 \
    info_param->param_int_[2] = param_int3;

#define INT_TO_PARAM_4 \
    INT_TO_PARAM_3 \
    info_param->param_int_[3] = param_int4;

#define INT_TO_PARAM_5 \
    INT_TO_PARAM_4 \
    info_param->param_int_[4] = param_int5;

#define INT_TO_PARAM_6 \
    INT_TO_PARAM_5 \
    info_param->param_int_[5] = param_int6;

#define INT_TO_PARAM_7 \
    INT_TO_PARAM_6 \
    info_param->param_int_[6] = param_int7;

#define DEFINE_SUSPECT_INFO_ADD(n_int)                                                           \
  template<typename T = int64_t>                                                                  \
  int ADD_SUSPECT_INFO(storage::ObMergeType type, const ObLSID ls_id,                      \
                const ObTabletID tablet_id, ObSuspectInfoType info_type,  \
                INFO_PARAM_INT##n_int)                                                           \
  {                                                                                              \
    int64_t __pos = 0;                                                                           \
    int ret = OB_SUCCESS;                                                                        \
    compaction::ObScheduleSuspectInfo info;                                                      \
    info.tenant_id_ = MTL_ID();                                                                  \
    info.merge_type_ = type;                                                                     \
    info.ls_id_ = ls_id;                                                                          \
    info.tablet_id_ = tablet_id;                                                                  \
    info.add_time_ = ObTimeUtility::fast_current_time();                                          \
    info.hash_ = info.hash();                                                               \
    ObDiagnoseInfoParam<n_int, 0> param;                                                          \
    ObDiagnoseInfoParam<n_int, 0> *info_param = &param;                                           \
    info_param->type_.suspect_type_ = info_type;                                                   \
    info_param->struct_type_ = ObInfoParamStructType::SUSPECT_INFO_PARAM;                          \
    INT_TO_PARAM_##n_int                                                                          \
    info.info_param_ = info_param;                                                               \
    if (OB_FAIL(MTL(ObScheduleSuspectInfoMgr *)->add_suspect_info(info.hash(), info))) { \
      STORAGE_LOG(WARN, "failed to add suspect info", K(ret), K(info));                          \
    } else {                                                                                      \
      STORAGE_LOG(DEBUG, "success to add suspect info", K(ret), K(info));                          \
    }                                                                                              \
    return ret;                                                                                \
  }

#define DEFINE_SUSPECT_INFO_ADD_EXTRA(n, n_int)                                                  \
  template <typename T = int64_t, LOG_TYPENAME_TN##n>                                            \
  int ADD_SUSPECT_INFO(storage::ObMergeType type, const ObLSID ls_id,   \
                const ObTabletID tablet_id, ObSuspectInfoType info_type,  \
                INFO_PARAM_INT##n_int, LOG_PARAMETER_KV##n)             \
  {                                                                                              \
    int64_t __pos = 0;                                                                           \
    int ret = OB_SUCCESS;                                                                        \
    compaction::ObScheduleSuspectInfo info;                                                      \
    info.tenant_id_ = MTL_ID();                                                                  \
    info.merge_type_ = type;                                                                     \
    info.ls_id_ = ls_id;                                                                          \
    info.tablet_id_ = tablet_id;                                                                  \
    info.add_time_ = ObTimeUtility::fast_current_time();                                          \
    info.hash_ = info.hash();                                                               \
    ObDiagnoseInfoParam<n_int> param;                                                             \
    ObDiagnoseInfoParam<n_int> *info_param = &param;                                              \
    INT_TO_PARAM_##n_int                                                                          \
    info_param->type_.suspect_type_ = info_type;                                                   \
    info_param->struct_type_ = ObInfoParamStructType::SUSPECT_INFO_PARAM;                          \
    char *buf = info_param->comment_;                                                              \
    const int64_t buf_size = OB_DIAGNOSE_INFO_PARAM_STR_LENGTH;                                    \
    SIMPLE_TO_STRING_##n                                                                          \
    info.info_param_ = info_param;                                                                \
    if (OB_FAIL(ret)) {                                                                           \
      STORAGE_LOG(WARN, "fail to fill parameter kv into info param", K(ret));                     \
    } else if (OB_FAIL(MTL(ObScheduleSuspectInfoMgr *)->add_suspect_info(info.hash(), info))) { \
      STORAGE_LOG(WARN, "failed to add suspect info", K(ret), K(info));                          \
    } else {                                                                                      \
      STORAGE_LOG(DEBUG, "success to add suspect info", K(ret), K(info));                          \
    }                                                                                              \
    return ret;                                                                                          \
  }

DEFINE_SUSPECT_INFO_ADD(1)
DEFINE_SUSPECT_INFO_ADD(2)
DEFINE_SUSPECT_INFO_ADD(3)
DEFINE_SUSPECT_INFO_ADD(4)
DEFINE_SUSPECT_INFO_ADD(5)

DEFINE_SUSPECT_INFO_ADD_EXTRA(1, 1)
DEFINE_SUSPECT_INFO_ADD_EXTRA(1, 3)
// ObDiagnoseInfoParam func
template <int64_t int_size, int64_t str_size>
void ObDiagnoseInfoParam<int_size, str_size>::destroy()
{
  MEMSET(param_int_, 0, int_size * sizeof(int64_t));
  MEMSET(comment_, 0, str_size);
}

template <int64_t int_size, int64_t str_size>
int64_t ObDiagnoseInfoParam<int_size, str_size>::get_deep_copy_size() const
{
  return sizeof(*this);
}

template <int64_t int_size, int64_t str_size>
int ObDiagnoseInfoParam<int_size, str_size>::fill_comment(char *buf, const int64_t buf_len) const
{
  int ret = OB_SUCCESS;
  int type = ObDagType::ObDagTypeEnum::DAG_TYPE_MAX;
  if (OB_ISNULL(buf)) {
    ret = OB_INVALID_ARGUMENT;
    STORAGE_LOG(WARN, "invalid argument", K(ret), KP(buf));
  } else if (INFO_PARAM_TYPE_MAX <= struct_type_) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "unexpected struct_type", K(ret), K_(struct_type));
  } else if (FALSE_IT(type = (SUSPECT_INFO_PARAM == struct_type_ ? type_.suspect_type_ : type_.dag_type_))) {
  } else if (OB_DIAGNOSE_INFO_PARAMS[struct_type_].max_type_ <= type) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "unexpected type", K(ret), K(type), K(OB_DIAGNOSE_INFO_PARAMS[struct_type_].max_type_));
  } else if (OB_DIAGNOSE_INFO_PARAMS[struct_type_].info_type[type].int_size != int_size) {
    ret = OB_ERR_UNEXPECTED;
    STORAGE_LOG(WARN, "unexpected int size", K(ret), K_(struct_type), K(type), K(int_size),
        K(OB_DIAGNOSE_INFO_PARAMS[struct_type_].info_type[type].int_size));
  } else {
    ADD_COMPACTION_INFO_PARAM(buf, buf_len,
                              "info", OB_DIAGNOSE_INFO_PARAMS[struct_type_].info_type[type].info_str);
    for (int i = 0; i < int_size; i++) {
      ADD_COMPACTION_INFO_PARAM(buf, buf_len,
                                OB_DIAGNOSE_INFO_PARAMS[struct_type_].info_type[type].info_str_fmt[i], param_int_[i]);
    }
    if (OB_DIAGNOSE_INFO_PARAMS[struct_type_].info_type[type].with_comment) {
      ADD_COMPACTION_INFO_PARAM(buf, buf_len, "extra_info", comment_);
    }
  }
  return ret;
}

template <int64_t int_size, int64_t str_size>
int ObDiagnoseInfoParam<int_size, str_size>::deep_copy(ObIAllocator &allocator, ObIBasicInfoParam *&out_param) const
{
  int ret = OB_SUCCESS;
  void *buf = nullptr;
  out_param = nullptr;
  if (OB_ISNULL(buf = allocator.alloc(get_deep_copy_size()))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
    STORAGE_LOG(WARN, "fail to alloc memory", K(ret));
  } else {
    ObDiagnoseInfoParam<int_size, str_size> *info_param = nullptr;
    if (OB_ISNULL(info_param = (new (buf) ObDiagnoseInfoParam<int_size, str_size>()))) {
      ret = OB_ERR_UNEXPECTED;
      STORAGE_LOG(WARN, "info_param is nullptr", K(ret));
    } else {
      info_param->type_ = type_;
      info_param->struct_type_ = struct_type_;
      MEMCPY(info_param->param_int_, param_int_, int_size * sizeof(int64_t));
      MEMCPY(info_param->comment_, comment_, str_size);
    }
    if (OB_SUCC(ret)) {
      out_param = info_param;
    } else {
      allocator.free(buf);
    }
  }
  return ret;
}
}//compaction
}//oceanbase

#endif
