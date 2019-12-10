#pragma once
#include <torch/csrc/WindowsTorchApiMacro.h>

namespace torch {
namespace jit {
namespace tensorexpr {

class Add;
class Sub;
class Mul;
class Div;
class Max;
class Min;
class CompareSelect;
class IntImm;
class FloatImm;
class Cast;
class Variable;
class Let;
class Ramp;
class Load;
class For;
class Block;
class Store;
class Broadcast;
class Expr;
class Stmt;
class BaseCallNode;
class Intrinsics;
class FunctionCall;
class Allocate;
class Free;

class TORCH_API IRMutator {
 public:
  virtual ~IRMutator() {}
  virtual Expr mutate(const Add* v);
  virtual Expr mutate(const Sub* v);
  virtual Expr mutate(const Mul* v);
  virtual Expr mutate(const Div* v);
  virtual Expr mutate(const Max* v);
  virtual Expr mutate(const Min* v);
  virtual Expr mutate(const CompareSelect* v);
  virtual Expr mutate(const IntImm* v);
  virtual Expr mutate(const FloatImm* v);
  virtual Expr mutate(const Cast* v);
  virtual Expr mutate(const Variable* v);
  virtual Expr mutate(const Let* v);
  virtual Expr mutate(const Ramp* v);
  virtual Expr mutate(const Load* v);
  virtual Expr mutate(const Broadcast* v);
  // BaseCallNode is the base class for all call nodes.
  // For any visitors that only needs the common behavior, only override this
  // function is enough. This is because all derived class handlers will call
  // this function by default.
  // Override the derived class handler only if the logic is more specific to
  // that.
  virtual Expr mutate(const BaseCallNode* v);
  virtual Expr mutate(const Intrinsics* v);
  virtual Expr mutate(const FunctionCall* v);

  virtual Stmt mutate(const For* v);
  virtual Stmt mutate(const Block* v);
  virtual Stmt mutate(const Store* v);

  virtual Stmt mutate(const Allocate* v);
  virtual Stmt mutate(const Free* v);
};

} // namespace tensorexpr
} // namespace jit
} // namespace torch
