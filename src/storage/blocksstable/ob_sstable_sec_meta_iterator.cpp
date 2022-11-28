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

#define USING_LOG_PREFIX STORAGE

#include "share/rc/ob_tenant_base.h"
#include "share/schema/ob_table_param.h"
#include "ob_sstable_sec_meta_iterator.h"
#include "storage/blocksstable/ob_logic_macro_id.h"

namespace oceanbase
{
namespace blocksstable
{

ObSSTableSecMetaIterator::ObSSTableSecMetaIterator()
  : sstable_meta_(nullptr), index_read_info_(nullptr), tenant_id_(OB_INVALID_TENANT_ID),
    prefetch_flag_(), idx_cursor_(), macro_reader_(), block_cache_(nullptr),
    micro_reader_(nullptr), micro_reader_helper_(),
    query_range_(nullptr), start_bound_micro_block_(), end_bound_micro_block_(),
    micro_handles_(), row_(), curr_handle_idx_(0), prefetch_handle_idx_(0), prev_block_row_cnt_(0),
    curr_block_start_idx_(0), curr_block_end_idx_(0), curr_block_idx_(0), step_cnt_(0),
    is_reverse_scan_(false), is_prefetch_end_(false),
    is_range_end_key_multi_version_(false), is_inited_(false) {}

void ObSSTableSecMetaIterator::reset()
{
  sstable_meta_ = nullptr;
  index_read_info_ = nullptr;
  tenant_id_ = OB_INVALID_TENANT_ID;
  prefetch_flag_.reset();
  idx_cursor_.reset();
  block_cache_ = nullptr;
  micro_reader_ = nullptr;
  micro_reader_helper_.reset();
  row_.reset();
  query_range_ = nullptr;
  curr_handle_idx_ = 0;
  prefetch_handle_idx_ = 0;
  prev_block_row_cnt_ = 0;
  curr_block_start_idx_ = 0;
  curr_block_end_idx_ = 0;
  curr_block_idx_ = 0;
  step_cnt_ = 0;
  is_reverse_scan_ = false;
  is_prefetch_end_ = false;
  is_range_end_key_multi_version_ = false;
  is_inited_ = false;
}

int ObSSTableSecMetaIterator::open(
    const ObDatumRange &query_range,
    const ObMacroBlockMetaType meta_type,
    const ObSSTable &sstable,
    const ObTableReadInfo &index_read_info,
    ObIAllocator &allocator,
    const bool is_reverse_scan,
    const int64_t sample_step)
{
  int ret = OB_SUCCESS;
  if (IS_INIT) {
    ret = OB_INIT_TWICE;
    LOG_WARN("Fail to open sstable secondary meta iterator", K(ret));
  } else if (OB_UNLIKELY(!query_range.is_valid()
      || !sstable.is_valid()
      || meta_type == ObMacroBlockMetaType::MAX)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid argument to open sstable secondary meta iterator",
        K(ret), K(query_range), K(sstable), K(meta_type));
  } else if (is_sstable_meta_empty(meta_type, sstable.get_meta())) {
    set_iter_end();
    is_inited_ = true;
    LOG_DEBUG("Empty sstable secondary meta", K(ret), K(meta_type), K(sstable));
  } else {
    sstable_meta_ = &sstable.get_meta();
    index_read_info_ = &index_read_info;
    tenant_id_ = MTL_ID();
    prefetch_flag_.set_not_use_block_cache();
    query_range_ = &query_range;
    is_reverse_scan_ = is_reverse_scan;
    block_cache_ = &ObStorageCacheSuite::get_instance().get_block_cache();
  }

  if (OB_FAIL(ret) || is_prefetch_end_) {
  } else if (OB_FAIL(idx_cursor_.init(sstable, allocator, index_read_info_,
      get_index_tree_type_map()[meta_type]))) {
    LOG_WARN("Fail to init index block tree cursor", K(ret), K(meta_type));
  } else if (OB_FAIL(init_micro_reader(
      static_cast<ObRowStoreType>(sstable.get_meta().get_basic_meta().row_store_type_),
      allocator))) {
    LOG_WARN("Fail to get root row store type", K(ret), K(sstable));
  } else {
    const int64_t schema_rowkey_cnt = sstable.get_meta().get_basic_meta().rowkey_column_count_
        - ObMultiVersionRowkeyHelpper::get_extra_rowkey_col_cnt();
    is_range_end_key_multi_version_ =
        schema_rowkey_cnt < query_range.get_end_key().get_datum_cnt();
    bool start_key_beyond_range = false;
    bool end_key_beyond_range = false;
    if (is_reverse_scan) {
      if (OB_FAIL(locate_bound_micro_block(
          query_range.get_start_key(),
          query_range.get_border_flag().inclusive_start(),
          start_bound_micro_block_,
          start_key_beyond_range))) {
        LOG_WARN("Fail to locate start bound micro block", K(ret));
      } else if (OB_FAIL(locate_bound_micro_block(
          query_range.get_end_key(),
          (!query_range.get_border_flag().inclusive_end() || is_range_end_key_multi_version_),
          end_bound_micro_block_,
          end_key_beyond_range))) {
        LOG_WARN("Fail to locate end bound micro block", K(ret));
      }
    } else {
      if (OB_FAIL(locate_bound_micro_block(
          query_range.get_end_key(),
          (!query_range.get_border_flag().inclusive_end() || is_range_end_key_multi_version_),
          end_bound_micro_block_,
          end_key_beyond_range))) {
        LOG_WARN("Fail to locate end bound micro block", K(ret));
      } else if (OB_FAIL(locate_bound_micro_block(
          query_range.get_start_key(),
          query_range.get_border_flag().inclusive_start(),
          start_bound_micro_block_,
          start_key_beyond_range))) {
        LOG_WARN("Fail to locate start bound micro block", K(ret));
      }
    }

    if (OB_FAIL(ret) || is_prefetch_end_) {
    } else if (OB_UNLIKELY(start_key_beyond_range)) {
      ret = OB_BEYOND_THE_RANGE;
      set_iter_end();
    } else if (OB_FAIL(prefetch_micro_block(1 /* fetch first micro block */))) {
      LOG_WARN("Fail to prefetch next micro block", K(ret), K_(is_prefetch_end));
    } else if (OB_FAIL(row_.init(allocator, index_read_info_->get_request_count()))) {
      STORAGE_LOG(WARN, "Failed to init datum row", K(ret));
    } else {
      if (sample_step != 0) {
        // is sample scan
        const int64_t start_offset = sample_step > 1 ? (sample_step / 2 - 1) : 0;
        step_cnt_ = is_reverse_scan ? (-sample_step) : sample_step;
        curr_block_idx_ = is_reverse_scan ? (-1 - start_offset) : start_offset;
      } else {
        step_cnt_ = is_reverse_scan ? -1 : 1;
        curr_block_idx_ = is_reverse_scan ? -1 : 0;
      }
      curr_block_start_idx_ = 1;
      curr_block_end_idx_ = -1;
      is_inited_ = true;
      LOG_DEBUG("Open secondary meta iterator", K(ret), K(meta_type), K(is_reverse_scan),
          K(sample_step), K_(step_cnt), K_(curr_block_idx), K_(tenant_id), KPC_(query_range));
    }
  }
  return ret;
}


int ObSSTableSecMetaIterator::get_next(ObDataMacroBlockMeta &macro_meta)
{
  int ret = OB_SUCCESS;
  if (IS_NOT_INIT) {
    ret = OB_NOT_INIT;
    LOG_WARN("Secondary meta iterator not inited", K(ret));
  } else {
    while (OB_SUCC(ret) && !is_target_row_in_curr_block()) {
      if (OB_FAIL(open_next_micro_block())) {
        if (OB_UNLIKELY(OB_ITER_END != ret)) {
          LOG_WARN("Fail to open next micro block", K(ret));
        }
      }
    }
    if (OB_FAIL(ret)) {
    } else if (OB_FAIL(micro_reader_->get_row(curr_block_idx_, row_))) {
      LOG_WARN("Fail to get secondary meta row from block", K(ret));
    } else if (OB_FAIL(macro_meta.parse_row(row_))) {
      LOG_WARN("Fail to parse macro meta", K(ret));
    } else {
      curr_block_idx_ += step_cnt_;
    }
  }
  return ret;
}

bool ObSSTableSecMetaIterator::is_sstable_meta_empty(
    const ObMacroBlockMetaType meta_type,
    const ObSSTableMeta &sstable_meta)
{
  const bool is_empty = sstable_meta.is_empty();
  const bool is_data_macro_meta_empty = sstable_meta.get_macro_info().get_macro_meta_addr().is_none();
  return is_empty
      || (ObMacroBlockMetaType::DATA_BLOCK_META == meta_type && is_data_macro_meta_empty);
}

void ObSSTableSecMetaIterator::set_iter_end()
{
  is_prefetch_end_ = true;
  curr_block_start_idx_ = 1;
  curr_block_end_idx_ = -1;
  curr_block_idx_ = 0;
  prefetch_handle_idx_ = 0;
  curr_handle_idx_ = 0;
}

int ObSSTableSecMetaIterator::init_micro_reader(
    const ObRowStoreType row_store_type,
    ObIAllocator &allocator)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(micro_reader_helper_.init(allocator))) {
    LOG_WARN("Failed to init micro reader helper", K(ret));
  } else if (OB_FAIL(micro_reader_helper_.get_reader(row_store_type, micro_reader_))) {
    LOG_WARN("Failed to get micro block reader", K(ret), K(row_store_type));
  }
  return ret;
}

int ObSSTableSecMetaIterator::locate_bound_micro_block(
    const ObDatumRowkey &rowkey,
    const bool lower_bound,
    ObMicroBlockId &bound_block,
    bool &is_beyond_range)
{
  int ret = OB_SUCCESS;
  is_beyond_range = false;
  const ObIndexBlockRowHeader *idx_header = nullptr;
  MacroBlockId macro_id;
  ObLogicMacroBlockId logic_id;
  bool equal = false;
  if (OB_FAIL(idx_cursor_.pull_up_to_root())) {
    LOG_WARN("Fail to pull up tree cursor back to root", K(ret));
  } else if (OB_FAIL(idx_cursor_.drill_down(
      rowkey,
      ObIndexBlockTreeCursor::LEAF,
      lower_bound,
      equal,
      is_beyond_range))) {
    LOG_WARN("Fail to locate micro block address in index tree", K(ret));
  } else if (OB_FAIL(idx_cursor_.get_idx_row_header(idx_header))) {
    LOG_WARN("Fail to get index block row header", K(ret));
  } else {
    bound_block.macro_id_ = idx_header->get_macro_id();
    bound_block.offset_ = idx_header->get_block_offset();
    bound_block.size_ = idx_header->get_block_size();
  }
  return ret;
}

int ObSSTableSecMetaIterator::open_next_micro_block()
{
  int ret = OB_SUCCESS;
  int64_t row_cnt = 0;
  int64_t begin_idx = 0;
  int64_t end_idx = 0;
  ObMicroBlockData micro_data;
  ObMicroBlockDataHandle &micro_handle = micro_handles_[curr_handle_idx_ % HANDLE_BUFFER_SIZE];
  if (is_prefetch_end_ && 0 == handle_buffer_count()) {
    ret = OB_ITER_END;
  } else if (OB_FAIL(prefetch_micro_block(HANDLE_BUFFER_SIZE - handle_buffer_count()))) {
    LOG_WARN("Fail to prefetch micro blocks", K(ret), K(handle_buffer_count()));
  } else if (OB_FAIL(micro_handle.get_data_block_data(macro_reader_, micro_data))) {
    LOG_WARN("Fail to get micro block data", K(ret), K_(curr_handle_idx), K(micro_handle));
  } else if (OB_UNLIKELY(!micro_data.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid micro block data", K(ret), K(micro_data));
  } else if (OB_FAIL(micro_reader_->init(micro_data, *index_read_info_))) {
    LOG_WARN("Fail to init micro block reader", K(ret));
  } else if (OB_FAIL(micro_reader_->get_row_count(row_cnt))) {
    LOG_WARN("Fail to get end index", K(ret));
  } else {
    end_idx = row_cnt;
    ObMicroBlockId block_id(
        micro_handle.macro_block_id_,
        micro_handle.micro_info_.offset_,
        micro_handle.micro_info_.size_);
    const bool is_start_bound = block_id == start_bound_micro_block_;
    const bool is_end_bound = block_id == end_bound_micro_block_;
    const bool is_index_scan = true;
    if (!is_start_bound && !is_end_bound) {
      --end_idx;
      // full scan
    } else if (OB_FAIL(micro_reader_->locate_range(
        *query_range_,
        is_start_bound,
        is_end_bound,
        begin_idx,
        end_idx,
        is_index_scan))) {
      LOG_WARN("Fail to locate range", K(ret), KPC(query_range_),K(is_start_bound), K(is_end_bound),
          K_(start_bound_micro_block), K_(end_bound_micro_block));
    }
    LOG_DEBUG("Open next micro block", K(ret), K(is_start_bound), K(is_end_bound),
        K(begin_idx), K(end_idx), K_(curr_block_idx), K(is_index_scan), K(block_id));
  }

  if (OB_SUCC(ret)) {
    const int64_t curr_block_row_cnt = end_idx - begin_idx + 1;
    if (is_reverse_scan_) {
      if (OB_UNLIKELY(curr_block_idx_ >= 0)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Invalid current block index on reverse scan", K(ret), K_(curr_block_idx),
            K_(curr_block_start_idx), K_(curr_block_end_idx), K(begin_idx), K(end_idx));
      } else if (curr_block_idx_ + curr_block_row_cnt >= 0) {
        // next row in this block
        curr_block_idx_ = end_idx + curr_block_idx_ + 1;
      } else {
        curr_block_idx_ += curr_block_row_cnt;
      }
    } else {
      if (OB_UNLIKELY(curr_block_idx_ < prev_block_row_cnt_)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Invalid current block index on sequential scan", K(ret), K_(curr_block_idx),
            K_(curr_block_start_idx), K_(curr_block_end_idx), K(begin_idx), K(end_idx));
      } else if (curr_block_idx_ - prev_block_row_cnt_ < row_cnt) {
        // First block in scan : begin_idx may larger than 0, update curr_block_idx_
        // Non-first block : next row in this block
        curr_block_idx_ = begin_idx + (curr_block_idx_ - prev_block_row_cnt_);
      } else {
        curr_block_idx_ -= prev_block_row_cnt_;
      }
    }
    prev_block_row_cnt_ = row_cnt;
    curr_block_start_idx_ = begin_idx;
    curr_block_end_idx_ = end_idx;
    ++curr_handle_idx_;
  }

  return ret;
}

int ObSSTableSecMetaIterator::prefetch_micro_block(int64_t prefetch_depth)
{
  int ret = OB_SUCCESS;
  if (is_prefetch_end_) {
    //prefetch end
  } else if (OB_UNLIKELY(prefetch_depth + handle_buffer_count() > HANDLE_BUFFER_SIZE)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Prefetch depth larger than available buffer", K(ret));
  } else {
    int64_t prefetch_count = 0;
    const ObIndexBlockRowHeader *idx_row_header = nullptr;
    ObLogicMacroBlockId logic_id;
    ObMicroBlockId micro_block_id;
    while (OB_SUCC(ret) && prefetch_count < prefetch_depth && !is_prefetch_end_) {
      if (OB_FAIL(idx_cursor_.get_idx_row_header(idx_row_header))) {
        LOG_WARN("Fail to get index block row header", K(ret));
      } else if (OB_UNLIKELY(!idx_row_header->is_data_block())) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("Unexpected non-leaf node when prefetch sec meta micro block", K(ret));
      } else {
        micro_block_id.macro_id_ = idx_row_header->get_macro_id();
        micro_block_id.offset_ = idx_row_header->get_block_offset();
        micro_block_id.size_ = idx_row_header->get_block_size();
        is_prefetch_end_ = is_reverse_scan_
            ? start_bound_micro_block_ == micro_block_id
            : end_bound_micro_block_ == micro_block_id;

        LOG_DEBUG("Prefetch next micro block",
            K(ret), K_(prefetch_handle_idx), K(micro_block_id), KPC(idx_row_header));
        if (OB_FAIL(get_micro_block(
            micro_block_id.macro_id_,
            *idx_row_header,
            micro_handles_[prefetch_handle_idx_ % HANDLE_BUFFER_SIZE]))) {
          LOG_WARN("Fail to prefetch next micro block",
              K(ret), K(micro_block_id), KPC(idx_row_header), K_(prefetch_handle_idx));
        } else {
          ++prefetch_handle_idx_;
          ++prefetch_count;
          if (!is_prefetch_end_ && OB_FAIL(idx_cursor_.move_forward(is_reverse_scan_))) {
            LOG_WARN("Index tree cursor fail to move forward", K(ret));
          }
        }
      }
    }
  }
  return ret;
}

// TODO: Always async io for now, opt with cache
int ObSSTableSecMetaIterator::get_micro_block(
    const MacroBlockId &macro_id,
    const ObIndexBlockRowHeader &idx_row_header,
    ObMicroBlockDataHandle &data_handle)
{
  int ret = OB_SUCCESS;
  data_handle.reset();
  ObTabletHandle tablet_handle;
  if (OB_UNLIKELY(!macro_id.is_valid() || !idx_row_header.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("Invalid parameters to locate micro block", K(ret), K(macro_id), K(idx_row_header));
  } else if (OB_FAIL(block_cache_->get_cache_block(
      tenant_id_,
      macro_id,
      idx_row_header.get_block_offset(),
      idx_row_header.get_block_size(),
      data_handle.cache_handle_))) {
    if (OB_UNLIKELY(OB_ENTRY_NOT_EXIST != ret)) {
      LOG_WARN("Fail to get micro block handle from cache", K(ret), K(idx_row_header));
    } else {
      // Cache miss, async IO
      ObMicroIndexInfo idx_info;
      idx_info.row_header_ = &idx_row_header;
      // TODO: @saitong.zst not safe here, remove tablet_handle from SecMeta prefetch interface, disable cache decoders
      if (OB_FAIL(block_cache_->prefetch(
          tenant_id_,
          macro_id,
          idx_info,
          prefetch_flag_,
          *index_read_info_,
          tablet_handle,
          data_handle.io_handle_))) {
        LOG_WARN("Fail to prefetch with async io", K(ret));
      } else {
        data_handle.block_state_ = ObSSTableMicroBlockState::IN_BLOCK_IO;
      }
    }
  } else {
    data_handle.block_state_ = ObSSTableMicroBlockState::IN_BLOCK_CACHE;
  }

  if (OB_SUCC(ret)) {
    data_handle.macro_block_id_ = macro_id;
    data_handle.micro_info_.offset_ = idx_row_header.get_block_offset();
    data_handle.micro_info_.size_ = idx_row_header.get_block_size();
    const bool deep_copy_key = true;
    if (OB_FAIL(idx_row_header.fill_micro_des_meta(deep_copy_key, data_handle.des_meta_))) {
      LOG_WARN("Fail to fill deserialize meta", K(ret));
    }
  }
  return ret;
}

} // blocksstable
} // oceanbase
