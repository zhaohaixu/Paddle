// Copyright (c) 2023 PaddlePaddle Authors. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <cmath>
#include <vector>

#include "cutlass/bfloat16.h"
#include "cutlass/fast_math.h"
#include "cutlass/gemm/gemm.h"
#include "cutlass/layout/matrix.h"
#include "cutlass/layout/vector.h"
#include "cutlass/matrix.h"
#include "cutlass/numeric_types.h"
#include "cutlass/tensor_ref.h"

#include <inttypes.h>
#include "cutlass/epilogue/threadblock/default_epilogue_simt.h"
#include "cutlass/epilogue/threadblock/default_epilogue_tensor_op.h"
#include "cutlass/epilogue/threadblock/default_epilogue_volta_tensor_op.h"
#include "cutlass/gemm/device/default_gemm_configuration.h"
#include "cutlass/gemm/kernel/default_gemm.h"
#include "cutlass/gemm/threadblock/default_mma.h"
#include "cutlass/gemm/threadblock/default_mma_core_simt.h"
#include "cutlass/gemm/threadblock/default_mma_core_sm70.h"
#include "cutlass/gemm/threadblock/default_mma_core_sm75.h"
#include "cutlass/gemm/threadblock/default_mma_core_sm80.h"
#include "cutlass/gemm/threadblock/threadblock_swizzle.h"
#include "cutlass/matrix_shape.h"
#include "cutlass/platform/platform.h"
#include "cutlass/transform/threadblock/predicated_tile_iterator.h"
#include "debug_utils.h"
#include "epilogue/epilogue_pipelined.h"
#include "epilogue/epilogue_rescale_output.h"
#include "gemm/find_default_mma.h"
#include "gemm/mma_from_smem.h"
#include "gemm_kernel_utils.h"
#include "transform/tile_smem_loader.h"

using namespace gemm_kernel_utils;

namespace {
template <typename scalar_t, typename Arch>
constexpr int getWarpsPerSm() {
  return (Arch::kMinComputeCapability >= 80 &&
                  !cutlass::platform::is_same<scalar_t, float>::value
              ? 16
              : 12);
}
static CUTLASS_DEVICE float atomicMaxFloat(float* addr, float value) {
  // source: https://stackoverflow.com/a/51549250
  return (value >= 0)
             ? __int_as_float(atomicMax((int*)addr, __float_as_int(value)))
             : __uint_as_float(
                   atomicMin((unsigned int*)addr, __float_as_uint(value)));
}
}  // namespace

template <
    // The datatype of Q/K/V
    typename scalar_t_,
    // Architecture we are targeting (eg `cutlass::arch::Sm80`)
    typename ArchTag,
    // If Q/K/V are correctly aligned in memory and we can run a fast kernel
    bool isAligned_,
    bool maskIsAligned_,
    int kQueriesPerBlock,
    int kKeysPerBlock,
    bool kSingleValueIteration,  // = `value.shape[-1] <= kKeysPerBlock`
    bool kAddMask,
    // This is quite faster when mask need broadcast at row axis
    bool kMaskBroadcastRow = true>
struct AttentionKernel {
  using scalar_t = scalar_t_;
  using accum_t = float;

  using lse_scalar_t = float;
  using output_t = scalar_t;
  // Accumulator between 2 iterations
  // Using `accum_t` improves perf on f16 at the cost of
  // numerical errors
  using output_accum_t = accum_t;
  static constexpr bool kIsAligned = isAligned_;
  static constexpr bool kMaskIsAligned = maskIsAligned_;

  static constexpr int32_t kAlignLSE = 32;  // block size of backward
  static constexpr bool kPreloadV = ArchTag::kMinComputeCapability >= 80 &&
                                    cutlass::sizeof_bits<scalar_t>::value == 16;
  static constexpr bool kKeepOutputInRF = kSingleValueIteration;
  static constexpr bool kNeedsOutputAccumulatorBuffer =
      !kKeepOutputInRF &&
      !cutlass::platform::is_same<output_accum_t, output_t>::value;

  static_assert(kQueriesPerBlock % 32 == 0, "");
  static_assert(kKeysPerBlock % 32 == 0, "");
  static constexpr int kNumWarpsPerBlock =
      kQueriesPerBlock * kKeysPerBlock / (32 * 32);
  static constexpr int kWarpSize = 32;

  // Launch bounds
  static constexpr int kNumThreads = kWarpSize * kNumWarpsPerBlock;
  static constexpr int kMinBlocksPerSm =
      getWarpsPerSm<scalar_t, ArchTag>() / kNumWarpsPerBlock;

  struct Params {
    // Input tensors
    scalar_t* query_ptr;           // [num_queries, num_heads, head_dim]
    scalar_t* key_ptr;             // [num_keys, num_heads, head_dim]
    scalar_t* value_ptr;           // [num_keys, num_heads, head_dim_value]
    scalar_t* mask_ptr = nullptr;  // [num_heads, num_queries, num_keys]
    int32_t* cu_seqlens_q_ptr = nullptr;
    int32_t* cu_seqlens_k_ptr = nullptr;

    // Output tensors
    output_t* output_ptr;  // [num_queries, num_heads, head_dim_value]
    output_accum_t*
        output_accum_ptr;         // [num_queries, num_heads, head_dim_value]
    lse_scalar_t* logsumexp_ptr;  // [num_heads, num_queries] - can be null

    // Scale
    accum_t scale;

    // Dimensions/strides
    int32_t head_dim;
    int32_t head_dim_value;
    int32_t num_queries;
    int32_t num_keys;

    bool causal;
    bool mask_broadcast_row;

    int32_t q_strideM;
    int32_t k_strideM;
    int32_t v_strideM;
    int32_t mask_strideM = 0;

    // Everything below is only used in `advance_to_block`
    // and shouldn't use registers
    int32_t q_strideH;
    int32_t k_strideH;
    int32_t v_strideH;
    int32_t o_strideH = 0;
    int32_t mask_strideH = 0;

    int64_t q_strideB;
    int64_t k_strideB;
    int64_t v_strideB;
    int64_t o_strideB;
    int32_t mask_strideB = 0;

    int32_t num_batches;
    int32_t num_heads;

    CUTLASS_HOST_DEVICE int32_t o_strideM() const {
      // return head_dim_value * num_heads;
      return head_dim_value;
    }
    // Moves pointers to what we should process
    // Returns "false" if there is no work to do
    CUTLASS_DEVICE bool advance_to_block() {
      auto batch_id = blockIdx.z;
      auto head_id = blockIdx.y;
      auto query_start = blockIdx.x * kQueriesPerBlock;

      auto lse_dim = ceil_div((int32_t)num_queries, kAlignLSE) * kAlignLSE;

      int64_t q_start, k_start;
      // Advance to current batch - in case of different sequence lengths
      if (cu_seqlens_q_ptr != nullptr) {
        assert(cu_seqlens_k_ptr != nullptr);
        cu_seqlens_q_ptr += batch_id;
        cu_seqlens_k_ptr += batch_id;
        q_start = cu_seqlens_q_ptr[0];
        k_start = cu_seqlens_k_ptr[0];
        int64_t q_next_start = cu_seqlens_q_ptr[1];
        int64_t k_next_start = cu_seqlens_k_ptr[1];
        num_queries = q_next_start - q_start;
        num_keys = k_next_start - k_start;

        if (query_start >= num_queries) {
          return false;
        }
      } else {
        query_ptr += batch_id * q_strideB;
        key_ptr += batch_id * k_strideB;
        value_ptr += batch_id * v_strideB;
        // output_ptr += int64_t(batch_id * num_queries) * o_strideM();
        output_ptr += batch_id * o_strideB;
        ;

        if (output_accum_ptr != nullptr) {
          // output_accum_ptr += int64_t(batch_id * num_queries) * o_strideM();
          output_accum_ptr += batch_id * o_strideB;
        }
        q_start = 0;
        k_start = 0;
      }

      // Advance to the current batch / head / query_start
      query_ptr += (q_start + query_start) * q_strideM + head_id * q_strideH;
      key_ptr += k_start * k_strideM + head_id * k_strideH;
      value_ptr += k_start * v_strideM + head_id * v_strideH;
      // output_ptr += int64_t(q_start + query_start) * o_strideM() +
      //     head_id * head_dim_value;
      output_ptr +=
          int64_t(q_start + query_start) * o_strideM() + head_id * o_strideH;

      if (mask_ptr != nullptr) {
        mask_ptr += (batch_id * mask_strideB) + (head_id * mask_strideH);
      }
      if (output_accum_ptr != nullptr) {
        // output_accum_ptr += int64_t(q_start + query_start) * o_strideM() +
        //     head_id * head_dim_value;

        output_accum_ptr +=
            int64_t(q_start + query_start) * o_strideM() + head_id * o_strideH;
      } else {
        // Accumulate directly in the destination buffer (eg for f32)
        output_accum_ptr = (accum_t*)output_ptr;
      }
      if (logsumexp_ptr != nullptr) {
        // lse[batch_id, head_id, query_start]
        logsumexp_ptr +=
            batch_id * lse_dim * num_heads + head_id * lse_dim + query_start;
      }

      num_queries -= query_start;
      if (causal) {
        num_keys = cutlass::fast_min(int32_t(query_start + kQueriesPerBlock),
                                     num_keys);
      }
      num_batches = 0;  // no longer used after

      // Make sure the compiler knows these variables are the same on all
      // the threads of the warp.
      query_ptr = warp_uniform(query_ptr);
      key_ptr = warp_uniform(key_ptr);
      value_ptr = warp_uniform(value_ptr);
      mask_ptr = warp_uniform(mask_ptr);
      output_ptr = warp_uniform(output_ptr);
      output_accum_ptr = warp_uniform(output_accum_ptr);
      logsumexp_ptr = warp_uniform(logsumexp_ptr);
      num_queries = warp_uniform(num_queries);
      num_keys = warp_uniform(num_keys);
      head_dim = warp_uniform(head_dim);
      head_dim_value = warp_uniform(head_dim_value);
      return true;
    }

    __host__ dim3 getBlocksGrid() const {
      return dim3(ceil_div(num_queries, (int32_t)kQueriesPerBlock),
                  num_heads,
                  num_batches);
    }
    __host__ dim3 getThreadsGrid() const {
      return dim3(kWarpSize, kNumWarpsPerBlock, 1);
    }
  };

  struct MM0 {
    /*
      In this first matmul, we compute a block of `Q @ K.T`.
      While the calculation result is still hot in registers, we update
      `mi`, `m_prime`, `s_prime` in shared-memory, and then store this value
      into a shared-memory ("AccumulatorSharedStorage") that is used later as
      operand A for the second matmul (see MM1)
    */
    using GemmType = DefaultGemmType<ArchTag, scalar_t, kIsAligned>;

    using OpClass = typename GemmType::OpClass;
    using DefaultConfig =
        typename cutlass::gemm::device::DefaultGemmConfiguration<
            OpClass,
            ArchTag,
            scalar_t,
            scalar_t,
            scalar_t,  // ElementC
            accum_t    // ElementAccumulator
            >;
    static constexpr int kAlignmentA =
        kIsAligned ? DefaultConfig::kAlignmentA : GemmType::kMinimumAlignment;
    static constexpr int kAlignmentB =
        kIsAligned ? DefaultConfig::kAlignmentB : GemmType::kMinimumAlignment;
    using ThreadblockShape = cutlass::gemm::
        GemmShape<kQueriesPerBlock, kKeysPerBlock, GemmType::ThreadK>;
    using WarpShape = cutlass::gemm::GemmShape<32, 32, GemmType::WarpK>;
    using DefaultMma = typename cutlass::gemm::threadblock::FindDefaultMma<
        scalar_t,                   // ElementA,
        cutlass::layout::RowMajor,  // LayoutA,
        kAlignmentA,
        scalar_t,                      // ElementB,
        cutlass::layout::ColumnMajor,  // LayoutB,
        kAlignmentB,
        accum_t,
        cutlass::layout::RowMajor,  // LayoutC,
        OpClass,
        ArchTag,                              // ArchTag
        ThreadblockShape,                     // ThreadblockShape
        WarpShape,                            // WarpShape
        typename GemmType::InstructionShape,  // InstructionShape
        DefaultConfig::kStages,      // Should use `DefaultConfig::kStages`, but
                                     // that uses too much smem
        typename GemmType::Operator  // Operator
        >::DefaultMma;
    using MmaCore = typename DefaultMma::MmaCore;
    using IteratorA = typename DefaultMma::IteratorA;
    using IteratorB = typename DefaultMma::IteratorB;
    using Mma = typename DefaultMma::ThreadblockMma;
    using AccumLambdaIterator = typename DefaultMmaAccumLambdaIterator<
        typename Mma::Operator::IteratorC,
        accum_t,
        kWarpSize>::Iterator;
    static_assert(MmaCore::WarpCount::kM * MmaCore::WarpCount::kN *
                          MmaCore::WarpCount::kK ==
                      kNumWarpsPerBlock,
                  "");

    // used for efficient load of mask_ tile Bij from global to shared memory
    using MaskLoader = TileSmemLoader<
        scalar_t,
        cutlass::MatrixShape<kQueriesPerBlock, kKeysPerBlock>,
        MmaCore::kThreads,
        // input restriction: kv_len has to be a multiple of this value
        kMaskIsAligned ? 128 / cutlass::sizeof_bits<scalar_t>::value : 1>;

    // Epilogue to store to shared-memory in a format that we can use later for
    // the second matmul
    using B2bGemm = typename cutlass::gemm::threadblock::B2bGemm<
        typename Mma::Operator::IteratorC,
        typename Mma::Operator,
        scalar_t,
        WarpShape,
        ThreadblockShape>;
    using AccumulatorSharedStorage = typename B2bGemm::AccumulatorSharedStorage;
  };

  struct MM1 {
    /**
      Second matmul: perform `attn @ V` where `attn` is the attention (not
      normalized) and stored in shared memory
    */
    using GemmType = DefaultGemmType<ArchTag, scalar_t, kIsAligned>;

    using OpClass = typename GemmType::OpClass;
    using DefaultConfig =
        typename cutlass::gemm::device::DefaultGemmConfiguration<
            OpClass,
            ArchTag,
            scalar_t,
            scalar_t,
            output_accum_t,  // ElementC
            accum_t          // ElementAccumulator
            >;
    static constexpr int kAlignmentA = DefaultConfig::kAlignmentA;  // from smem
    static constexpr int kAlignmentB =
        kIsAligned ? DefaultConfig::kAlignmentB : GemmType::kMinimumAlignment;
    using ThreadblockShape = cutlass::gemm::
        GemmShape<kQueriesPerBlock, kKeysPerBlock, GemmType::ThreadK>;
    using WarpShape = cutlass::gemm::GemmShape<32, 32, GemmType::WarpK>;
    using InstructionShape = typename GemmType::InstructionShape;

    using LayoutB = cutlass::layout::RowMajor;
    using DefaultGemm = cutlass::gemm::kernel::DefaultGemm<
        scalar_t,                   // ElementA,
        cutlass::layout::RowMajor,  // LayoutA,
        kAlignmentA,
        scalar_t,  // ElementB,
        LayoutB,   // LayoutB,
        kAlignmentB,
        output_accum_t,
        cutlass::layout::RowMajor,  // LayoutC,
        accum_t,
        OpClass,
        ArchTag,
        ThreadblockShape,
        WarpShape,
        typename GemmType::InstructionShape,
        typename DefaultConfig::EpilogueOutputOp,
        void,  // ThreadblockSwizzle - not used
        DefaultConfig::kStages,
        false,  // SplitKSerial
        typename GemmType::Operator>;

    using DefaultMmaFromSmem =
        typename cutlass::gemm::threadblock::DefaultMmaFromSharedMemory<
            typename DefaultGemm::Mma,
            typename MM0::AccumulatorSharedStorage,
            false>;  // kScaleOperandA
    using Mma = typename DefaultMmaFromSmem::Mma;
    using IteratorB = typename Mma::IteratorB;
    using WarpCount = typename Mma::WarpCount;
    static_assert(WarpCount::kM * WarpCount::kN * WarpCount::kK ==
                      kNumWarpsPerBlock,
                  "");

    using DefaultEpilogue = typename DefaultGemm::Epilogue;
    using OutputTileIterator =
        typename cutlass::epilogue::threadblock::PredicatedTileIterator<
            typename DefaultEpilogue::OutputTileIterator::ThreadMap,
            output_t>;
    using OutputTileIteratorAccum =
        typename cutlass::epilogue::threadblock::PredicatedTileIterator<
            typename DefaultEpilogue::OutputTileIterator::ThreadMap,
            output_accum_t>;

    struct SharedStorageMM1 {
      typename Mma::SharedStorage mm;
    };
  };

  static constexpr int64_t kAlignmentQ = MM0::kAlignmentA;
  static constexpr int64_t kAlignmentK = MM0::kAlignmentB;
  static constexpr int64_t kAlignmentV = 1;
  static constexpr int64_t kAlignmentM = kMaskIsAligned ? kAlignmentQ : 1;

  // Shared storage - depends on kernel params
  struct ScalingCoefs {
    cutlass::Array<accum_t, kQueriesPerBlock> m_prime;
    cutlass::Array<accum_t, kQueriesPerBlock> s_prime;
    cutlass::Array<accum_t, kQueriesPerBlock> mi;
    cutlass::Array<accum_t, kQueriesPerBlock * MM0::MmaCore::WarpCount::kN>
        addition_storage;
  };

  struct SharedStorageEpilogueAtEnd : ScalingCoefs {
    struct SharedStorageAfterMM0 {
      // Everything here might be overwritten during MM0
      union {
        typename MM0::MaskLoader::SmemTile mask;
        typename MM0::AccumulatorSharedStorage si;
      };
      typename MM1::SharedStorageMM1 mm1;
    };

    union {
      typename MM0::Mma::SharedStorage mm0;
      SharedStorageAfterMM0 after_mm0;
      typename MM1::DefaultEpilogue::SharedStorage epilogue;
    };

    CUTLASS_DEVICE typename MM1::DefaultEpilogue::SharedStorage&
    epilogue_shared_storage() {
      return epilogue;
    }
  };

  struct SharedStorageEpilogueInLoop : ScalingCoefs {
    struct SharedStorageAfterMM0 {
      // Everything here might be overwritten during MM0
      union {
        typename MM0::MaskLoader::SmemTile mask;
        typename MM0::AccumulatorSharedStorage si;
      };
      typename MM1::SharedStorageMM1 mm1;
      typename MM1::DefaultEpilogue::SharedStorage epilogue;
    };

    union {
      typename MM0::Mma::SharedStorage mm0;
      SharedStorageAfterMM0 after_mm0;
    };

    CUTLASS_DEVICE typename MM1::DefaultEpilogue::SharedStorage&
    epilogue_shared_storage() {
      return after_mm0.epilogue;
    }
  };

  using SharedStorage = typename cutlass::platform::conditional<
      kSingleValueIteration || kKeepOutputInRF,
      SharedStorageEpilogueAtEnd,
      SharedStorageEpilogueInLoop>::type;

  static bool __host__ check_supported(Params const& p) {
    CHECK_ALIGNED_PTR(p.query_ptr, kAlignmentQ);
    CHECK_ALIGNED_PTR(p.key_ptr, kAlignmentK);
    CHECK_ALIGNED_PTR(p.value_ptr, kAlignmentV);
    CHECK_ALIGNED_PTR(p.mask_ptr, kAlignmentM);
    if (p.mask_ptr != nullptr) {
      CHECK_ALIGNED_PTR(p.mask_ptr, kAlignmentM);
      XFORMERS_CHECK(p.mask_strideB % kAlignmentM == 0,
                     "attn_mask is not correctly aligned");
      XFORMERS_CHECK(p.mask_strideH % kAlignmentM == 0,
                     "attn_mask is not correctly aligned");
      XFORMERS_CHECK(p.mask_strideM % kAlignmentM == 0,
                     "attn_mask is not correctly aligned");
    }
    XFORMERS_CHECK(p.q_strideM % kAlignmentQ == 0,
                   "query is not correctly aligned");
    XFORMERS_CHECK(p.k_strideM % kAlignmentK == 0,
                   "key is not correctly aligned");
    XFORMERS_CHECK(p.v_strideM % kAlignmentV == 0,
                   "value is not correctly aligned");
    XFORMERS_CHECK(p.q_strideH % kAlignmentQ == 0,
                   "query is not correctly aligned");
    XFORMERS_CHECK(p.k_strideH % kAlignmentK == 0,
                   "key is not correctly aligned");
    XFORMERS_CHECK(p.v_strideH % kAlignmentV == 0,
                   "value is not correctly aligned");
    return true;
  }

  static void CUTLASS_DEVICE attention_kernel(Params& p) {
    // In this block, we will only ever:
    // - read query[query_start:query_end, :]
    // - write to output[query_start:query_end, :]

    extern __shared__ char smem_buffer[];
    SharedStorage& shared_storage = *((SharedStorage*)smem_buffer);
    auto& m_prime = shared_storage.m_prime;
    auto& s_prime = shared_storage.s_prime;
    auto& si = shared_storage.after_mm0.si;
    auto& mi = shared_storage.mi;
    const uint32_t query_start = blockIdx.x * kQueriesPerBlock;

    static_assert(kQueriesPerBlock < kNumWarpsPerBlock * kWarpSize, "");
    if (thread_id() < kQueriesPerBlock) {
      s_prime[thread_id()] = accum_t(0);
      m_prime[thread_id()] =
          -cutlass::platform::numeric_limits<accum_t>::infinity();
      mi[thread_id()] = -cutlass::platform::numeric_limits<accum_t>::infinity();
    }
    typename MM1::Mma::FragmentC accum_o;
    accum_o.clear();

    auto createOutputIter = [&](int col) -> typename MM1::OutputTileIterator {
      using OutputTileIterator = typename MM1::OutputTileIterator;
      return OutputTileIterator(
          typename OutputTileIterator::Params{(int32_t)p.o_strideM()},
          p.output_ptr,
          typename OutputTileIterator::TensorCoord{p.num_queries,
                                                   p.head_dim_value},
          thread_id(),
          {0, col});
    };

    auto createOutputAccumIter = [&](int col) ->
        typename MM1::OutputTileIteratorAccum {
          using OutputTileIteratorAccum = typename MM1::OutputTileIteratorAccum;
          return OutputTileIteratorAccum(
              typename OutputTileIteratorAccum::Params{(int32_t)p.o_strideM()},
              p.output_accum_ptr,
              typename OutputTileIteratorAccum::TensorCoord{p.num_queries,
                                                            p.head_dim_value},
              thread_id(),
              {0, col});
        };

    // Iterate through keys
    for (int32_t iter_key_start = 0; iter_key_start < p.num_keys;
         iter_key_start += kKeysPerBlock) {
      int32_t problem_size_0_m =
          cutlass::fast_min((int32_t)kQueriesPerBlock, p.num_queries);
      int32_t problem_size_0_n = cutlass::fast_min(int32_t(kKeysPerBlock),
                                                   p.num_keys - iter_key_start);
      int32_t const& problem_size_0_k = p.head_dim;
      int32_t const& problem_size_1_n = p.head_dim_value;
      int32_t const& problem_size_1_k = problem_size_0_n;

      auto prologueV = [&](int blockN) {
        typename MM1::Mma::IteratorB iterator_V(
            typename MM1::IteratorB::Params{MM1::LayoutB(p.v_strideM)},
            p.value_ptr + iter_key_start * p.v_strideM,
            {problem_size_1_k, problem_size_1_n},
            thread_id(),
            cutlass::MatrixCoord{0, blockN * MM1::Mma::Shape::kN});
        MM1::Mma::prologue(shared_storage.after_mm0.mm1.mm,
                           iterator_V,
                           thread_id(),
                           problem_size_1_k);
      };

      __syncthreads();  // Need to have shared memory initialized, and `m_prime`
                        // updated from end of prev iter
      //
      // MATMUL: Q.K_t
      //
      // Computes the block-matrix product of:
      // (a) query[query_start:query_end, :]
      // with
      // (b) key[iter_key_start:iter_key_start + kKeysPerBlock]
      // and stores that into `shared_storage.si`
      //

      // Compute threadblock location
      cutlass::gemm::GemmCoord tb_tile_offset = {0, 0, 0};

      cutlass::MatrixCoord tb_offset_A{tb_tile_offset.m() * MM0::Mma::Shape::kM,
                                       tb_tile_offset.k()};

      cutlass::MatrixCoord tb_offset_B{
          tb_tile_offset.k(), tb_tile_offset.n() * MM0::Mma::Shape::kN};

      // Construct iterators to A and B operands
      typename MM0::IteratorA iterator_A(
          typename MM0::IteratorA::Params(
              typename MM0::MmaCore::LayoutA(p.q_strideM)),
          p.query_ptr,
          {problem_size_0_m, problem_size_0_k},
          thread_id(),
          tb_offset_A);

      typename MM0::IteratorB iterator_B(
          typename MM0::IteratorB::Params(
              typename MM0::MmaCore::LayoutB(p.k_strideM)),
          p.key_ptr + iter_key_start * p.k_strideM,
          {problem_size_0_k, problem_size_0_n},
          thread_id(),
          tb_offset_B);

      auto my_warp_id = warp_id();
      auto my_lane_id = lane_id();

      // Construct thread-scoped matrix multiply
      typename MM0::Mma mma(
          shared_storage.mm0, thread_id(), my_warp_id, my_lane_id);

      typename MM0::Mma::FragmentC accum;

      accum.clear();

      auto gemm_k_iterations =
          (problem_size_0_k + MM0::Mma::Shape::kK - 1) / MM0::Mma::Shape::kK;

      // Compute threadblock-scoped matrix multiply-add
      mma(gemm_k_iterations, accum, iterator_A, iterator_B, accum);
      __syncthreads();

      if (kPreloadV) {
        prologueV(0);
      }

      typename MM0::Mma::Operator::IteratorC::TensorCoord
          iteratorC_tile_offset = {
              (tb_tile_offset.m() * MM0::Mma::WarpCount::kM) +
                  (my_warp_id % MM0::Mma::WarpCount::kM),
              (tb_tile_offset.n() * MM0::Mma::WarpCount::kN) +
                  (my_warp_id / MM0::Mma::WarpCount::kM)};

      if (kAddMask) {
        accum =
            cutlass::multiplies<typename MM0::Mma::FragmentC>()(p.scale, accum);
      }

      int32_t mask_iter_m = kMaskBroadcastRow ? 1 : problem_size_0_m;

      // apply attention mask if applicable
      if (kAddMask) {
        // load mask tile Bij into shared memory
        typename MM0::MaskLoader::GmemTileIterator mask_iter(
            {cutlass::layout::RowMajor(p.mask_strideM)},
            // attn_mask_pointer points to matrix of size (n_queries, n_keys)
            // for the relevant batch_id and head_id
            p.mask_ptr + query_start * p.mask_strideM + iter_key_start,
            {mask_iter_m, problem_size_0_n},
            thread_id());

        cutlass::TensorRef<scalar_t, cutlass::layout::RowMajor> mask_tensor_ref(
            shared_storage.after_mm0.mask.data(),
            cutlass::layout::RowMajor(MM0::ThreadblockShape::kN));
        typename MM0::MaskLoader::SmemTileIterator smem_tile_iter(
            mask_tensor_ref, thread_id());
        MM0::MaskLoader::load(mask_iter, smem_tile_iter);

        // Pij += Bij, Pij is in register fragment and Bij is in shared memory
        auto lane_offset = MM0::AccumLambdaIterator::get_lane_offset(
            lane_id(), warp_id(), iteratorC_tile_offset);

        MM0::AccumLambdaIterator::iterateRows(
            lane_offset,
            [&](int accum_m) {},
            [&](int accum_m, int accum_n, int idx) {
              if (accum_m < problem_size_0_m && accum_n < problem_size_0_n) {
                if (kMaskBroadcastRow) {
                  accum[idx] += mask_tensor_ref.at({0, accum_n});
                } else {
                  accum[idx] += mask_tensor_ref.at({accum_m, accum_n});
                }
              }
            },
            [&](int accum_m) {});
      }

      // Mask out last if causal
      if (p.causal && p.num_keys - iter_key_start <= kKeysPerBlock) {
        auto query_start = blockIdx.x * kQueriesPerBlock;
        auto lane_offset = MM0::AccumLambdaIterator::get_lane_offset(
            lane_id(), warp_id(), iteratorC_tile_offset);
        int32_t last_col;
        MM0::AccumLambdaIterator::iterateRows(
            lane_offset,
            [&](int accum_m) {
              last_col = query_start + accum_m - iter_key_start;
            },
            [&](int accum_m, int accum_n, int idx) {
              if (accum_n > last_col) {
                accum[idx] =
                    -cutlass::platform::numeric_limits<accum_t>::infinity();
              }
            },
            [&](int accum_m) {});
      }
      DISPATCH_BOOL(
          iter_key_start == 0, kIsFirst, ([&] {
            DISPATCH_BOOL(
                p.num_keys - iter_key_start >= kKeysPerBlock,
                kFullColumns,
                ([&] {
                  // Update `mi` from accum stored in registers
                  // Also does accum[i] <- exp(accum[i] - mi)
                  iterative_softmax<typename MM0::Mma::Operator::IteratorC,
                                    kFullColumns,
                                    kIsFirst,
                                    kKeepOutputInRF>(
                      accum_o,
                      accum,
                      mi,
                      m_prime,
                      s_prime,
                      shared_storage.addition_storage,
                      lane_id(),
                      thread_id(),
                      warp_id(),
                      p.num_keys - iter_key_start,
                      iteratorC_tile_offset,
                      // (p.mask_ptr != nullptr) ? 1.0f : p.scale);
                      kAddMask ? 1.0f : p.scale);
                  // 1.0f);
                }));
          }));

      // Output results to shared-memory
      int warp_idx_mn_0 = my_warp_id % (MM0::Mma::Base::WarpCount::kM *
                                        MM0::Mma::Base::WarpCount::kN);
      auto output_tile_coords =
          cutlass::MatrixCoord{warp_idx_mn_0 % MM0::Mma::Base::WarpCount::kM,
                               warp_idx_mn_0 / MM0::Mma::Base::WarpCount::kM};

      MM0::B2bGemm::accumToSmem(
          shared_storage.after_mm0.si, accum, my_lane_id, output_tile_coords);

      __syncthreads();

      //
      // MATMUL: Attn . V
      // Run the matmul `attn @ V` for a block of attn and V.
      // `attn` is read from shared memory (in `shared_storage_si`)
      // `V` is read from global memory (with iterator_B)
      //

      const int64_t nBlockN =
          kSingleValueIteration ? 1
                                : ceil_div((int64_t)problem_size_1_n,
                                           int64_t(MM1::ThreadblockShape::kN));
      for (int blockN = 0; blockN < nBlockN; ++blockN) {
        int gemm_k_iterations =
            (problem_size_1_k + MM1::Mma::Shape::kK - 1) / MM1::Mma::Shape::kK;

        // Compute threadblock-scoped matrix multiply-add and store it in accum
        // (in registers)
        if (!kPreloadV) {
          __syncthreads();  // we share shmem between mma and epilogue
        }

        typename MM1::Mma::IteratorB iterator_V(
            typename MM1::IteratorB::Params{MM1::LayoutB(p.v_strideM)},
            p.value_ptr + iter_key_start * p.v_strideM,
            {problem_size_1_k, problem_size_1_n},
            thread_id(),
            cutlass::MatrixCoord{0, blockN * MM1::Mma::Shape::kN});
        typename MM1::Mma mma_pv(shared_storage.after_mm0.mm1.mm,
                                 shared_storage.after_mm0.si,
                                 (int)thread_id(),
                                 (int)warp_id(),
                                 (int)lane_id(),
                                 (int)problem_size_1_k);
        mma_pv.set_prologue_done(kPreloadV);
        if (!kKeepOutputInRF) {
          accum_o.clear();
        }
        mma_pv(gemm_k_iterations, accum_o, iterator_V, accum_o);
        __syncthreads();

        if (kPreloadV && !kSingleValueIteration && blockN + 1 < nBlockN) {
          prologueV(blockN + 1);
        }

        if (!kKeepOutputInRF) {
          DISPATCH_BOOL(
              iter_key_start == 0, kIsFirst, ([&] {
                DISPATCH_BOOL(
                    (iter_key_start + kKeysPerBlock) >= p.num_keys,
                    kIsLast,
                    ([&] {
                      using DefaultEpilogue = typename MM1::DefaultEpilogue;
                      using DefaultOp =
                          typename MM1::DefaultConfig::EpilogueOutputOp;
                      using ElementCompute = typename DefaultOp::ElementCompute;
                      using EpilogueOutputOp = typename cutlass::epilogue::
                          thread::MemoryEfficientAttentionNormalize<
                              typename cutlass::platform::conditional<
                                  kIsLast,
                                  output_t,
                                  output_accum_t>::type,
                              output_accum_t,
                              DefaultOp::kCount,
                              typename DefaultOp::ElementAccumulator,
                              ElementCompute,
                              kIsFirst,
                              kIsLast,
                              cutlass::Array<ElementCompute, kQueriesPerBlock>>;
                      using Epilogue = typename cutlass::epilogue::threadblock::
                          EpiloguePipelined<
                              typename DefaultEpilogue::Shape,
                              typename MM1::Mma::Operator,
                              DefaultEpilogue::kPartitionsK,
                              typename cutlass::platform::conditional<
                                  kIsLast,
                                  typename MM1::OutputTileIterator,
                                  typename MM1::OutputTileIteratorAccum>::type,
                              typename DefaultEpilogue::
                                  AccumulatorFragmentIterator,
                              typename DefaultEpilogue::WarpTileIterator,
                              typename DefaultEpilogue::SharedLoadIterator,
                              EpilogueOutputOp,
                              typename DefaultEpilogue::Padding,
                              DefaultEpilogue::kFragmentsPerIteration,
                              true,  // IterationsUnroll
                              typename MM1::OutputTileIteratorAccum  // Read
                                                                     // iterator
                              >;

                      int col = blockN * MM1::Mma::Shape::kN;
                      auto source_iter = createOutputAccumIter(col);
                      auto dest_iter =
                          call_conditional<kIsLast,
                                           decltype(createOutputIter),
                                           decltype(createOutputAccumIter)>::
                              apply(
                                  createOutputIter, createOutputAccumIter, col);
                      EpilogueOutputOp rescale(s_prime, m_prime);
                      Epilogue epilogue(
                          shared_storage.epilogue_shared_storage(),
                          thread_id(),
                          warp_id(),
                          lane_id());
                      epilogue(rescale, dest_iter, accum_o, source_iter);
                    }));
              }));
          if (!kSingleValueIteration) {
            __syncthreads();
          }
        }
      }
      __syncthreads();  // we modify `m_prime` after
    }

    if (kKeepOutputInRF) {
      constexpr bool kIsFirst = true;
      constexpr bool kIsLast = true;
      using DefaultEpilogue = typename MM1::DefaultEpilogue;
      using DefaultOp = typename MM1::DefaultConfig::EpilogueOutputOp;
      using ElementCompute = typename DefaultOp::ElementCompute;
      using EpilogueOutputOp =
          typename cutlass::epilogue::thread::MemoryEfficientAttentionNormalize<
              output_t,        // output
              output_accum_t,  // source
              DefaultOp::kCount,
              typename DefaultOp::ElementAccumulator,  // accum
              output_accum_t,                          // compute
              kIsFirst,
              kIsLast,
              cutlass::Array<ElementCompute, kQueriesPerBlock>>;
      using Epilogue =
          typename cutlass::epilogue::threadblock::EpiloguePipelined<
              typename DefaultEpilogue::Shape,
              typename MM1::Mma::Operator,
              DefaultEpilogue::kPartitionsK,
              typename MM1::OutputTileIterator,  // destination
              typename DefaultEpilogue::AccumulatorFragmentIterator,
              typename DefaultEpilogue::WarpTileIterator,
              typename DefaultEpilogue::SharedLoadIterator,
              EpilogueOutputOp,
              typename DefaultEpilogue::Padding,
              DefaultEpilogue::kFragmentsPerIteration,
              true,                                  // IterationsUnroll
              typename MM1::OutputTileIteratorAccum  // source tile
              >;
      auto dest_iter = createOutputIter(0);
      EpilogueOutputOp rescale(s_prime, m_prime);
      Epilogue epilogue(shared_storage.epilogue_shared_storage(),
                        thread_id(),
                        warp_id(),
                        lane_id());
      epilogue(rescale, dest_iter, accum_o);
    }

    // 7. Calculate logsumexp
    // To make the backward easier, we pad logsumexp with `inf`
    // this avoids a few bound checks, and is not more expensive during fwd
    static_assert(kQueriesPerBlock < kNumWarpsPerBlock * kWarpSize, "");
    if (p.logsumexp_ptr && thread_id() < kQueriesPerBlock) {
      auto lse_dim = ceil_div((int32_t)p.num_queries, kAlignLSE) * kAlignLSE;
      if (thread_id() < p.num_queries) {
        p.logsumexp_ptr[thread_id()] =
            accum_t(mi[thread_id()]) +
            cutlass::fast_log(accum_t(s_prime[thread_id()]));
      } else if (thread_id() < lse_dim) {
        p.logsumexp_ptr[thread_id()] =
            cutlass::platform::numeric_limits<accum_t>::infinity();
      }
    }
  }

  template <typename WarpIteratorC,
            bool kFullColumns,
            bool kIsFirst,
            bool kKeepOutputInRF>
  CUTLASS_DEVICE static void iterative_softmax(
      typename WarpIteratorC::Fragment& frag_o,  // output so far
      typename WarpIteratorC::Fragment& frag,
      cutlass::Array<accum_t, kQueriesPerBlock>& mi,
      cutlass::Array<accum_t, kQueriesPerBlock>& m_prime,
      cutlass::Array<accum_t, kQueriesPerBlock>& s_prime,
      cutlass::Array<accum_t, kQueriesPerBlock * MM0::MmaCore::WarpCount::kN>&
          addition_storage,
      int8_t lane_id,
      int8_t thread_id,
      int8_t warp_id,
      int16_t max_col,
      typename WarpIteratorC::TensorCoord const& tile_offset,
      float scaling) {
    /* Iterates on the accumulator and corresponding position on result matrix
    (1) Update `mi[r]` to the max value of the row `r`
    (2) In a second iteration do the following:
        (a) accum   <- exp(accum - mi)
        (b) m_prime <- exp(m_prime - mi)
        (c) s_prime <- s_prime * m_prime + sum(accum)
    All of this is done on registers, before we store all of this
    on shared memory for the next matmul with Value.
    */
    using Fragment = typename WarpIteratorC::Fragment;
    using LambdaIterator =
        typename DefaultMmaAccumLambdaIterator<WarpIteratorC,
                                               accum_t,
                                               kWarpSize>::Iterator;
    // Convert to `accum_t` (rather than double)
    constexpr float kLog2e = 1.4426950408889634074;  // log_2(e) = M_LOG2E
    static constexpr int kLinesPerWarp = kQueriesPerBlock / kNumWarpsPerBlock;
    if (!kIsFirst) {
      if (thread_id < kQueriesPerBlock) {
        m_prime[thread_id] = mi[thread_id];
      }
      __syncthreads();
    }

    auto lane_offset =
        LambdaIterator::get_lane_offset(lane_id, warp_id, tile_offset);

    // First update `mi` to the max per-row
    {
      accum_t max;
      LambdaIterator::iterateRows(
          lane_offset,
          [&](int accum_m) {
            max = -cutlass::platform::numeric_limits<accum_t>::infinity();
          },
          [&](int accum_m, int accum_n, int idx) {
            if (kFullColumns || accum_n < max_col) {
              max = cutlass::fast_max(max, frag[idx]);
            }
          },
          [&](int accum_m) {
            // Having 4x atomicMax seems faster than reduce within warp
            // first...
            atomicMaxFloat(&mi[accum_m], max * scaling);
          });
    }
    frag = cutlass::multiplies<Fragment>()(scaling * kLog2e, frag);

    // Make sure we all share the update values for `mi`
    __syncthreads();

    if (thread_id < kQueriesPerBlock) {
      auto m_prime_exp = exp2f(kLog2e * (m_prime[thread_id] - mi[thread_id]));
      m_prime[thread_id] = m_prime_exp;
      s_prime[thread_id] *= m_prime_exp;
    }
    __syncthreads();  // Update output fragments
    if (kKeepOutputInRF && !kIsFirst) {
      accum_t mp;
      LambdaIterator::iterateRows(
          lane_offset,
          [&](int accum_m) { mp = m_prime[accum_m]; },
          [&](int accum_m, int accum_n, int idx) { frag_o[idx] *= mp; },
          [&](int accum_m) {});
      __syncthreads();
    }
    // Update accum_m, accum_n, ...
    {
      accum_t mi_row, total_row;
      LambdaIterator::iterateRows(
          lane_offset,
          [&](int accum_m) { mi_row = kLog2e * mi[accum_m]; },
          [&](int accum_m, int accum_n, int idx) {
            frag[idx] = (kFullColumns || accum_n < max_col)
                            ? exp2f(frag[idx] - mi_row)
                            : accum_t(0.0);
          },
          [&](int accum_m) {});
      LambdaIterator::iterateRows(
          lane_offset,
          [&](int accum_m) { total_row = 0.0; },
          [&](int accum_m, int accum_n, int idx) { total_row += frag[idx]; },
          [&](int accum_m) {
            if (LambdaIterator::reduceSameRow(
                    lane_id, total_row, [](accum_t a, accum_t b) {
                      return a + b;
                    })) {
              addition_storage[accum_m + kQueriesPerBlock *
                                             tile_offset.column()] = total_row;
            }
          });
      __syncthreads();
      if (lane_id < kLinesPerWarp) {
        int id = warp_id * kLinesPerWarp + lane_id;
        total_row = s_prime[id];
        CUTLASS_PRAGMA_UNROLL
        for (int i = 0; i < MM0::MmaCore::WarpCount::kN; ++i) {
          total_row += addition_storage[id + kQueriesPerBlock * i];
        }
        s_prime[id] = total_row;
      }
    }
  }

  static CUTLASS_DEVICE int8_t lane_id() { return threadIdx.x; }
  static CUTLASS_DEVICE int8_t warp_id() { return threadIdx.y; }
  static CUTLASS_DEVICE int16_t thread_id() {
    return threadIdx.x + threadIdx.y * blockDim.x;
  }
};

template <typename AK>
__global__ void __launch_bounds__(AK::kNumThreads, AK::kMinBlocksPerSm)
    attention_kernel_batched_impl(typename AK::Params p) {
  if (!p.advance_to_block()) {
    return;
  }
  AK::attention_kernel(p);
}

template <typename AK>
__global__ void __launch_bounds__(AK::kNumThreads, AK::kMinBlocksPerSm)
    attention_kernel_batched(typename AK::Params params);
