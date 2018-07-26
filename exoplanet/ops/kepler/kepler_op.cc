#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/framework/tensor_shape.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/util/work_sharder.h"

#include <limits>

#include "kepler_op.h"

using namespace tensorflow;
using namespace exoplanet;

using CPUDevice = Eigen::ThreadPoolDevice;
using GPUDevice = Eigen::GpuDevice;

REGISTER_OP("Kepler")
  .Attr("T: {float, double}")
  .Attr("maxiter: int = 2000")
  .Attr("tol: float = -1")
  .Input("manom: T")
  .Input("eccen: T")
  .Output("eanom: T")
  .SetShapeFn([](shape_inference::InferenceContext* c) {
    shape_inference::ShapeHandle e;
    TF_RETURN_IF_ERROR(c->Merge(c->input(0), c->input(1), &e));
    c->set_output(0, c->input(0));
    return Status::OK();
  });

template <typename T>
class KeplerOpBase : public OpKernel {
 public:
  explicit KeplerOpBase (OpKernelConstruction* context) : OpKernel(context) {
    OP_REQUIRES_OK(context, context->GetAttr("maxiter", &maxiter_));
    OP_REQUIRES(context, maxiter_ >= 0,
                errors::InvalidArgument("Need maxiter >= 0, got ", maxiter_));
    OP_REQUIRES_OK(context, context->GetAttr("tol", &tol_));

    // Make sure that the tolerance isn't smaller than machine precision.
    auto eps = std::numeric_limits<T>::epsilon();
    if (tol_ < eps) tol_ = 2 * eps;
  }

  virtual void DoCompute (OpKernelContext* context,
      int maxiter, float tol, int N, const T* const M, const T* const e, T* E) = 0;


  void Compute(OpKernelContext* context) override {
    // Inputs
    const Tensor& M_tensor = context->input(0);
    const Tensor& e_tensor = context->input(1);

    // Dimensions
    const int64 N = M_tensor.NumElements();
    OP_REQUIRES(context, N <= tensorflow::kint32max,
        errors::InvalidArgument("too many elements in tensor"));
    OP_REQUIRES(context, e_tensor.NumElements() == N,
        errors::InvalidArgument("e and M must have the same number of elements"));

    // Output
    Tensor* E_tensor = NULL;
    OP_REQUIRES_OK(context, context->allocate_output(0, M_tensor.shape(), &E_tensor));

    // Access the data
    const auto M = M_tensor.template flat<T>();
    const auto e = e_tensor.template flat<T>();
    auto E = E_tensor->template flat<T>();

    DoCompute(context, maxiter_, tol_, static_cast<int>(N), M.data(), e.data(), E.data());
  }

 private:
  int maxiter_;
  float tol_;
};

template <class Device, typename T>
class KeplerOp;

template <typename T>
class KeplerOp<CPUDevice, T> : public KeplerOpBase<T> {

  public:
    explicit KeplerOp (OpKernelConstruction* context) : KeplerOpBase<T>(context) {}

    void DoCompute (OpKernelContext* ctx,
      int maxiter, float tol, int N, const T* const M, const T* const e, T* E
    ) override {
      auto work = [&](int64 begin, int64 end) {
        for (int i = begin; i < end; ++i) {
          E[i] = kepler::solve_kepler<T>(M[i], e[i], maxiter, tol);
        }
      };
      auto worker_threads = *ctx->device()->tensorflow_cpu_worker_threads();
      int64 cost = 5;
      Shard(worker_threads.num_threads, worker_threads.workers, N, cost, work);
    }

};

#define REGISTER_CPU(type)                                                 \
  REGISTER_KERNEL_BUILDER(                                                 \
      Name("Kepler").Device(DEVICE_CPU).TypeConstraint<type>("T"),         \
      KeplerOp<CPUDevice, type>)

REGISTER_CPU(float);
REGISTER_CPU(double);

#undef REGISTER_CPU

#ifdef GOOGLE_CUDA

template <typename T>
class KeplerOp<GPUDevice, T> : public KeplerOpBase<T> {

  public:
    explicit KeplerOp (OpKernelConstruction* context) : KeplerOpBase<T>(context) {}

    void DoCompute (OpKernelContext* ctx,
      int maxiter, float tol, int N, const T* const M, const T* const e, T* E
    ) override {
      KeplerCUDAFunctor<T>()(ctx->eigen_device<GPUDevice>(), maxiter, tol, N, M, e, E);
    }

};

#define REGISTER_GPU(type)                                                 \
  REGISTER_KERNEL_BUILDER(                                                 \
      Name("Kepler").Device(DEVICE_GPU).TypeConstraint<type>("T"),         \
      KeplerOp<GPUDevice, type>)

REGISTER_GPU(float);
REGISTER_GPU(double);

#undef REGISTER_GPU

#endif
