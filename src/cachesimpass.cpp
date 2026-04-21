#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
using namespace llvm;

namespace {
class CacheSimPass : public PassInfoMixin<CacheSimPass> {
    PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM)
    {
        Module *M = F.getParent();
        DataLayout &DL = M->getDataLayout();
        LLVMContext &C = M->getContext();
        
        FunctionCallee storeDataHook = M->getOrInsertFunction(
            "__cachesim_store_data", 
            FunctionType::get(
                Type::getVoidTy(C),
                {PointerType::getUnqual(C)},
                false
            )
        );

        FunctionCallee loadDataHook = M->getOrInsertFunction(
            "__cachesim_load_data",
            FunctionType::get(
                Type::getVoidTy(C), 
                {PointerType::getUnqual(C)}, 
                false
            )
        );

        FunctionCallee loadInstrHook = M->getOrInsertFunction(
            "__cachesim_load_instr",
            FunctionType::get(
                Type::getVoidTy(c), 
                {PointerType::getUnqual(C)}, 
                false
            )
        );
    
        for (BasicBlock &BB : F) {
            for (Instruction &I : BB) {
                IRBuilder<> Builder(&I);
                if (auto *LI = dyn_cast<LoadInst>(&I)) {
                    IRBuilder<> Builder(LI);
                    Value *op = LI->getPointerOperand();
                    Builder.CreateCall(loadDataHook, {op});
                } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
                    IRBuilder<> Builder(SI);
                    Value *op = SI->getPointerOperand();
                    Builder.CreateCall(storeDataHook, {op});
                }
            }
        }
    
        return PreservedAnalyses::none();
    }
};
} // anonymous namespace

PassPluginLibraryInfo getCacheSimPluginInfo()
{
    auto registerCacheSimCallback = [](PassBuilder &PB) {
        PB.registerOptimizerLastEPCallback(
            [](FunctionPassManger &PM, OptimizationLevel Level) {
                PB.addPass(CacheSimPass());
            });
    };

    return {LLVM_PLUGIN_API_VERSION, "CacheSimPass", 
            LLVM_VERSION_STRING, registerCacheSimCallback};
}

extern "C" LLVM_ATTIBUTE_WEAK ::llvm::PassPluginLibraryInfo 
llvmGetPassPluginInfo()
{
    return getCacheSimPluginInfo();
}

