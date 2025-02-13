#include <torch/csrc/jit/codegen/cuda/dispatch.h>
#include <torch/csrc/jit/codegen/cuda/fusion.h>
#include <torch/csrc/jit/codegen/cuda/ir_all_nodes.h>
#include <torch/csrc/jit/codegen/cuda/ir_cloner.h>
#include <torch/csrc/jit/codegen/cuda/ir_printer.h>
#include <torch/csrc/jit/codegen/cuda/mutator.h>

#include <torch/csrc/jit/ir/ir.h>

#include <c10/util/Exception.h>
#include <c10/util/irange.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace torch {
namespace jit {
namespace fuser {
namespace cuda {

Statement::Statement(const Statement* src, IrCloner* ir_cloner) {
  name_ = src->name_;
  fusion_ = ir_cloner->fusion();
  ir_cloner->registerClone(src, this);
}

Val* Statement::asVal() {
  TORCH_INTERNAL_ASSERT(isVal(), "Cannot cast to Val as this is not a Val.");
  return this->as<Val>();
}

Expr* Statement::asExpr() {
  TORCH_INTERNAL_ASSERT(isExpr(), "Cannot cast to Expr as this is not a Expr.");
  return this->as<Expr>();
}

void Statement::print() const {
  IrPrinter ir_printer(std::cout);
  ir_printer.handle(this);
  std::cout << std::endl;
}

// When we create a Val we immediately register them with the active fusion.
Val::Val(ValType _vtype, DataType _dtype, bool register_val)
    : vtype_(_vtype), dtype_(_dtype) {
  Fusion* fusion = FusionGuard::getCurFusion();
  TORCH_CHECK(
      fusion != nullptr, "No active fusion group found when creating a Val.");
  fusion_ = fusion;
  if (register_val) {
    name_ = fusion_->registerVal(this);
  }
}

// NOTE: we don't clone the definition_ and uses_ here
//  since they may introduce cloning cycles. Instead, we copy
//  the original pointers and we'll fix them up later part of the
//  Fusion copy. Neither definition_ nor uses_ are copied through
//  this constructor now leaving them to be resolved by later stages
//
Val::Val(const Val* src, IrCloner* ir_cloner)
    : Statement(src, ir_cloner),
      vtype_(src->vtype_),
      dtype_(src->dtype_),
      is_fusion_input_(src->is_fusion_input_),
      is_fusion_output_(src->is_fusion_output_) {}

const std::vector<Expr*>& Val::uses() const {
  if (vtype_ == ValType::TensorView) {
    if (!fusion()->isTVUseInfoValid() && !fusion()->isUpdatingTVUseInfo()) {
      fusion()->resetTvUses();
    }
  }
  return uses_;
}

namespace {

// Traverse definition of all values involved in constructing the provided val.
// Check if all values involved are constant values, meaning the provided
// val is also a constant value.
class ConstCheck : OptOutConstDispatch {
 private:
  bool is_const_ = true;

  void handle(const Bool* b) override {
    is_const_ = is_const_ && b->isConst();
  }

  void handle(const Double* d) override {
    is_const_ = is_const_ && d->isConst();
  }

  void handle(const Int* i) override {
    is_const_ = is_const_ && i->isConst();
  }

  void handle(const NamedScalar* ns) override {
    is_const_ = is_const_ && false;
  }

  void handle(const Expr* expr) override {
    for (auto inp : expr->inputs()) {
      handle(inp);
    }
  }

  void handle(const Val* val) override {
    if (val->definition() != nullptr) {
      handle(val->definition());
    } else {
      OptOutConstDispatch::handle(val);
    }
  }

 public:
  static bool isConst(const Val* val) {
    ConstCheck cc;
    cc.handle(val);
    return cc.is_const_;
  }
};

} // namespace

bool Val::isConstScalar() const {
  if (!isScalar())
    return false;
  return ConstCheck::isConst(this);
}

c10::optional<int64_t> Val::getInt() const {
  if (isConstScalar() && isAnInt()) {
    if (this->getValType() == ValType::Scalar) {
      return this->as<Int>()->value();
    }
  }
  return c10::optional<int64_t>();
}

bool Val::isZeroInt() const {
  auto int_val = getInt();
  return int_val.has_value() && int_val.value() == 0;
}

bool Val::isOneInt() const {
  auto int_val = getInt();
  return int_val.has_value() && int_val.value() == 1;
}

c10::optional<DataType> Val::getDataType() const {
  TORCH_INTERNAL_ASSERT(
      dtype_ != DataType::Null, "Value does not have a data type.");
  return dtype_;
}

bool Val::isProducerOf(const Val* other) const {
  TORCH_INTERNAL_ASSERT(other != nullptr);
  TORCH_INTERNAL_ASSERT(fusion() == other->fusion());

  if (definition() == nullptr) {
    return false;
  }
  return std::any_of(
      definition()->inputs().begin(),
      definition()->inputs().end(),
      [other](const Val* input) { return input == other; });
}

bool Val::isConsumerOf(const Val* other) const {
  return other->isProducerOf(this);
}

// We don't register with the active fusion in Expr as this needs to be done
// after inputs and outputs are registered with the Expr
Expr::Expr(ExprType type) : type_{type} {
  Fusion* fusion = FusionGuard::getCurFusion();
  if (fusion == nullptr)
    TORCH_CHECK(false, "No active fusion group found when creating an Expr.");
  fusion_ = fusion;
}

Expr::Expr(const Expr* src, IrCloner* ir_cloner)
    : Statement(src, ir_cloner),
      type_(src->type_),
      inputs_(ir_cloner->clone(src->inputs_)),
      outputs_(ir_cloner->clone(src->outputs_)) {}

bool Expr::sameAs(const Statement* other) const {
  if (this == other) {
    return true;
  }
  if (!other->isA<Expr>()) {
    return false;
  }
  const Expr* other_expr = other->as<Expr>();
  if (getExprType() != other_expr->getExprType()) {
    return false;
  }
  if (inputs().size() != other_expr->inputs().size() ||
      outputs().size() != other_expr->outputs().size()) {
    return false;
  }
  for (const auto i : c10::irange(inputs().size())) {
    if (!input(i)->sameAs(other_expr->input(i))) {
      return false;
    }
  }
  return true;
}

} // namespace cuda
} // namespace fuser
} // namespace jit
} // namespace torch
