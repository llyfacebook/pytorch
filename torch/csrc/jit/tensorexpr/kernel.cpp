#include <torch/csrc/jit/tensorexpr/kernel.h>
#include <torch/csrc/jit/tensorexpr/ir_printer.h>
#include <torch/csrc/jit/tensorexpr/schedule.h>

using namespace torch::jit;
using namespace torch::jit::tensorexpr;

namespace torch {
namespace jit {
namespace tensorexpr {

static int te_cuda_pointwise_loop_levels = -1;
static int te_cuda_pointwise_block_count = -1;
static int te_cuda_pointwise_block_size = -1;

int& GetTECudaPointwiseLoopLevels() {
  return te_cuda_pointwise_loop_levels;
}

int& GetTECudaPointwiseBlockCount() {
  return te_cuda_pointwise_block_count;
}

int& GetTECudaPointwiseBlockSize() {
  return te_cuda_pointwise_block_size;
}

} // namespace tensorexpr
} // namespace jit
} // namespace torch


static Dtype texprType(const c10::optional<at::ScalarType>& st) {
  switch (*st) {
    case at::ScalarType::Int:
      return kInt32;
    case at::ScalarType::Float:
      return kFloat32;
    default:
      LOG(FATAL) << "Unhandled datatype";
      return kUninitialized;
  }
}

static at::ScalarType tensorType(Tensor* t) {
  auto const& stype = t->function()->body().dtype().scalar_type();
  if (stype == kInt32) {
    return at::ScalarType::Int;
  } else if (stype == kFloat32) {
    return at::ScalarType::Float;
  }
  LOG(FATAL) << "Unhandled datatype";
  return at::ScalarType::Float;
}

static std::vector<ExprHandle> texprSizes(const c10::VaryingShape& shape) {
  std::vector<ExprHandle> dims;
  for (size_t i = 0; i < *shape.size(); i++) {
    dims.push_back(IntImm::make(*shape[i]));
  }
  return dims;
}

static std::vector<DimArg> texprDims(const torch::jit::Value* v) {
  CHECK(v->type()->kind() == TypeKind::TensorType);
  auto tt = v->type()->cast<TensorType>();
  std::vector<DimArg> dimArgs;
  int i = 0;
  for (auto const& s : texprSizes(tt->sizes())) {
    dimArgs.push_back({s, "i" + std::to_string(i++)});
  }
  return dimArgs;
}

template <typename T>
int64_t bufferSize(T t) {
  int64_t size = 1;
  for (int i = 0; i < t.ndim(); i++) {
    size *= t.dim(i).template AsNode<IntImm>()->value();
  }
  return size;
}

ExprHandle TensorExprKernel::constant(const torch::jit::Value* v) {
  if (v->node()->kind() == prim::Constant) {
    const auto val = toIValue(v).value();
    if (val.isDouble()) {
      return FloatImm::make(val.toDouble());
    } else if (val.isInt()) {
      return IntImm::make(val.toInt());
    } else if (val.isNone()) {
      // This is just a placeholder so we don't throw.  None-handling
      // is operator-specific and should be handled properly in
      // the operator-specific lowering code.
      return IntImm::make(0);
    } else {
      LOG(FATAL) << "Unhandled constant datatype";
    }
  }
  CHECK(scalars_.count(v->unique())) << "Couldn't find scalar value";
  return scalars_.at(v->unique());
}

void TensorExprKernel::promoteInputs(std::vector<ExprHandle>& inputs) {
  bool any_float = std::any_of(inputs.begin(), inputs.end(), [](const ExprHandle& e) {
    return e.dtype() == kFloat32;
  });

  if (!any_float)
    return;

  for (ExprHandle& e : inputs) {
    if (e.dtype() == kInt32) {
      e = cast<float>(e);
    }
  }
}

ExprHandle TensorExprKernel::demoteOutput(const ExprHandle& e, const torch::jit::Value* v) {
  CHECK(v->type()->kind() == TypeKind::TensorType);
  auto tt = v->type()->cast<TensorType>()->scalarType();
  if (e.dtype() == kFloat32 && tt == at::ScalarType::Int) {
    return cast<int>(e);
  }

  return e;
}

static bool isOne(ExprHandle e) {
  auto const& n = e.AsNode<IntImm>();
  if (!n) {
    return false;
  }
  return n->value() == 1;
}

static std::vector<ExprHandle> broadcastShapes(
    const std::vector<ExprHandle>& a,
    const std::vector<ExprHandle>& b) {
  auto at = a.rbegin();
  auto bt = b.rbegin();
  std::vector<ExprHandle> ret;
  while (at != a.rend() || bt != b.rend()) {
    if (at == a.rend()) {
      ret.push_back(*bt++);
      continue;
    }
    if (bt == b.rend()) {
      ret.push_back(*at++);
      continue;
    }
    // TODO: if neither *at nor *bt is 1, ensure they are identical
    // expressions.  Nb: `==` doesn't work since that simply produces a new
    // ExprHandle.
    ExprHandle dim = isOne(*at) ? *bt : *at;
    ret.push_back(dim);
    at++;
    bt++;
  }
  std::reverse(ret.begin(), ret.end());
  return ret;
}

template <typename... Args>
static std::vector<ExprHandle> broadcastShapes(
    const std::vector<ExprHandle>& a,
    const std::vector<ExprHandle>& b,
    Args... args) {
  return broadcastShapes(broadcastShapes(a, b), args...);
}

std::vector<ExprHandle> TensorExprKernel::valueShape(const torch::jit::Value* v) {
  auto it = tensors_.find(v->unique());
  if (it == tensors_.end()) {
    return {1};
  }
  return it->second->function()->dims();
}

Tensor* TensorExprKernel::ComputeOneOperand(
    const std::string& name,
    const torch::jit::Value* v,
    std::function<ExprHandle(const ExprHandle&)> inner_expr) {
  auto const& n = v->node();
  auto const& shape = valueShape(n->inputs()[0]);
  return Compute(
      name,
      c10::fmap<DimArg>(shape),
      [this, v, inner_expr](const std::vector<VarHandle>& axes) {
        auto const& n = v->node();
        std::vector<ExprHandle> inputs = {tensorOrConstant(n->inputs()[0], axes)};

        promoteInputs(inputs);
        ExprHandle compute = inner_expr(inputs[0]);
        return demoteOutput(compute, n->output());
      });
}

Tensor* TensorExprKernel::ComputeTwoOperand(
    const std::string& name,
    const torch::jit::Value* v,
    std::function<ExprHandle(const ExprHandle&, const ExprHandle&)> inner_expr) {
  auto const& n = v->node();
  auto const& shape =
      broadcastShapes(valueShape(n->inputs()[0]), valueShape(n->inputs()[1]));
  return Compute(
      name,
      c10::fmap<DimArg>(shape),
      [this, v, inner_expr](const std::vector<VarHandle>& axes) {
        auto const& n = v->node();
        std::vector<ExprHandle> inputs = {
            tensorOrConstant(n->inputs()[0], axes),
            tensorOrConstant(n->inputs()[1], axes),
        };

        promoteInputs(inputs);
        ExprHandle compute = inner_expr(inputs[0], inputs[1]);
        return demoteOutput(compute, n->output());
      });
}

Tensor* TensorExprKernel::ComputeTwoOperandWithAlpha(
    const std::string& name,
    const torch::jit::Value* v,
    std::function<ExprHandle(const ExprHandle&, const ExprHandle&)> inner_expr) {
  auto const& n = v->node();
  auto const& shape =
      broadcastShapes(valueShape(n->inputs()[0]), valueShape(n->inputs()[1]));
  return Compute(
      name,
      c10::fmap<DimArg>(shape),
      [this, v, inner_expr](const std::vector<VarHandle>& axes) {
        auto const& n = v->node();
        std::vector<ExprHandle> inputs = {
            tensorOrConstant(n->inputs()[0], axes),
            tensorOrConstant(n->inputs()[1], axes),
            tensorOrConstant(n->inputs()[2], axes),
        };

        promoteInputs(inputs);
        ExprHandle compute = inner_expr(inputs[0], inputs[2] * inputs[1]);
        return demoteOutput(compute, n->output());
      });
}

Tensor* TensorExprKernel::ComputeThreeOperand(
    const std::string& name,
    const torch::jit::Value* v,
    std::function<ExprHandle(const ExprHandle&, const ExprHandle&, const ExprHandle&)> inner_expr) {
  auto const& n = v->node();
  auto const& shape = broadcastShapes(
      valueShape(n->inputs()[0]),
      valueShape(n->inputs()[1]),
      valueShape(n->inputs()[2]));
  return Compute(
      name,
      c10::fmap<DimArg>(shape),
      [this, v, inner_expr](const std::vector<VarHandle>& axes) {
        auto const& n = v->node();
        std::vector<ExprHandle> inputs = {
            tensorOrConstant(n->inputs()[0], axes),
            tensorOrConstant(n->inputs()[1], axes),
            tensorOrConstant(n->inputs()[2], axes),
        };

        promoteInputs(inputs);
        ExprHandle compute = inner_expr(inputs[0], inputs[1], inputs[2]);
        return demoteOutput(compute, n->output());
      });
}

Tensor* TensorExprKernel::ComputeFourOperand(
    const std::string& name,
    const torch::jit::Value* v,
    std::function<ExprHandle(const ExprHandle&, const ExprHandle&, const ExprHandle&, const ExprHandle&)>
        inner_expr) {
  auto const& n = v->node();
  auto const& shape = broadcastShapes(
      valueShape(n->inputs()[0]),
      valueShape(n->inputs()[1]),
      valueShape(n->inputs()[2]),
      valueShape(n->inputs()[3]));
  return Compute(
      name,
      c10::fmap<DimArg>(shape),
      [this, v, inner_expr](const std::vector<VarHandle>& axes) {
        auto const& n = v->node();
        std::vector<ExprHandle> inputs = {
            tensorOrConstant(n->inputs()[0], axes),
            tensorOrConstant(n->inputs()[1], axes),
            tensorOrConstant(n->inputs()[2], axes),
            tensorOrConstant(n->inputs()[3], axes),
        };

        promoteInputs(inputs);
        ExprHandle compute = inner_expr(inputs[0], inputs[1], inputs[2], inputs[3]);
        return demoteOutput(compute, n->output());
      });
}

Tensor* TensorExprKernel::ComputeValue(const torch::jit::Value* v) {
  switch (v->node()->kind()) {
    case aten::add: {
      return ComputeTwoOperandWithAlpha(
          "aten_add", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            return lhs + rhs;
          });
    } break;

    case aten::_cast_Float: {
      return ComputeOneOperand("aten_cast_float", v, [](const ExprHandle& a) {
        return cast<float>(a);
      });
    } break;

    case aten::sub: {
      return ComputeTwoOperandWithAlpha(
          "aten_sub", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            return lhs - rhs;
          });
    } break;

    case aten::mul: {
      return ComputeTwoOperand(
          "aten_mul", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            return lhs * rhs;
          });
    } break;

    case aten::div: {
      return ComputeTwoOperand(
          "aten_div", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            return lhs / rhs;
          });
    } break;

    case aten::addcmul: {
      return ComputeFourOperand(
          "aten_addcmul",
          v,
          [](const ExprHandle& a0, const ExprHandle& a1, const ExprHandle& a2, const ExprHandle& a3) {
            return a0 + a3 * a1 * a2;
          });
    } break;

    case aten::eq: {
      return ComputeTwoOperand(
          "aten_eq", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            return lhs == rhs;
          });
    } break;

    case aten::ne: {
      return ComputeTwoOperand(
          "aten_ne", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            return lhs != rhs;
          });
    } break;
    case aten::ge: {
      return ComputeTwoOperand(
          "aten_ge", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            return lhs >= rhs;
          });
    } break;

    case aten::gt: {
      return ComputeTwoOperand(
          "aten_gt", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            return lhs > rhs;
          });
    } break;

    case aten::le: {
      return ComputeTwoOperand(
          "aten_le", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            return lhs <= rhs;
          });
    } break;

    case aten::lt: {
      return ComputeTwoOperand(
          "aten_lt", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            return lhs < rhs;
          });
    } break;

    case aten::min: {
      return ComputeTwoOperand(
          "aten_min", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            return Min::make(lhs, rhs, false);
          });
    } break;

    case aten::max: {
      return ComputeTwoOperand(
          "aten_max", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            return Max::make(lhs, rhs, false);
          });
    } break;

    case aten::clamp: {
      bool no_min = false;
      bool no_max = false;
      if (v->node()->input(1)->node()->kind() == prim::Constant) {
          const auto val = toIValue(v->node()->input(1)).value();
          if (val.isNone()) {
            no_min = true;
          }
      }

      if (v->node()->input(2)->node()->kind() == prim::Constant) {
          const auto val = toIValue(v->node()->input(2)).value();
          if (val.isNone()) {
            no_max = true;
          }
      }

      return ComputeThreeOperand(
          "aten_clamp", v, [no_min, no_max](const ExprHandle& in, const ExprHandle& min, const ExprHandle& max) {
            if (no_min && no_max) {
              return in;
            } else if (no_min) {
              return Min::make(in, max, false);
            } else if (no_max) {
              return Max::make(in, min, false);
            } else {
              return Max::make(Min::make(in, max, false), min, false);
            }
          });
    } break;

    case aten::sigmoid: {
      return ComputeOneOperand("aten_sigmoid", v, [](const ExprHandle& a) {
        return ExprHandle(1.0f) / (ExprHandle(1.0f) + exp(ExprHandle(-0.0f) - a));
      });
    } break;

    case aten::reciprocal: {
      return ComputeOneOperand(
          "aten_reciprocal", v, [](const ExprHandle& a) { return ExprHandle(1.0f) / a; });
    } break;

    case aten::neg: {
      return ComputeOneOperand(
          "aten_neg", v, [](const ExprHandle& a) { return ExprHandle(-0) - a; });
    } break;

    case aten::relu: {
      return ComputeOneOperand("aten_relu", v, [](const ExprHandle& a) {
        return Max::make(a, 0, false);
      });
    } break;

    case aten::log: {
      return ComputeOneOperand(
          "aten_log", v, [](const ExprHandle& a) { return log(a); });
    } break;

    case aten::log10: {
      return ComputeOneOperand(
          "aten_log10", v, [](const ExprHandle& a) { return log10(a); });
    } break;

    case aten::log2: {
      return ComputeOneOperand(
          "aten_log2", v, [](const ExprHandle& a) { return log2(a); });
    } break;

    case aten::exp: {
      return ComputeOneOperand(
          "aten_exp", v, [](const ExprHandle& a) { return exp(a); });
    } break;

    case aten::expm1: {
      return ComputeOneOperand(
          "aten_expm1", v, [](const ExprHandle& a) { return expm1(a); });
    } break;

    case aten::erf: {
      return ComputeOneOperand(
          "aten_erf", v, [](const ExprHandle& a) { return erf(a); });
    } break;

    case aten::erfc: {
      return ComputeOneOperand(
          "aten_erfc", v, [](const ExprHandle& a) { return erfc(a); });
    } break;

    case aten::cos: {
      return ComputeOneOperand(
          "aten_cos", v, [](const ExprHandle& a) { return cos(a); });
    } break;

    case aten::sin: {
      return ComputeOneOperand(
          "aten_sin", v, [](const ExprHandle& a) { return sin(a); });
    } break;

    case aten::tan: {
      return ComputeOneOperand(
          "aten_tan", v, [](const ExprHandle& a) { return tan(a); });
    } break;

    case aten::type_as: {
      return ComputeTwoOperand(
          "aten_type_as", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            return Cast::make(rhs.dtype(), lhs);
    });
    } break;

    case aten::rand_like: {
      return ComputeOneOperand(
          "aten_rand_like", v, [](const ExprHandle& a) {
	    return Intrinsics::make(IntrinsicsOp::kRand, a.dtype());
	  });
    } break;

    case aten::pow: {
      return ComputeTwoOperand(
          "aten_pow", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            const FloatImm* float_imm = rhs.AsNode<FloatImm>();
            if (float_imm) {
              float imm = float_imm->value();
              if (imm == 1.0f) {
                return lhs;
              } else if (imm == 2.0f) {
                return lhs * lhs;
              } else if (imm == 3.0f) {
                return (lhs * lhs) * lhs;
              } else if (imm == 4.0f) {
                ExprHandle tmp = lhs * lhs;
                return tmp * tmp;
              } else if (imm == 0.5f) {
                return sqrt(lhs);
              } else if (imm == 0.0f) {
                return ExprHandle(1.0f);
              } else if (imm == -0.5f) {
                return rsqrt(lhs);
              } else if (imm == -1.0f) {
                return ExprHandle(1.0f) / lhs;
              } else if (imm == -2.0f) {
                return ExprHandle(1.0f) / (lhs * lhs);
              }
            }

            const Cast* float_cast = rhs.AsNode<Cast>();
            if (float_cast) {
              const IntImm* int_imm = dynamic_cast<const IntImm*>(float_cast->src_value());
              if (int_imm) {
                float imm = int_imm->value();
                if (imm == 1) {
                  return lhs;
                } else if (imm == 2) {
                  return lhs * lhs;
                } else if (imm == 3) {
                  return (lhs * lhs) * lhs;
                } else if (imm == 4) {
                  ExprHandle tmp = lhs * lhs;
                  return tmp * tmp;
                } else if (imm == 0) {
                  return ExprHandle(1.0f);
                } else if (imm == -1) {
                  return ExprHandle(1.0f) / lhs;
                } else if (imm == -2) {
                  return ExprHandle(1.0f) / (lhs * lhs);
                }
              }
            }
            return pow(lhs, rhs);
          });
    } break;

    case aten::fmod: {
      return ComputeTwoOperand(
          "aten_fmod", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            return fmod(lhs, rhs);
          });
    } break;

    case aten::lerp: {
      return ComputeThreeOperand(
          "aten_lerp", v, [](const ExprHandle& a, const ExprHandle& end, const ExprHandle& weight) {
            return a + weight * (end - a);
          });
    } break;
    case aten::remainder: {
      return ComputeTwoOperand(
          "aten_remainder", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            return fmod((rhs + fmod(lhs, rhs)), rhs);
          });

    } break;

    case aten::acos: {
      return ComputeOneOperand(
          "aten_acos", v, [](const ExprHandle& a) { return acos(a); });
    } break;

    case aten::asin: {
      return ComputeOneOperand(
          "aten_asin", v, [](const ExprHandle& a) { return asin(a); });
    } break;

    case aten::cosh: {
      return ComputeOneOperand(
          "aten_cosh", v, [](const ExprHandle& a) { return cosh(a); });
    } break;

    case aten::sinh: {
      return ComputeOneOperand(
          "aten_sinh", v, [](const ExprHandle& a) { return sinh(a); });
    } break;

    case aten::atan: {
      return ComputeOneOperand(
          "aten_atan", v, [](const ExprHandle& a) { return atan(a); });
    } break;

    case aten::atan2: {
      return ComputeTwoOperand(
          "aten_atan2", v, [](const ExprHandle& lhs, const ExprHandle& rhs) { return atan2(lhs, rhs); });
    } break;

    case aten::tanh: {
      return ComputeOneOperand("aten_tanh", v, [](const ExprHandle& a) {
        // return
        // (ExprHandle(-.67436811832e-5f)+(ExprHandle(.2468149110712040f)+(ExprHandle(.583691066395175e-1f)+ExprHandle(.3357335044280075e-1f)*a)*a)*a)/(ExprHandle(.2464845986383725f)+(ExprHandle(.609347197060491e-1f)+(ExprHandle(.1086202599228572f)+ExprHandle(.2874707922475963e-1f)*a)*a)*a);
        return tanh(a);
      });
    } break;

    case aten::sqrt: {
      return ComputeOneOperand(
          "aten_sqrt", v, [](const ExprHandle& a) { return sqrt(a); });
    } break;

    case aten::rsqrt: {
      return ComputeOneOperand(
          "aten_rsqrt", v, [](const ExprHandle& a) { return rsqrt(a); });
    } break;

    case aten::abs: {
      return ComputeOneOperand(
          "aten_abs", v, [](const ExprHandle& a) { return fabs(a); });
    } break;

    case aten::ceil: {
      return ComputeOneOperand(
          "aten_ceil", v, [](const ExprHandle& a) { return ceil(a); });
    } break;

    case aten::floor: {
      return ComputeOneOperand(
          "aten_floor", v, [](const ExprHandle& a) { return floor(a); });
    } break;

    case aten::round: {
      return ComputeOneOperand(
          "aten_round", v, [](const ExprHandle& a) { return round(a); });
    } break;

    case aten::trunc: {
      return ComputeOneOperand(
          "aten_trunc", v, [](const ExprHandle& a) { return trunc(a); });
    } break;

    case aten::threshold: {
      return ComputeThreeOperand(
          "aten_threshold", v, [](const ExprHandle& a, const ExprHandle& threshold, const ExprHandle& value) {
            return ifThenElse(CompareSelect::make(a, threshold, kGT), a, value);
      });
    } break;

    case aten::frac: {
      return ComputeOneOperand(
          "aten_frac", v, [](const ExprHandle& a) { return a - floor(a); });
    } break;

    case aten::lgamma: {
      return ComputeOneOperand(
          "aten_lgamma", v, [](const ExprHandle& a) { return lgamma(a); });
    } break;

    case prim::ConstantChunk: {
      return Compute(
          "prim_constantchunk",
          texprDims(v),
          [this, v](const std::vector<VarHandle>& axes) {
            auto const& n = v->node();
            int64_t dim = n->i(attr::dim);
            int64_t chunks = n->i(attr::chunks);
            return chunk(
                tensors_.at(n->inputs()[0]->unique()),
                v->offset(),
                dim,
                chunks,
                axes);
          });
    }

    case aten::cat: {
      return Compute(
          "aten_cat", texprDims(v), [this, v](const std::vector<VarHandle>& axes) {
            auto const& n = v->node();
            auto inputs = n->inputs()[0]->node()->inputs();
            size_t dim = n->inputs()[1]->node()->i(attr::value);

            std::vector<ExprHandle> new_axes(axes.begin(), axes.end());
            ExprHandle load = tensorOrConstant(inputs[0], new_axes);
            size_t offset = bufferSizes(tensors_.at(inputs[0]->unique()))[dim];
            new_axes[dim] = new_axes[dim] - IntImm::make(offset);

            for (int ii = 1; ii < inputs.size(); ++ii) {
              load = ifThenElse(
                  CompareSelect::make(axes[dim], IntImm::make(offset), kLT),
                  load,
                  tensorOrConstant(inputs[ii], new_axes));
              offset += bufferSizes(tensors_.at(inputs[ii]->unique()))[dim];
              new_axes[dim] = new_axes[dim] - IntImm::make(offset);
            }

            return load;
          });
    }

    case aten::slice: {
      return Compute(
          "aten_slice", texprDims(v), [this, v](const std::vector<VarHandle>& axes) {
            auto const& n = v->node();
            int dim = constant(n->inputs()[1]).AsNode<IntImm>()->value();
            ExprHandle start = constant(n->inputs()[2]);
            ExprHandle stride = constant(n->inputs()[4]);

            std::vector<ExprHandle> new_axes(axes.begin(), axes.end());
            new_axes[dim] = stride*new_axes[dim] + start;
            return tensorOrConstant(n->inputs()[0], new_axes);
          });
    }

    case aten::unsqueeze: {
      return Compute(
          "aten_unsqueeze", texprDims(v), [this, v](const std::vector<VarHandle>& axes) {
            auto const& n = v->node();
            int dim = constant(n->inputs()[1]).AsNode<IntImm>()->value();
            if (dim < 0) {
              dim += axes.size() - 1;
            }

            std::vector<ExprHandle> new_axes(axes.begin(), axes.end());
            new_axes.erase(new_axes.begin()+dim);
            return tensorOrConstant(n->inputs()[0], new_axes);
          });
    }

    case aten::_sigmoid_backward: {
      return ComputeTwoOperand(
          "aten_sigmoid_backward", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            return lhs * rhs * (ExprHandle(1.0f) - rhs);
          });
    }

    case aten::_tanh_backward: {
      return ComputeTwoOperand(
          "aten_tanh_backward", v, [](const ExprHandle& lhs, const ExprHandle& rhs) {
            return lhs * (ExprHandle(1.0f) - rhs * rhs);
          });
    }

    default: {
      throw std::runtime_error("Unhandled node kind");
    }
  }
}

void TensorExprKernel::LowerToBackend(BackendType backend_type) {
  std::vector<Tensor*> tensor_outputs(tensor_outputs_);

  if (backend_type == BackendType::kCudaCodeGen) {
    for (int i = 0; i < tensor_outputs_.size(); i++) {
      Tensor* tensor = tensor_outputs_[i];
      ExprHandle total_count = tensor->function()->dim(0);
      for (int i = 1; i < tensor->function()->ndim(); i++) {
        total_count = total_count * tensor->function()->dim(i);
      }
      // Flatten the index for GPU kernels.
      // TODO: move this to fusing axis when it is ready.
      Tensor* new_out = Compute(
          tensor->function()->func_var().name_hint() + "_flat",
          {total_count},
          [tensor](const VarHandle& index) -> ExprHandle {
            std::vector<ExprHandle> dims;
            ExprHandle value = index;
            for (int i = tensor->function()->ndim() - 1; i >= 0; i--) {
              ExprHandle idx = value;
              if (i > 0) {
                idx = Mod::make(value, tensor->function()->dim(i));
              }
              dims.push_back(idx);
              value = value / tensor->function()->dim(i);
            }
            std::reverse(dims.begin(), dims.end());
            return tensor->call(dims);
          });
      tensor_outputs[i] = new_out;
    }
  }

  torch::jit::tensorexpr::schedule::Schedule sch(tensor_outputs);

  // Compute non-output tensors_ inline
  for (auto& p : tensors_) {
    p.second->ComputeInline();
  }
  if (backend_type == kCudaCodeGen) {
    for (int i = 0; i < tensor_outputs_.size(); i++) {
      tensor_outputs_[i]->ComputeInline();

      // TODO: implement splitting of variable axes.  Until then, skip this
      // optimization when axes are dynamic.
      bool dynamicShapes = false;
      for (auto const& dim : tensor_outputs_[i]->function()->dims()) {
        if (!dim.AsNode<IntImm>()) {
          dynamicShapes = true;
          break;
        }
      }
      if (dynamicShapes) {
        continue;
      }
      Tensor* tensor = tensor_outputs[i];
      VarHandle index = tensor->function()->arg(0);
      int loop_levels = GetTECudaPointwiseLoopLevels();
      const int kDefaultLoopLevels = 2;
      loop_levels = (loop_levels > 0) ? loop_levels : kDefaultLoopLevels;
      int block_count = GetTECudaPointwiseBlockCount();
      int block_size = GetTECudaPointwiseBlockSize();

      if (loop_levels == 2) {
	VarHandle outer;
	VarHandle inner;
	int kDefaultBlockSize = 512;
	if (block_size < 0) {
	  block_size = kDefaultBlockSize;
	}
	tensor->SplitWithMask(index, block_size, true, &outer, &inner);
	tensor->GPUExecConfig({outer}, {inner});
      } else if (loop_levels == 3) {
	VarHandle outer;
	VarHandle inner;
	VarHandle inner_1;
	VarHandle inner_2;
	// TODO: change the number of microprocessors
	const int kDefaultBlockCount = 1280;
	const int kDefaultBlockSize = 256;
	block_count = (block_count > 0) ? block_count : kDefaultBlockCount;
	block_size = (block_size > 0) ? block_size : kDefaultBlockSize;
	tensor->SplitWithMask(index, block_count * block_size, true, &outer, &inner);
	tensor->SplitWithMask(inner, block_size, true, &inner_1, &inner_2);
	tensor->GPUExecConfig({inner_1}, {inner_2});
      } else {
	throw std::runtime_error("Invalid loop-level: " + std::to_string(loop_levels));
      }
    }
  }

  Stmt* stmt = sch.Lower();

  // Set up formal params (inputs, then outputs) for kernel.
  std::vector<CodeGen::BufferArg> params;
  for (auto const& arg : kernelArgs_) {
    params.push_back(arg.buffer());
    for (auto const& size : arg.sizes()) {
      params.push_back(size.var);
    }
    for (auto const& stride : arg.strides()) {
      params.push_back(stride.var);
    }
  }
  for (auto& o : tensor_outputs) {
    params.push_back(o);
  }

  // Generate code.
  std::string codegen_name;
  switch (backend_type_) {
    case kCudaCodeGen:
      codegen_name = "cuda_codegen";
      break;
    case kLLVMCodeGen:
      codegen_name = "llvm_codegen";
      break;
    case kSimpleIREval:
      codegen_name = "simple_ir_eval";
      break;
    default:
      throw std::runtime_error(
          "invalid backend type: " +
          std::to_string(static_cast<int>(backend_type_)));
  }
  codegen_ = CreateCodeGen(codegen_name, stmt, params);
}

void TensorExprKernel::PickAndCheckBackendType(
    const at::ArrayRef<IValue>& inputs) {
  at::Device device = [&inputs]() {
    for (auto const& input : inputs) {
      if (input.isTensor()) {
        return input.toTensor().device();
      }
    }
    throw std::runtime_error("No tensor inputs");
  }();
  BackendType backend_type = BackendType::kUninitialized;
  if (device.type() == at::kCUDA) {
    backend_type = kCudaCodeGen;
  } else if (device.type() == at::kCPU) {
#ifdef ENABLE_LLVM
    backend_type = kLLVMCodeGen;
#else
    backend_type = kSimpleIREval;
    ;
#endif
  } else {
    throw std::runtime_error("Invalid device type");
  }

  if (backend_type_ == kUninitialized) {
    backend_type_ = backend_type;
    device_ = device;
    LowerToBackend(backend_type);
  } else if (backend_type_ != backend_type) {
    // TODO: if we have to support muliptole backends with the same subgraph,
    // we need to add kernel caching.
    throw std::runtime_error(
        "Inconsistent backend_type: " + std::to_string(backend_type_) + " vs " +
        std::to_string(backend_type));
  }
}

void TensorExprKernel::CodeGenRun(
    const std::vector<CodeGen::CallArg>& run_args) {
  switch (backend_type_) {
    case kSimpleIREval:
    case kLLVMCodeGen:
    case kCudaCodeGen:
      codegen_->call(run_args);
      break;
    default:
      throw std::runtime_error(
          "Invalid backend type: " + std::to_string(backend_type_));
  }
}

ExprHandle TensorExprKernel::createInputIndexExpr(
    const Buffer& buffer,
    const std::vector<VarHandle>& axes,
    const c10::VaryingShape& sizes,
    const c10::VaryingStrides& strides,
    const c10::VaryingStrides& contiguity,
    const std::unordered_map<int64_t, VarHandle>& sizeVars) {
  TORCH_CHECK(
      axes.size() == strides.size(), "strides and axes are not the same size");

  std::vector<ShapeArg> strideArgs;
  std::vector<ShapeArg> sizeArgs;
  ExprHandle stride = 1;
  ExprHandle index = 0;
  int n = axes.size() - 1;

  for (int i = 0; i < axes.size(); i++) {
    // For discontiguous tensors, create a parameter to represent stride.
    if (!*contiguity[i]) {
      VarHandle v =
          VarHandle{"stride_" + buffer.data().name_hint() + "_" + std::to_string(i),
              kInt32};
      strideArgs.emplace_back(n - i, v);
      stride = v;
    }

    // If size is dynamic (indicated by negative value) create a size param.
    ExprHandle size;
    auto sizeVal = *sizes[n - i];
    if (sizeVal < 0) {
      auto it = sizeVars.find(sizeVal);
      TORCH_CHECK(it != sizeVars.end());
      auto const& v = it->second;
      sizeArgs.emplace_back(n - i, v);
      size = v;
    } else {
      size = int32_t{sizeVal};
    }

    index = index + axes[n - i] * stride;
    stride = stride * size;
  }

  kernelArgs_.emplace_back(buffer, std::move(sizeArgs), std::move(strideArgs));
  return buffer(index);
}

void TensorExprKernel::bindInput(const torch::jit::Value* input) {
  auto const& t = input->type();
  switch (t->kind()) {
    case TypeKind::TensorType: {
      auto tt = input->type()->cast<TensorType>();
      Buffer in_buffer(
          "t" + input->debugName(), texprType(tt->scalarType()), {0});
      std::vector<DimArg> inputTensorDims;
      std::unordered_map<int64_t, VarHandle> sizeVars;
      for (int i = 0; i < *tt->sizes().size(); i++) {
        auto const& size = *tt->sizes()[i];
        if (size < 0) {
          VarHandle v(
              "size_" + std::to_string(input->unique()) + "_" +
                  std::to_string(i),
              kInt32);
          sizeVars.emplace(size, v);
          inputTensorDims.push_back(v);
        } else {
          inputTensorDims.push_back({int32_t{size}, "i" + std::to_string(i)});
        }
      }
      tensors_.emplace(
          input->unique(),
          Compute("input", inputTensorDims, [&](const std::vector<VarHandle>& axes) {
            return createInputIndexExpr(
                in_buffer,
                axes,
                tt->sizes(),
                tt->strides(),
                tt->contiguity(),
                sizeVars);
          }));
      break;
    }
    case TypeKind::FloatType: {
      VarHandle v("v" + input->debugName(), kFloat32);
      kernelArgs_.push_back(v);
      scalars_.emplace(input->unique(), v);
      break;
    }
    case TypeKind::IntType: {
      VarHandle v("v" + input->debugName(), kInt32);
      kernelArgs_.push_back(v);
      scalars_.emplace(input->unique(), v);
      break;
    }
    default: {
      LOG(FATAL) << "Unhandled input type: " << *t;
      break;
    }
  }
}

TensorExprKernel::TensorExprKernel(const Graph& subgraph) {
  KernelScope kernel_scope(&kernel_arena_);

  // Bind inputs to buffers.
  n_inputs_ = subgraph.inputs().size();
  for (auto const& input : subgraph.inputs()) {
    bindInput(input);
  }

  // Bind nodes to tensor compute expressions.
  for (auto const& n : subgraph.nodes()) {
    if (n->kind() == prim::Constant || n->kind() == prim::ListConstruct) {
      continue;
    } else {
      for (auto const& output : n->outputs()) {
        if (output->hasUses()) {
          tensors_.emplace(output->unique(), ComputeValue(output));
        }
      }
    }
  }

  // Move output operands from `tensors_` to `tensor_outputs_`
  for (const auto& output : subgraph.outputs()) {
    CHECK(tensors_.count(output->unique())) << "Output must be a tensor";
    tensor_outputs_.emplace_back(tensors_.at(output->unique()));
    tensors_.erase(output->unique());
  }
}

void TensorExprKernel::run(Stack& stack) {
  KernelScope kernel_scope(&kernel_arena_);
  // Set up arguments (inputs, then outputs) for kernel call.
  auto inputs = last(stack, n_inputs_);
  PickAndCheckBackendType(inputs);

  std::map<const Expr*, int32_t> varToSize;

  std::vector<CodeGen::CallArg> run_args;
  for (int i = 0; i < inputs.size(); i++) {
    auto const& input = inputs[i];
    if (input.isInt()) {
      run_args.push_back((int32_t)input.toInt());
    } else if (input.isDouble()) {
      run_args.push_back((float)input.toDouble());
    } else if (input.isTensor()) {
      auto const& tensor = input.toTensor();
      run_args.push_back(tensor.data_ptr());
      for (auto const& size : kernelArgs_[i].sizes()) {
        int32_t s = tensor.sizes()[size.idx];
        run_args.push_back(s);
        varToSize[size.var.node()] = s;
      }
      for (auto const& stride : kernelArgs_[i].strides()) {
        int32_t s = tensor.strides()[stride.idx];
        run_args.push_back(s);
      }
    }
  }

  std::vector<at::Tensor> outputs;
  for (auto& o : tensor_outputs_) {
    std::vector<int64_t> tensorSize;
    for (auto const& dim : o->function()->dims()) {
      auto it = varToSize.find(dim.node());
      if (it != varToSize.end()) {
        tensorSize.push_back(it->second);
      } else {
        auto const& s = dim.AsNode<IntImm>();
        TORCH_CHECK(s);
        tensorSize.push_back(s->value());
      }
    }

    outputs.push_back(at::empty(
        tensorSize, c10::TensorOptions(tensorType(o)).device(device_)));
    run_args.push_back(outputs.back().data_ptr());
  }

  // Call the kernel.
  CodeGenRun(run_args);

  // Update the stack.
  drop(stack, n_inputs_);
  for (auto& o : outputs) {
    push_one(stack, std::move(o));
  }
}
