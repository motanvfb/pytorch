#include <ATen/ATen.h>
#include <c10/util/irange.h>
#include <cmath>
#include <tuple>
#include <vector>

#ifdef USE_FBGEMM
#include <fbgemm/QuantUtils.h>
#endif
#include <ATen/native/quantized/cpu/quant_utils.h>

namespace {
void calculate_moving_average(
    const at::Tensor& x,
    const at::Tensor& averaging_const,
    at::Tensor& running_min,
    at::Tensor& running_max) {
  at::Tensor x_min, x_max;
  std::tie(x_min, x_max) = at::_aminmax(x);
  const float min_curr_val = x_min.item().toFloat();
  const float max_curr_val = x_max.item().toFloat();
  // Moving Average Min/Max observer for input tensor
  float* running_min_val = running_min.data_ptr<float>();
  float* running_max_val = running_max.data_ptr<float>();
  float* averaging_const_val = averaging_const.data_ptr<float>();
  *running_min_val = std::isinf(*running_min_val) ? min_curr_val
                                                  : *running_min_val +
          *averaging_const_val * (min_curr_val - *running_min_val);
  *running_max_val = std::isinf(*running_max_val) ? max_curr_val
                                                  : *running_max_val +
          *averaging_const_val * (max_curr_val - *running_max_val);
  return;
}

std::tuple<at::Tensor, at::Tensor> choose_qparams_fake_quant(
    const at::Tensor& x,
    const at::Tensor& inp_running_min,
    const at::Tensor& inp_running_max,
    at::Tensor& scale,
    at::Tensor& zero_point,
    bool per_row_fake_quant,
    bool symmetric_quant,
    int qmin,
    int qmax,
    int ch_axis) {
  std::tuple<at::Tensor, at::Tensor> fake_quant_out;
  at::Tensor x_min, x_max;

  if (per_row_fake_quant) {
    float* x_min_data = inp_running_min.data_ptr<float>();
    float* x_max_data = inp_running_max.data_ptr<float>();

    for (const auto i : c10::irange(x_min.numel())) {
#ifdef USE_FBGEMM
      fbgemm::TensorQuantizationParams x_qparams{};
      x_qparams = fbgemm::ChooseQuantizationParams(
          x_min_data[i],
          x_max_data[i],
          qmin,
          qmax,
          symmetric_quant,
          /* preserve sparsity */ // Set preserve_sparsity to true for
                                  // symmetric quantization.
          false /* force power of two*/);
      scale[i] = x_qparams.scale;
      zero_point[i] = x_qparams.zero_point;
#else
      quant_utils::TensorQuantizationParams x_qparams{};
      x_qparams = quant_utils::ChooseQuantizationParams(
          x_min_data[i],
          x_max_data[i],
          qmin,
          qmax,
          symmetric_quant, /* preserve sparsity */
          false /* force power of two*/);
      scale[i] = x_qparams.scale;
      zero_point[i] = x_qparams.zero_point;
#endif
    }
    fake_quant_out = at::fake_quantize_per_channel_affine_cachemask(
        x, scale, zero_point, ch_axis, qmin, qmax);
  } else {
#ifdef USE_FBGEMM
    fbgemm::TensorQuantizationParams x_qparams{};
    // compute quantization parameters using min-max values
    x_qparams = fbgemm::ChooseQuantizationParams(
        inp_running_min.item().toFloat(),
        inp_running_max.item().toFloat(),
        qmin,
        qmax,
        symmetric_quant, /* bool preserve_sparsity */
        false /* force power of two*/);

    scale[0] = x_qparams.scale;
    zero_point[0] = x_qparams.zero_point;
#else
    quant_utils::TensorQuantizationParams x_qparams{};
    // compute quantization parameters using min-max values
    x_qparams = quant_utils::ChooseQuantizationParams(
        inp_running_min.item().toFloat(),
        inp_running_max.item().toFloat(),
        qmin,
        qmax,
        symmetric_quant, /* bool preserve_sparsity */
        false /* force power of two*/);
    scale[0] = x_qparams.scale;
    zero_point[0] = x_qparams.zero_point;
#endif
    fake_quant_out =
        at::_fake_quantize_per_tensor_affine_cachemask_tensor_qparams(
            x, scale, zero_point, qmin, qmax);
  }
  return fake_quant_out;
}
} // namespace

namespace at {
namespace native {

std::tuple<at::Tensor, at::Tensor> fused_moving_avg_obs_fake_quant_cpu(
    const at::Tensor& self,
    const at::Tensor& observer_on,
    const at::Tensor& fake_quant_on,
    const at::Tensor& averaging_const,
    at::Tensor& running_min,
    at::Tensor& running_max,
    at::Tensor& scale,
    at::Tensor& zero_point,
    const int64_t quant_min,
    const int64_t quant_max,
    const int64_t ch_axis,
    bool per_row_fake_quant,
    bool symmetric_quant) {
  auto mask = at::ones_like(self, at::kBool, MemoryFormat::Preserve);
  // Calculate min/max
  auto observe = observer_on.item().toInt();
  if (observe) {
    calculate_moving_average(self, averaging_const, running_min, running_max);
  }
  // Calculate qparams and fake_quantize
  auto fake_quant = fake_quant_on.item().toInt();
  if (fake_quant) {
    return choose_qparams_fake_quant(
        self,
        running_min,
        running_max,
        scale,
        zero_point,
        per_row_fake_quant,
        symmetric_quant,
        quant_min,
        quant_max,
        ch_axis);
  }
  return std::make_tuple(self, mask);
}
} // namespace native
} // namespace at