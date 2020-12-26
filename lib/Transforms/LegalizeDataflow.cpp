//===------------------------------------------------------------*- C++ -*-===//
//
//===----------------------------------------------------------------------===//

#include "Dialect/HLSKernel/HLSKernel.h"
#include "Transforms/Passes.h"
#include "mlir/Dialect/Linalg/IR/LinalgOps.h"

using namespace std;
using namespace mlir;
using namespace scalehls;

namespace {
struct LegalizeDataflow : public LegalizeDataflowBase<LegalizeDataflow> {
  void runOnOperation() override;
};
} // namespace

static bool isDataflowOp(Operation *op) {
  return !isa<AllocOp, AllocaOp, ConstantOp, TensorLoadOp, TensorToMemrefOp,
              ReturnOp>(op);
}

// For storing the intermediate memory and successor loops indexed by the
// predecessor loop.
using Successors = SmallVector<std::pair<Value, Operation *>, 2>;
using SuccessorsMap = DenseMap<Operation *, Successors>;

static void getSuccessorsMap(Block &block, SuccessorsMap &map) {
  DenseMap<Operation *, SmallPtrSet<Value, 2>> memsMap;
  DenseMap<Value, SmallPtrSet<Operation *, 2>> loopsMap;

  for (auto loop : block.getOps<AffineForOp>())
    loop.walk([&](Operation *op) {
      if (auto affineStore = dyn_cast<AffineStoreOp>(op)) {
        memsMap[loop].insert(affineStore.getMemRef());

      } else if (auto store = dyn_cast<StoreOp>(op)) {
        memsMap[loop].insert(store.getMemRef());

      } else if (auto affineLoad = dyn_cast<AffineLoadOp>(op)) {
        loopsMap[affineLoad.getMemRef()].insert(loop);

      } else if (auto load = dyn_cast<LoadOp>(op)) {
        loopsMap[load.getMemRef()].insert(loop);
      }
    });

  // Find successors of all operations. Since this is a dataflow analysis, this
  // traverse will not enter any control flow operations.
  for (auto &op : block.getOperations()) {
    // Loops need to be separately handled.
    if (auto loop = dyn_cast<AffineForOp>(op)) {
      for (auto mem : memsMap[loop]) {
        for (auto successor : loopsMap[mem]) {
          // If the successor loop not only loads from the memory, but also
          // store to the memory, it is considered as a legal successor.
          if (successor == loop || memsMap[successor].count(mem))
            continue;

          map[loop].push_back(std::pair<Value, Operation *>(mem, successor));
        }
      }
    } else if (isDataflowOp(&op)) {
      for (auto result : op.getResults()) {
        for (auto successor : result.getUsers()) {
          // If the intermediate result is not shaped type, or the successor is
          // not a dataflow operation, it is considered as a legal successor.
          if (!result.getType().isa<ShapedType>() || !isDataflowOp(successor))
            continue;

          map[&op].push_back(std::pair<Value, Operation *>(result, successor));
        }
      }
    }
  }
}

void LegalizeDataflow::runOnOperation() {
  auto func = getOperation();
  auto builder = OpBuilder(func);

  SuccessorsMap successorsMap;
  getSuccessorsMap(func.front(), successorsMap);

  llvm::SmallDenseMap<int64_t, int64_t, 16> dataflowToMerge;

  // Walk through all dataflow operations in a reversed order for establishing a
  // ALAP scheduling.
  for (auto i = func.front().rbegin(); i != func.front().rend(); ++i) {
    auto op = &*i;
    if (isDataflowOp(op)) {
      int64_t dataflowLevel = 0;

      // Walk through all successor ops.
      for (auto pair : successorsMap[op]) {
        auto successor = pair.second;
        if (auto attr = successor->getAttrOfType<IntegerAttr>("dataflow_level"))
          dataflowLevel = max(dataflowLevel, attr.getInt());
        else {
          op->emitError("has unexpected successor, legalization failed");
          return;
        }
      }

      // Set an attribute for indicating the scheduled dataflow level.
      op->setAttr("dataflow_level", builder.getIntegerAttr(builder.getI64Type(),
                                                           dataflowLevel + 1));

      // Eliminate bypass paths if detected.
      for (auto pair : successorsMap[op]) {
        auto value = pair.first;
        auto successor = pair.second;
        auto successorDataflowLevel =
            successor->getAttrOfType<IntegerAttr>("dataflow_level").getInt();

        // Bypass path does not exist.
        if (dataflowLevel == successorDataflowLevel)
          continue;

        // If insert-copy is set, insert CopyOp to the bypass path. Otherwise,
        // record all the bypass paths in dataflowToMerge.
        if (insertCopy) {
          // Insert CopyOps if required.
          SmallVector<Value, 4> values;
          values.push_back(value);

          builder.setInsertionPoint(successor);
          for (auto i = dataflowLevel; i > successorDataflowLevel; --i) {
            // Create CopyOp.
            Value newValue;
            Operation *copyOp;
            if (auto valueType = value.getType().dyn_cast<MemRefType>()) {
              newValue = builder.create<mlir::AllocOp>(op->getLoc(), valueType);
              copyOp = builder.create<linalg::CopyOp>(op->getLoc(),
                                                      values.back(), newValue);
            } else {
              copyOp = builder.create<hlskernel::CopyOp>(
                  op->getLoc(), value.getType(), values.back());
              newValue = copyOp->getResult(0);
            }

            // Set CopyOp dataflow level.
            copyOp->setAttr("dataflow_level",
                            builder.getIntegerAttr(builder.getI64Type(), i));

            // Chain created CopyOps.
            if (i == successorDataflowLevel + 1)
              value.replaceUsesWithIf(newValue, [&](mlir::OpOperand &use) {
                return successor->isAncestor(use.getOwner());
              });
            else
              values.push_back(newValue);
          }
        } else {
          // Always retain the longest merge path.
          if (auto dst = dataflowToMerge.lookup(successorDataflowLevel))
            dataflowToMerge[successorDataflowLevel] = max(dst, dataflowLevel);
          else
            dataflowToMerge[successorDataflowLevel] = dataflowLevel;
        }
      }
    }
  }

  // Collect all operations in each dataflow level.
  DenseMap<int64_t, SmallVector<Operation *, 2>> dataflowOps;
  func.walk([&](Operation *dataflowOp) {
    if (auto attr = dataflowOp->getAttrOfType<IntegerAttr>("dataflow_level"))
      dataflowOps[attr.getInt()].push_back(dataflowOp);
  });

  // Reorder operations that are legalized.
  for (auto pair : dataflowOps) {
    auto ops = pair.second;
    auto lastOp = ops.back();

    for (auto it = ops.begin(); it < std::prev(ops.end()); ++it) {
      auto op = *it;
      op->moveBefore(lastOp);
    }
  }

  // Merge dataflow levels according to the bypasses and minimum granularity.
  if (minGran != 1 || !insertCopy) {
    unsigned newLevel = 1;
    unsigned toMerge = minGran;
    for (unsigned i = 1, e = dataflowOps.size(); i <= e; ++i) {
      // If the current level is the start point of a bypass, refresh toMerge.
      // Otherwise, decrease toMerge by 1.
      if (auto dst = dataflowToMerge.lookup(i))
        toMerge = dst - i;
      else
        toMerge--;

      // Annotate all ops in the current level to the new level.
      for (auto op : dataflowOps[i])
        op->setAttr("dataflow_level",
                    builder.getIntegerAttr(builder.getI64Type(), newLevel));

      // Update toMerge and newLevel if required.
      if (toMerge == 0) {
        toMerge = minGran;
        newLevel++;
      }
    }
  }

  // Set dataflow attribute.
  func.setAttr("dataflow", builder.getBoolAttr(true));
}

std::unique_ptr<mlir::Pass> scalehls::createLegalizeDataflowPass() {
  return std::make_unique<LegalizeDataflow>();
}
