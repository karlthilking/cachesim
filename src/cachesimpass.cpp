#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

namespace {
class CacheSimPass : public PassInfoMixin<CacheSimPass> {
private:
    void setFunctionAttrs(Function *F)
    {
        F->setDoesNotThrow();
        F->addFnAttr(Attribute::NoFree);
        F->addFnAttr(Attribute::WillReturn);
        F->addFnAttr(Attribute::NoSync);
        F->addFnAttr(Attribute::NoRecurse);
        F->setMemoryEffects(MemoryEffects::inaccessibleMemOnly());
    }
public:
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &_)
    {
        for (Function &F : M) {
            LLVMContext &C = F.getContext();

            FunctionCallee storeDataHook = M.getOrInsertFunction(
                "__cachesim_store_data",
                FunctionType::get(
                    Type::getVoidTy(C),
                    {PointerType::getUnqual(C)},
                    false
                )
            );
            setFunctionAttrs(dyn_cast<Function>(storeDataHook.getCallee()));
                
            FunctionCallee loadDataHook = M.getOrInsertFunction(
                "__cachesim_load_data",
                FunctionType::get(
                    Type::getVoidTy(C),
                    {Type::getVoidTy(C)},
                    false
                )
            );
            setFunctionAttrs(dyn_cast<Function>(loadDataHook.getCallee()));

            // FunctionCallee loadInstrHook = M.getOrInsertFunction(
            //     "__cachesim_load_instr",
            //     FunctionType::get(
            //         Type::getVoidTy(C),
            //         {Type::getInt64Ty(C)},
            //         false
            //     )
            // );
            // setFunctionAttrs(dyn_cast<Function>(loadInstrHook.getCallee()));

            for (BasicBlock &BB: F) {
                for (Instruction &I : BB) {
                    IRBuilder<> Builder(&I);
                    // InlineAsm *getPC = InlineAsm::get(
                    //     FunctionType::get(Builder.getInt64Ty(), false),
                    //     "adr %0, .",
                    //     "=r",
                    //     false
                    // );
                    // Value *pc = Builder.CreateCall(getPC);
                    // Builder.CreateCall(loadInstrHook, {pc});
                    if (auto *LI = dyn_cast<LoadInst>(&I)) {
                        Builder.CreateCall(
                            loadDataHook, 
                            LI->getPointerOperand()
                        );
                    } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
                        Builder.CreateCall(
                            storeDataHook,
                            SI->getPointerOperand()
                        );
                    }
                }
            }
        }
        
        return PreservedAnalyses::none();
    }

    static bool isRequired() { return true; }
};
} // anonymous namespace

PassPluginLibraryInfo getCacheSimPluginInfo()
{
    const auto registerCacheSimCallback = [](PassBuilder &PB) {
        PB.registerOptimizerLastEPCallback(
            [](ModulePassManager &PM, 
               [[maybe_unused]] OptimizationLevel Level,
               [[maybe_unused]] ThinOrFullLTOPhase Phase) {
                PM.addPass(CacheSimPass());
            });
    };

    return {LLVM_PLUGIN_API_VERSION, "CacheSimPass", 
            LLVM_VERSION_STRING, registerCacheSimCallback};
}

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo 
llvmGetPassPluginInfo()
{
    return getCacheSimPluginInfo();
}

