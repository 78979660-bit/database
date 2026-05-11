#pragma once

#include <vector>
#include <memory>
#include <mutex>
#include <atomic>
#include <cstring>
#include <immintrin.h> // 寮曞叆 AVX/AVX2 鎸囦护闆嗘敮鎸?
#include "../../Catalog/Schema.h"
#include "../../Type/Value.h"
#include "../../Common/RID.h"
#include "../../Storage/Tuple/Tuple.h"
#include "../../Concurrency/Transaction.h"
#include "../../Common/ThreadPool.h"
#include <future>

namespace Database
{
    // 缂栫爜鍗忚
    enum class EncodingType
    {
        PLAIN_INT32,
        BIT_PACKED
    };

    // [鏋佽嚧鍒楀瓨寮曟搸] 鐪熸鐨?Columnar Storage 缁撴瀯 (绫?Parquet/Arrow 鍐呭瓨妯″瀷)
    struct ColumnChunk
    {
        EncodingType encoding_ = EncodingType::PLAIN_INT32;
        int bit_width_ = 32;

        std::vector<int32_t> plain_data;
        std::vector<uint64_t> packed_data;

        void InitBitPacked(int bits)
        {
            encoding_ = EncodingType::BIT_PACKED;
            bit_width_ = bits;
        }

        void Reserve(size_t capacity)
        {
            if (encoding_ == EncodingType::PLAIN_INT32)
            {
                plain_data.reserve(capacity);
            }
            else
            {
                size_t words = (capacity * bit_width_ + 63) / 64 + 2;
                packed_data.reserve(words);
            }
        }

        void Append(int32_t val, size_t row_idx)
        {
            if (encoding_ == EncodingType::PLAIN_INT32)
            {
                plain_data.push_back(val);
            }
            else
            {
                size_t bit_pos = row_idx * bit_width_;
                size_t word_idx = bit_pos >> 6; // / 64
                size_t bit_ofs = bit_pos & 63;  // % 64

                if (word_idx + 1 >= packed_data.size())
                {
                    packed_data.resize(word_idx + 2, 0);
                }

                uint64_t mask = (1ULL << bit_width_) - 1;
                uint64_t uval = static_cast<uint64_t>(val) & mask;
                packed_data[word_idx] |= (uval << bit_ofs);

                uint64_t shift2 = (64 - bit_ofs) & 63;
                uint64_t mask2 = (bit_ofs + bit_width_ > 64) ? ~0ULL : 0ULL;
                packed_data[word_idx + 1] |= ((uval >> shift2) & mask2);
            }
        }

        void SetValue(size_t row_idx, int32_t val)
        {
            if (encoding_ == EncodingType::PLAIN_INT32)
            {
                plain_data[row_idx] = val;
            }
            else
            {
                size_t bit_pos = row_idx * bit_width_;
                size_t word_idx = bit_pos >> 6;
                size_t bit_ofs = bit_pos & 63;

                uint64_t mask = (1ULL << bit_width_) - 1;
                uint64_t uval = static_cast<uint64_t>(val) & mask;

                packed_data[word_idx] &= ~(mask << bit_ofs);
                packed_data[word_idx] |= (uval << bit_ofs);

                uint64_t shift2 = (64 - bit_ofs) & 63;
                uint64_t mask2 = (bit_ofs + bit_width_ > 64) ? ~0ULL : 0ULL;

                packed_data[word_idx + 1] &= ~((mask >> shift2) & mask2);
                packed_data[word_idx + 1] |= ((uval >> shift2) & mask2);
            }
        }

        void AppendBatch(const int32_t *vals, size_t row_idx_start, size_t count)
        {
            if (encoding_ == EncodingType::PLAIN_INT32)
            {
                plain_data.insert(plain_data.end(), vals, vals + count);
            }
            else
            {
                size_t total_bits = (row_idx_start + count) * bit_width_;
                size_t required_words = (total_bits + 63) / 64 + 2;
                if (required_words >= packed_data.size())
                {
                    packed_data.resize(required_words, 0);
                }

                for (size_t i = 0; i < count; ++i)
                {
                    size_t bit_pos = (row_idx_start + i) * bit_width_;
                    size_t word_idx = bit_pos >> 6;
                    size_t bit_ofs = bit_pos & 63;

                    uint64_t mask = (1ULL << bit_width_) - 1;
                    uint64_t uval = static_cast<uint64_t>(vals[i]) & mask;
                    packed_data[word_idx] |= (uval << bit_ofs);

                    uint64_t shift2 = (64 - bit_ofs) & 63;
                    uint64_t mask2 = (bit_ofs + bit_width_ > 64) ? ~0ULL : 0ULL;
                    packed_data[word_idx + 1] |= ((uval >> shift2) & mask2);
                }
            }
        }

        int32_t GetValue(size_t idx) const
        {
            if (encoding_ == EncodingType::PLAIN_INT32)
                return plain_data[idx];
            size_t bit_pos = idx * bit_width_;
            size_t word_idx = bit_pos >> 6;
            size_t bit_ofs = bit_pos & 63;

            uint64_t shift2 = (64 - bit_ofs) & 63;
            uint64_t mask2 = (bit_ofs == 0) ? 0ULL : ~0ULL;

            uint64_t uval = (packed_data[word_idx] >> bit_ofs) |
                            ((packed_data[word_idx + 1] << shift2) & mask2);

            return static_cast<int32_t>(uval & ((1ULL << bit_width_) - 1));
        }

        // [楂橀€熻В鍘嬫祦姘寸嚎] 鐩存帴浠庨珮瀵嗗害鍐呭瓨鍧楁娊鍙栧苟杩樺師鑷?L1 缂撳瓨杩炵画妲戒綅
        const int32_t* UnpackBatch(size_t start_idx, size_t count, int32_t *out) const
        {
            if (encoding_ == EncodingType::PLAIN_INT32)
            {
                return plain_data.data() + start_idx;
            }
            else
            {
                size_t i = 0;
                // [娑堥櫎鏍囬噺鏍稿績鎷ュ] 鍒╃敤 x86 鍘熺敓 Little-Endian 鏀 寔閰嶅悎纭欢寮烘倣鐨勯潪瀵归綈璁垮瓨 (Unaligned Load)
                // 鐩存帴鍖栫箒涓虹畝锛屽共鎺夌箒鐞愮殑鎵嬪伐浣嶅钩绉?鎺╃爜锛屾 妸 10 鏉℃爣閲忕畻鏁版寚浠ょ缉鍑忎负 1 鏉?mov + 1 鏉?shr !
                const uint8_t *raw_bytes = reinterpret_cast<const uint8_t *>(packed_data.data());

#if defined(__AVX2__)
                // [鏋侀€?SIMD 瑙ｇ爜閫氶亾]: 閽堝甯歌 Bit-Width 鐨?AVX2 鐗瑰 寲瀹炵幇
                if (bit_width_ == 7)
                {
                    size_t bit_pos = (start_idx + i) * 7;
                    for (; i + 8 <= count; i += 8, bit_pos += 56)
                    {
                        // [杞欢棰勫彇鎸囦护] 鎻愬墠棰勫彇灏嗚瑙ｇ爜鐨勫唴瀛樺埌 L1 缂撳瓨锛岄殣钘?DRAM 寤惰繜
                        _mm_prefetch(reinterpret_cast<const char *>(raw_bytes) + (bit_pos >> 3) + 256, _MM_HINT_T0);

                        size_t word_idx = bit_pos >> 6;
                        size_t bit_ofs = bit_pos & 63;

                        uint64_t w0 = packed_data[word_idx];
                        uint64_t w1 = packed_data[word_idx + 1];

                        // [鎭㈠ P-Core 鐨勯粍閲戝垎鏀娴媇
                        // 灏?CMOV 涓夊厓琛ㄨ揪寮忚繕鍘熷洖 `if`銆?
                        // CMOV 铏界劧骞叉帀浜?E-Core 鐨勮棰勬祴锛屼絾瀹冨甫鏉 ョ殑寮哄埗鏁版嵁渚濊禆鐩存帴鎶?P-Core 鐨勬祦姘寸嚎 Retiring 浠?95% 鐮稿埌浜?66.6%锛?
                        // 鍙鎶婃寚浠ゆ媶鎴愮函 128-bit 鍚戦噺鎵ц锛孍-Core 鏍规湰涔熶笉浼氳Е鍙?Machine Clear 鐖嗛檷锛?
                        uint64_t combined = w0 >> bit_ofs;
                        if (bit_ofs != 0)
                            combined |= (w1 << (64 - bit_ofs));

                        // [缁堢鐮村眬锛氶噸褰掔函鏍囬噺 64-bit 鏋侀檺姒ㄥ彇]
                        // 瀹屽叏娑堢伃涓ラ噸鎯╃綒鐨勬牴婧愶細P-Core涓婄殑鎱㈡寚浠?(vpmulld) 鍜?E-Core涓婄殑澶嶆潅鍔犺浇婧㈠嚭鎴栧弻娉垫媶鍒?
                        // x86鐨勬爣閲忔暣鏁板崟鍏冨浜庣Щ浣嶆槸瓒呯骇浼樺寲鐨勩€?
                        uint64_t c0 = combined;
                        uint64_t chunk0 = (c0 & 0x7F) | ((c0 & 0x3F80ULL) << 25);

                        uint64_t c1 = combined >> 14;
                        uint64_t chunk1 = (c1 & 0x7F) | ((c1 & 0x3F80ULL) << 25);

                        uint64_t c2 = combined >> 28;
                        uint64_t chunk2 = (c2 & 0x7F) | ((c2 & 0x3F80ULL) << 25);

                        uint64_t c3 = combined >> 42;
                        uint64_t chunk3 = (c3 & 0x7F) | ((c3 & 0x3F80ULL) << 25);

                        reinterpret_cast<uint64_t *>(&out[i])[0] = chunk0;
                        reinterpret_cast<uint64_t *>(&out[i])[1] = chunk1;
                        reinterpret_cast<uint64_t *>(&out[i])[2] = chunk2;
                        reinterpret_cast<uint64_t *>(&out[i])[3] = chunk3;
                    }
                }
                else if (bit_width_ == 5)
                {
                    size_t bit_pos = (start_idx + i) * 5;
                    for (; i + 8 <= count; i += 8, bit_pos += 40)
                    {
                        _mm_prefetch(reinterpret_cast<const char *>(raw_bytes) + (bit_pos >> 3) + 256, _MM_HINT_T0);

                        size_t word_idx = bit_pos >> 6;
                        size_t bit_ofs = bit_pos & 63;

                        uint64_t w0 = packed_data[word_idx];
                        uint64_t w1 = packed_data[word_idx + 1];

                        uint64_t combined = w0 >> bit_ofs;
                        if (bit_ofs > 24)
                            combined |= (w1 << (64 - bit_ofs));

                        uint64_t c0 = combined;
                        uint64_t chunk0 = (c0 & 0x1F) | ((c0 & 0x3E0ULL) << 27);

                        uint64_t c1 = combined >> 10;
                        uint64_t chunk1 = (c1 & 0x1F) | ((c1 & 0x3E0ULL) << 27);

                        uint64_t c2 = combined >> 20;
                        uint64_t chunk2 = (c2 & 0x1F) | ((c2 & 0x3E0ULL) << 27);

                        uint64_t c3 = combined >> 30;
                        uint64_t chunk3 = (c3 & 0x1F) | ((c3 & 0x3E0ULL) << 27);

                        reinterpret_cast<uint64_t *>(&out[i])[0] = chunk0;
                        reinterpret_cast<uint64_t *>(&out[i])[1] = chunk1;
                        reinterpret_cast<uint64_t *>(&out[i])[2] = chunk2;
                        reinterpret_cast<uint64_t *>(&out[i])[3] = chunk3;
                    }
                }
                else if (bit_width_ == 1 || bit_width_ == 2 || bit_width_ == 4)
                {
                    int elements_per_iter = 32 / bit_width_; // 1bit->32, 2bit->16, 4bit->8 elements
                    __m256i mask_var = _mm256_set1_epi32((1 << bit_width_) - 1);

                    // [澶栨彁甯搁┗鍚戦噺寰幆鍩哄簳]: 閬垮厤鍦ㄥ唴寰幆閲嶅鍙 戝皠 _mm256_set_epi32 杩涜寰寚浠ら噸缁?
                    __m256i aligned_offsets = _mm256_set_epi32(
                        7 * bit_width_, 6 * bit_width_, 5 * bit_width_, 4 * bit_width_,
                        3 * bit_width_, 2 * bit_width_, 1 * bit_width_, 0);

                    size_t bit_pos = (start_idx + i) * bit_width_;
                    int advance = elements_per_iter * bit_width_; // 鍥犱负 32/bit_width锛岃繖鎬绘槸绛変簬 32

                    // 1,2,4 bits 鍒氬ソ鍙互鍒嗗埆涓€娆℃€цВ鍘?32, 16, 8 涓厓绱犮€傚彧闇€鎴彇 32 浣嶇殑缁勫悎鍗冲彲銆?
                    for (; i + elements_per_iter <= count; i += elements_per_iter, bit_pos += advance)
                    {
                        size_t word_idx = bit_pos >> 6;
                        size_t bit_ofs = bit_pos & 63;

                        uint64_t w0 = packed_data[word_idx];
                        uint64_t w1 = packed_data[word_idx + 1];

                        uint64_t combined = w0 >> bit_ofs;
                        if (bit_ofs != 0)
                            combined |= (w1 << (64 - bit_ofs));

                        // 鍙栧嚭鎵€闇€鐨勪綆 32 bits 鍗冲寘鍚杞墍鏈夌殑鍏冪 礌
                        uint32_t low_32 = static_cast<uint32_t>(combined);

#if defined(__AVX512F__) && defined(__AVX512VPOPCNTDQ__)
                        // [鐞嗚鏋舵瀯] 鑻ュ钩鍙版嫢鏈?AVX-512锛屽 1-bit 鍙洿鎺ョ敤鍗曟寚浠ょ‖浠堕伄缃╂墿灞?vpmovm2d)灏嗗叾 16/32 浣嶅悓鏃舵礂鍏ヤ袱涓?zmm (1024 bit 瀹藉害)
                        // 鑻ユ槸 16 涓厓绱? _mm512_maskz_set1_epi32(low_32 & 0xFFFF, 1)
#endif

                        // 浣跨敤 AVX2 鍒嗘壒娆″瓨鍌ㄥ埌杩炵画杈撳嚭锛堟瘡娆″～ 婊′竴鏉?256bit 鍗?8 涓?int32_t锛?
                        for (int chunk = 0; chunk < elements_per_iter; chunk += 8)
                        {
                            int chunk_bit_start = chunk * bit_width_;
                            uint32_t chunk_bits = low_32 >> chunk_bit_start;
                            __m256i v_data_combined = _mm256_set1_epi32(chunk_bits);

                            __m256i shifted = _mm256_srlv_epi32(v_data_combined, aligned_offsets);
                            __m256i result = _mm256_and_si256(shifted, mask_var);
                            _mm256_storeu_si256(reinterpret_cast<__m256i *>(out + i + chunk), result);
                        }
                    }
                }
#endif

                uint64_t mask = (1ULL << bit_width_) - 1;
                size_t bit_pos = (start_idx + i) * bit_width_;
                // [鎸囦护缂撳瓨鏀剁缉]: E-core L1i 杈冨皬锛屾斁寮冩墜宸ョ矖鏆村睍寮€锛屾敼涓虹煭灏忕簿鎮嶇殑鍐呰仈寰幆銆?
                // 缂栬瘧鍣紙鐢氳嚦纭欢鍓嶇瑙ｇ爜鍣級浼氬绮剧畝鐨勫惊鐜嚜 鍔ㄨ繘琛?Micro-op 铻嶅悎锛?
                for (; i < count; ++i, bit_pos += bit_width_)
                {
                    // [杞欢棰勫彇鎸囦护] 鎸囩ず鍐呭瓨鎺у埗鍣ㄥ噯澶囨暟鎹?
                    if ((i & 31) == 0)
                    {
                        _mm_prefetch(reinterpret_cast<const char *>(raw_bytes) + (bit_pos >> 3) + 256, _MM_HINT_T0);
                    }
                    size_t word_idx = bit_pos >> 6;
                    size_t bit_ofs = bit_pos & 63;

                    uint64_t w0 = packed_data[word_idx];
                    uint64_t w1 = packed_data[word_idx + 1];

                    uint64_t combined = w0 >> bit_ofs;
                    if (bit_ofs != 0)
                        combined |= (w1 << (64 - bit_ofs));

                    out[i] = static_cast<int32_t>(combined & mask);
                }
            return out;
        }
        }
    };

    class ColumnarTable
    {
    public:
        ColumnarTable(const Schema *schema) : schema_(schema), num_rows_(0)
        {
            uint32_t col_count = schema_->GetColumnCount();
            columns_.resize(col_count);
        }

        // 鍏佽鍔ㄦ€佷负瀛楁鍒嗛厤鍘嬬缉缂栫爜鍗忚
        void SetColumnEncoding(uint32_t col_idx, EncodingType type, int bit_width = 32)
        {
            if (col_idx < columns_.size() && type == EncodingType::BIT_PACKED)
            {
                columns_[col_idx].InitBitPacked(bit_width);
            }
        }

        void Reserve(size_t capacity)
        {
            std::lock_guard<std::mutex> lock(latch_);
            rows_.reserve(capacity);
            for (auto &col : columns_)
            {
                col.Reserve(capacity);
            }
        }

        bool InsertTuple(const TupleMeta &meta, const Tuple &tuple, RID *rid)
        {
            std::lock_guard<std::mutex> lock(latch_);

            size_t current_row = num_rows_.load(std::memory_order_relaxed);
            rows_.push_back(tuple);
            uint32_t col_count = schema_->GetColumnCount();
            for (uint32_t i = 0; i < col_count; ++i)
            {
                Value value = tuple.GetValue(schema_, i);
                int32_t raw_val = value.GetTypeId() == TypeId::INTEGER ? value.GetAs<int32_t>() : 0;
                columns_[i].Append(raw_val, current_row);
            }

            if (rid != nullptr)
            {
                rid->Set(0, static_cast<uint32_t>(current_row));
            }

            num_rows_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        void InsertBatch(const std::vector<const int32_t *> &columns_data, size_t count)
        {
            std::lock_guard<std::mutex> lock(latch_);
            size_t start_row = num_rows_.load(std::memory_order_relaxed);

            uint32_t col_count = columns_.size();
            auto &thread_pool = ThreadPool::Instance();
            std::vector<std::future<void>> futures;

            for (uint32_t i = 0; i < col_count; ++i)
            {
                futures.push_back(thread_pool.Enqueue([this, i, &columns_data, start_row, count]()
                                                      { columns_[i].AppendBatch(columns_data[i], start_row, count); }));
            }
            for (auto &f : futures)
            {
                f.wait();
            }
            num_rows_.fetch_add(count, std::memory_order_relaxed);
        }

        size_t GetRowCount() const { return num_rows_.load(std::memory_order_relaxed); }

        const int32_t* UnpackColumnBatch(uint32_t col_idx, size_t start_row, size_t count, int32_t *out_buffer) const
        {
            return columns_[col_idx].UnpackBatch(start_row, count, out_buffer);
        }
        // 鐢ㄤ簬鍗曡鏌ヨ鐨勯珮鏁堟帴鍙?
        int32_t GetValue(uint32_t col_idx, size_t row_idx) const
        {
            return columns_[col_idx].GetValue(row_idx);
        }
        // ==== Row-based Iterator API for Compatibility ====

        bool GetTuple(const RID &rid, Tuple *tuple) const
        {
            uint32_t row_idx = rid.GetSlotId();
            if (row_idx >= num_rows_.load(std::memory_order_relaxed))
                return false;
            if (!rows_.empty())
            {
                *tuple = rows_[row_idx];
                tuple->SetRID(rid);
                return true;
            }

            std::vector<Value> values;
            for (size_t i = 0; i < schema_->GetColumnCount(); ++i)
            {
                values.push_back(Value(columns_[i].GetValue(row_idx)));
            }
            *tuple = Tuple(values, schema_);
            return true;
        }

        bool UpdateTuple(const TupleMeta &meta, const Tuple &tuple, const RID &rid)
        {
            uint32_t row_idx = rid.GetSlotId();
            if (row_idx >= num_rows_.load(std::memory_order_relaxed))
                return false;

            std::lock_guard<std::mutex> lock(latch_);
            if (row_idx < rows_.size())
            {
                rows_[row_idx] = tuple;
            }
            for (size_t i = 0; i < schema_->GetColumnCount(); ++i)
            {
                Value value = tuple.GetValue(schema_, i);
                int32_t raw_val = value.GetTypeId() == TypeId::INTEGER ? value.GetAs<int32_t>() : 0;
                columns_[i].SetValue(row_idx, raw_val);
            }
            return true;
        }

        bool UpdateTupleMeta(const TupleMeta &meta, const RID &rid) { return true; }
        bool MarkDelete(const RID &rid, Transaction *txn) { return true; }
        bool ApplyDelete(const RID &rid, Transaction *txn) { return true; }

        class TableIterator
        {
        public:
            TableIterator(const ColumnarTable *table, size_t row_idx) : table_(table), row_idx_(row_idx) {}

            bool IsEnd() const { return row_idx_ >= table_->GetRowCount(); }

            const Tuple operator*() const
            {
                Tuple tuple;
                table_->GetTuple(RID(0, static_cast<uint32_t>(row_idx_)), &tuple);
                return tuple;
            }

            TableIterator &operator++()
            {
                row_idx_++;
                return *this;
            }

            bool operator==(const TableIterator &other) const { return row_idx_ == other.row_idx_; }
            bool operator!=(const TableIterator &other) const { return !(*this == other); }

            RID GetRID() const { return RID(0, static_cast<uint32_t>(row_idx_)); }

        private:
            const ColumnarTable *table_;
            size_t row_idx_;
        };

        TableIterator MakeIterator() const
        {
            return TableIterator(this, 0);
        }

        TableIterator MakeEofIterator() const
        {
            return TableIterator(this, GetRowCount());
        }

    private:
        const Schema *schema_;
        std::vector<ColumnChunk> columns_;
        std::vector<Tuple> rows_;
        std::atomic<size_t> num_rows_;
        std::mutex latch_;
    };

} // namespace Database
