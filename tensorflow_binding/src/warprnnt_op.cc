#ifdef WARPRNNT_ENABLE_GPU
#define EIGEN_USE_GPU
#include <cuda.h>
#endif

#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/kernels/bounds_check.h"
#include "tensorflow/core/framework/allocator.h"
#include "rnnt.h"


REGISTER_OP("WarpRNNT")
    .Input("trans_acts: float32")
    .Input("pred_acts: float32")
    .Input("labels: int32")
    .Input("input_lengths: int32")
    .Input("label_lengths: int32")
    .Attr("blank_label: int = 0")
    .Output("costs: float32")
    .Output("trans_grad: float32")
    .Output("pred_grad: float32");

namespace tf = tensorflow;

namespace warp_rnnt {

class WarpRNNTOpBase : public tf::OpKernel {
  public:
    explicit WarpRNNTOpBase(tf::OpKernelConstruction* ctx) : tf::OpKernel(ctx) {
        OP_REQUIRES_OK(ctx, ctx->GetAttr("blank_label", &blank_label_));
    }

    void Compute(tf::OpKernelContext* ctx) override {
        // Grab the input tensors
        const tf::Tensor* trans_acts;
        const tf::Tensor* pred_acts;
        const tf::Tensor* labels;
        const tf::Tensor* label_lengths;
        const tf::Tensor* input_lengths;
        OP_REQUIRES_OK(ctx, ctx->input("trans_acts", &trans_acts));
        OP_REQUIRES_OK(ctx, ctx->input("pred_acts", &pred_acts));
        OP_REQUIRES_OK(ctx, ctx->input("labels", &labels));
        OP_REQUIRES_OK(ctx, ctx->input("label_lengths", &label_lengths));
        OP_REQUIRES_OK(ctx, ctx->input("input_lengths", &input_lengths));

        OP_REQUIRES(ctx, trans_acts->shape().dims() == 3,
                    tf::errors::InvalidArgument("trans_acts is not a 3-Tensor"));
        OP_REQUIRES(ctx, pred_acts->shape().dims() == 3,
                    tf::errors::InvalidArgument("pred_acts is not a 3-Tensor"));
        OP_REQUIRES(ctx, labels->shape().dims() == 2,
                    tf::errors::InvalidArgument("labels is not a 2-Tensor"));
        OP_REQUIRES(ctx, tf::TensorShapeUtils::IsVector(label_lengths->shape()),
                     tf::errors::InvalidArgument("label_lengths is not a vector"));
        OP_REQUIRES(ctx, tf::TensorShapeUtils::IsVector(input_lengths->shape()),
                     tf::errors::InvalidArgument("input_lengths is not a vector"));

        const auto& acts_shape = trans_acts->shape();
        const auto batch_size = acts_shape.dim_size(0);
        const auto max_time = acts_shape.dim_size(1);
        const auto num_classes_raw = acts_shape.dim_size(2);
        const auto max_u = pred_acts->shape().dim_size(1);

        auto trans_acts_t = trans_acts->tensor<float, 3>();
        auto pred_acts_t = pred_acts->tensor<float, 3>();
        auto labels_t = labels->tensor<int32_t, 2>();

        OP_REQUIRES(
                ctx, tf::FastBoundsCheck(num_classes_raw, std::numeric_limits<int>::max()),
                tf::errors::InvalidArgument("num_classes cannot exceed max int"));
        const auto alphabet_size = static_cast<const int>(num_classes_raw);

        OP_REQUIRES(
                ctx, batch_size == input_lengths->dim_size(0),
                tf::errors::InvalidArgument("len(input_lengths) != batch_size.  ",
                                            "len(input_length):  ", input_lengths->dim_size(0),
                                            " batch_size: ", batch_size));
        auto input_lengths_t = input_lengths->vec<int32_t>();

        OP_REQUIRES(
                ctx, batch_size == label_lengths->dim_size(0),
                tf::errors::InvalidArgument("len(label_lengths) != batch_size.  ",
                                            "len(label_length):  ", label_lengths->dim_size(0),
                                            " batch_size: ", batch_size));
        auto label_lengths_t = label_lengths->vec<int32_t>();

        // check that labels are in the alphabet?

        for (int b = 0; b < batch_size; b++) {
            OP_REQUIRES(ctx, input_lengths_t(b) <= max_time,
                        tf::errors::InvalidArgument("input_lengths(", b, ") <= ", max_time));
        }

        tf::Tensor* costs = nullptr;
        OP_REQUIRES_OK(ctx, ctx->allocate_output("costs", input_lengths->shape(), &costs));
        auto costs_t = costs->vec<float>();

        tf::Tensor* trans_grads = nullptr;
        OP_REQUIRES_OK(ctx, ctx->allocate_output("trans_grads", trans_acts->shape(),
                                                 &trans_grads));
        set_zero(trans_grads);
        auto trans_grads_t = trans_grads->tensor<float, 3>();

        tf::Tensor* pred_grads = nullptr;
        OP_REQUIRES_OK(ctx, ctx->allocate_output("pred_grads", pred_grads->shape(),
                                                 &pred_grads));
        set_zero(pred_grads);
        auto pred_grads_t = pred_grads->tensor<float, 3>();

        auto options = create_options(ctx);
        options.blank_label = blank_label_;
        options.maxT = max_time;
        options.maxU = max_u;

        size_t workspace_size_bytes;
        bool use_gpu = false;
        if(options.loc == RNNT_GPU) {
            use_gpu = true;
        }
        auto warp_status = get_workspace_size(max_time,
                                              max_u,
                                              batch_size,
                                              use_gpu,
                                              &workspace_size_bytes);

        OP_REQUIRES(ctx, warp_status == RNNT_STATUS_SUCCESS,
                    tf::errors::Internal("warp_rnnt error in get_workspace_size: ",
                                         rnntGetStatusString(warp_status)));

        auto workspace_shape = tf::TensorShape{static_cast<int64_t>(workspace_size_bytes)};
        tf::Tensor workspace;
        OP_REQUIRES_OK(ctx, ctx->allocate_temp(tf::DT_UINT8, workspace_shape, &workspace));
        auto workspace_t = workspace.flat<uint8_t>();

        // compute RNNT
        warp_status = compute_rnnt_loss(trans_acts_t.data(),
                                        pred_acts_t.data(),
                                        trans_grads_t.data(),
                                        pred_grads_t.data(),
                                        labels_t.data(),
                                        label_lengths_t.data(),
                                        input_lengths_t.data(),
                                        alphabet_size, batch_size,
                                        costs_t.data(), workspace_t.data(), options);

        OP_REQUIRES(ctx, warp_status == RNNT_STATUS_SUCCESS,
                    tf::errors::Internal("warp_rnnt error in compute_rnnt_loss: ",
                                         rnntGetStatusString(warp_status)));

    }
  private:
    int blank_label_;
    virtual void set_zero(tf::Tensor* t) = 0;
    virtual rnntOptions create_options(tf::OpKernelContext* ctx) = 0;
};

class WarpRNNTOpCPU : public WarpRNNTOpBase {
  public:
    explicit WarpRNNTOpCPU(tf::OpKernelConstruction* ctx) : WarpRNNTOpBase(ctx) {
    }

  private:
    void set_zero(tf::Tensor* t) override {
        t->flat<float>().setZero();
    }

    rnntOptions create_options(tf::OpKernelContext* ctx) override {
        auto options = rnntOptions{};
        options.loc = RNNT_CPU;
        options.num_threads = ctx->device()->tensorflow_cpu_worker_threads()->num_threads;
        return options;
    }
};

REGISTER_KERNEL_BUILDER(Name("WarpRNNT").Device(::tensorflow::DEVICE_CPU), WarpRNNTOpCPU);

#ifdef WARPRNNT_ENABLE_GPU

class WarpRNNTOpGPU : public WarpRNNTOpBase {
  public:
    explicit WarpRNNTOpGPU(tf::OpKernelConstruction* ctx) : WarpRNNTOpBase(ctx) {
    }

  private:
    void set_zero(tf::Tensor* t) override {
        cudaMemset(t->flat<float>().data(), 0, t->NumElements()*sizeof(float));
    }

    rnntOptions create_options(tf::OpKernelContext* ctx) override {
        auto cuda_stream = ctx->eigen_device<Eigen::GpuDevice>().stream();
        auto options = rnntOptions{};
        options.loc = RNNT_GPU;
        options.stream = cuda_stream;
        return options;
    }
};

REGISTER_KERNEL_BUILDER(Name("WarpRNNT").Device(::tensorflow::DEVICE_GPU)
                        .HostMemory("costs"),
                        WarpRNNTOpGPU);
#undef EIGEN_USE_GPU
#endif

}
