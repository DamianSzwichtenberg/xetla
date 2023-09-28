﻿/*******************************************************************************
* Copyright (c) 2022-2023 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/
#include "tests/utils/utils.hpp"
#include "xetla.hpp"

enum class kslicing_impl_t { none = 0, global = 1, local = 2 };

template <kslicing_impl_t kslicing_type = kslicing_impl_t::none>
void gemm_universal_run(uint32_t iter) {
    // Tips, the example demonstrates programming kernel with XeTLA, it works as expected with current configurations.
    // Please make sure you fully understand these configurations before you do any modifications, incomplete changes may lead to unexpected behaviors.
    // Please contact us for support.

    //GEMM_UNIVERSAL input size
    size_t matrix_m = 4096;
    size_t matrix_n = 4096;
    size_t matrix_k = 4096;

    size_t size_a = matrix_m * matrix_k;
    size_t size_b = matrix_k * matrix_n;
    size_t size_c = matrix_m * matrix_n;

    using data_type_a = bf16;
    using data_type_b = bf16;
    using data_type_c = bf16;
    using data_type_acc = float;

    //Turn on the profiling property to facilitate subsequent profiling
    sycl::property_list properties {sycl::property::queue::enable_profiling()};

    //Define SYCL queue, context and device
    auto queue = sycl::queue(properties);
    auto context = queue.get_info<info::queue::context>();
    auto device = queue.get_info<info::queue::device>();

    std::cout << "Running on " << device.get_info<info::device::name>() << "\n";

    auto A = alloc_device_and_init<data_type_a>(
            size_a,
            [](data_type_a *data, size_t idx) {
                data[idx] = static_cast<data_type_a>(random_float());
            },
            queue, device, context);
    auto B = alloc_device_and_init<data_type_b>(
            size_b,
            [](data_type_b *data, size_t idx) {
                data[idx] = static_cast<data_type_b>(random_float());
            },
            queue, device, context);
    auto C = alloc_device_and_init<data_type_c>(
            size_c,
            [](data_type_c *data, size_t idx) {
                data[idx] = static_cast<data_type_c>(0.0f);
            },
            queue, device, context);

    //Define the shape of workgroup and subgroup
    //It's tunable parameters based on different input shape and hardware for better performance
    constexpr uint32_t wg_tile_m
            = (kslicing_type != kslicing_impl_t::local) ? 256 : 64;
    constexpr uint32_t wg_tile_n
            = (kslicing_type != kslicing_impl_t::local) ? 256 : 128;
    constexpr uint32_t sg_tile_m
            = (kslicing_type != kslicing_impl_t::local) ? 32 : 16;
    constexpr uint32_t sg_tile_n
            = (kslicing_type != kslicing_impl_t::local) ? 64 : 32;

    //There are implicit requirement for sg_tile_k range
    constexpr uint32_t sg_tile_k = 32;
    static constexpr uint32_t sync_freq = 8;
    static constexpr uint32_t stages = 3;

    // Org the compute shape for sub-matrix
    using tile_shape
            = xetla::group::tile_shape_t<wg_tile_n, // workgroup size in dim0
                    wg_tile_m, //	workgroup size in dim1
                    sg_tile_n, //	subgroup size in dim0
                    sg_tile_m>; //	subgroup size in dim1

    // Mirco-kernel configuration
    using gemm_t = typename xetla::group::gemm_selector_t<
            data_type_a, // input datatype for A
            data_type_b, // input datatype for B
            mem_layout::row_major, // memory layout for A
            mem_layout::row_major, // memory layout for B
            mem_space::global, // memory reading from global mem for A
            mem_space::global, // memory reading from global mem for B
            8, // buffer alignment for A, in unit of element
            8, // buffer alignment for B, in unit of element
            data_type_acc, // accumulator data type for intermediate resutls
            tile_shape, // computation tile shape
            sg_tile_k, // elements in each iteration
            mma_engine::xmx, // compute engine
            gpu_arch::Xe, // GPU arch
            stages, // number of prefetch pipe stage
            sync_freq> // frequency of periodic sync, in unit of inner loop
            ::gemm;

    using epilogue_t = xetla::group::epilogue_t<
            xetla::group::epilogue_policy_default<gpu_arch::Xe>, tile_shape,
            mem_desc_t<data_type_c, mem_layout::row_major, mem_space::global>>;

    // specify the range k_w/k_s by setting the corresponding ratio
    // splitk using global memory
    constexpr int num_global_splitk
            = (kslicing_type == kslicing_impl_t::global) ? 2 : 1;
    // splitk using local memory
    constexpr int num_local_splitk
            = (kslicing_type == kslicing_impl_t::local) ? 2 : 1;

    using dispatch_policy
            = gpu::xetla::kernel::dispatch_policy_kslicing<num_global_splitk,
                    num_local_splitk, gpu_arch::Xe>;

    using gemm_op_t = xetla::kernel::gemm_universal_t<dispatch_policy, gemm_t,
            epilogue_t>;

    // allocate temp buffers for global split
    size_t size_acc = gemm_op_t::get_acc_buf_size(matrix_m, matrix_n);
    size_t size_cnt = gemm_op_t::get_cnt_buf_size(matrix_m, matrix_n);
    auto Acc = alloc_device_and_init<data_type_acc>(
            size_acc,
            [](data_type_acc *data, size_t idx) {
                data[idx] = static_cast<data_type_acc>(0.0f);
            },
            queue, device, context);
    auto Cnt = alloc_device_and_init<uint32_t>(
            size_cnt,
            [](uint32_t *data, size_t idx) {
                data[idx] = static_cast<uint32_t>(0);
            },
            queue, device, context);

    if constexpr (kslicing_type != kslicing_impl_t::none) {
        std::cout << "gemm_universal with "
                  << (kslicing_type == kslicing_impl_t::global ? "global"
                                                               : "local")
                  << " cooperation" << std::endl;
    }

    // set up gemm_universal arguments
    typename gemm_op_t::arguments_t gemm_arg(matrix_m, matrix_k, matrix_n, A,
            matrix_k, B, matrix_n, C, matrix_n, Acc, Cnt);

    cl::sycl::nd_range<3> nd_range = gemm_op_t::get_nd_range(gemm_arg);
    if (!gemm_op_t::can_implement(gemm_arg)) {
        std::cout << "The arguments cannot be supported, aborting ... "
                  << std::endl;
        free(A, context);
        free(B, context);
        free(C, context);
        free(Acc, context);
        free(Cnt, context);
        FAIL();
    }

    uint32_t warmup = 10;
    long ops = 2 * static_cast<long>(matrix_m) * matrix_n * matrix_k;
    profiling_helper prof("gemm_universal", ops, "gflops");
    for (uint32_t i = 0; i < iter + warmup; i++) {
        if (i >= warmup) { prof.cpu_start(); }
        if constexpr (kslicing_type == kslicing_impl_t::global) {
            queue.memset(C, 0, size_c * sizeof(data_type_c));
        }
        auto gpu_event = queue.submit([&](handler &cgh) {
            // GPU kernel
            cgh.parallel_for(nd_range, [=](nd_item<3> item) SYCL_ESIMD_KERNEL {
                xetla_exec_item<3> ei(item);
                // allocate slm and nbarrier resource
                slm_barrier_init<gemm_op_t>();
                gemm_op_t gemm_op;
                gemm_op(ei, gemm_arg);
            });
        });
        gpu_event.wait();

        if (i >= warmup) {
            prof.cpu_end();
            prof.add_gpu_event(gpu_event);
        }
    }

    ASSERT_EQ(0,
            gemm_result_validate(A, B, C, 1, matrix_m, matrix_k, matrix_n,
                    queue, mem_layout::row_major, mem_layout::row_major));

    //performance
    prof.print_profiling_result(profiling_selector::GPU);

    free(A, context);
    free(B, context);
    free(C, context);
    free(Acc, context);
    free(Cnt, context);
}

int main() {
    // An example code for calculating matrix multiplication using
    // GEMM_UNIVERSAL API:
    //   C = A x B
    // The resulted matrix C is partitioned by the group range
    // in to multiple blocks. The block matrix
    //  C<i_w, j_w>
    // is computed by the workgroup with id: (0, i_w, j_w).
    // (i_w, j_w) is an element in range specified by group range.
    // Each thread with index (0, i_s, j_s) inside the same workgroup
    // is responsible for a sub block of matrix multiplication, which is
    //   C<i_w, j_w>[i_s*sg_m:(i_s+1):sg_m,j_s*sg_n:(j_s+1)*sg_n]

    // Alternatively, some threads can cooperate on the same sub block
    // matrix given the same (i_s, j_s), i.e. the index space is extended
    // from (0, i_s, j_s) to (k_s, i_s, j_s).

    // Another method to achieve the same effect is to extend the index space
    // in group range, i.e. from (0, i_w, j_w) to (k_w, i_w, j_w)

    // More detailed description referring to the cooperation (kslicing) could
    // be found in the example 01_gemm_universal with custom implementation

    // basic gemm_universal
    gemm_universal_run<kslicing_impl_t::none>(10);

    // basic gemm_universal with workgroup cooperation
    // gemm_universal_run<kslicing_impl_t::global>(10);

    // basic gemm_universal with thread cooperation
    // gemm_universal_run<kslicing_impl_t::local>(10);
    return (0);
}