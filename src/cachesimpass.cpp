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
        F->addFnAttr(Attribute::NoRecurse);
        F->addFnAttr(Attribute::NoUnwind);
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
                    {PointerType::getUnqual(C)},
                    false
                )
            );
            setFunctionAttrs(dyn_cast<Function>(loadDataHook.getCallee()));

            FunctionCallee loadInstHook = M.getOrInsertFunction(
                "__cachesim_load_instr",
                FunctionType::get(
                    Type::getVoidTy(C),
                    {PointerType::getInt64Ty(C), 
                     PointerType::getInt64Ty(C)},
                    false
                )
            );
            setFunctionAttrs(dyn_cast<Function>(loadInstHook.getCallee()));
#if defined(__aarch64__)
            InlineAsm *getPC = InlineAsm::get(
                FunctionType::get(
                    Type::getInt64Ty(C), 
                    {Type::getVoidTy(C)}, 
                    false
                ),
                "adr $0, .", "=r", false
            );
#elif defined(__x86_64)
            InlineAsm *getPC = InlineAsm::get(
                FunctionType::get(
                    Type::getInt64Ty(C),
                    {Type::getVoidTy(C)},
                    false,
                    false,
                    AD_ATT
                ),
                "lea 0(%rip), $0", "=r", false
            );
#endif

            for (BasicBlock &BB: F) {
                SmallVector<LoadInst *, 16> loads;
                SmallVector<StoreInst *, 16> stores;
                SmallVector<AtomicRMWInst *, 16> atomics;
                SmallVector<AtomicCmpXchgInst *, 16> xchgs;

                for (Instruction &I : BB) {
                    if (I.isDebugOrPseudoInst())
                        continue;
                    else if (LoadInst *LI = dyn_cast<LoadInst>(&I))
                        loads.push_back(LI);
                    else if (StoreInst *SI = dyn_cast<StoreInst>(&I))
                        stores.push_back(SI);
                    else if (AtomicRMWInst *RMW = dyn_cast<AtomicRMWInst>(&I))
                        atomics.push_back(RMW);
                    else if (AtomicCmpXchgInst *xchg = 
                             dyn_cast<AtomicCmpXchgInst>(&I))
                        xchgs.push_back(xchg);
                }
                
                Instruction &start = *BB.getFirstInsertionPt();
                Instruction &end = *BB.getTerminator();

                IRBuilder StartBuilder(&start);
                Value *startPC = StartBuilder.CreateCall(getPC);

                IRBuilder EndBuilder(&end);
                Value *endPC = EndBuilder.CreateCall(getPC);
                EndBuilder.CreateCall(loadInstHook, {startPC, endPC});

                for (LoadInst *LI : loads) {
                    IRBuilder<> B(LI);
                    B.CreateCall(loadDataHook, LI->getPointerOperand());
                }
                for (StoreInst *SI : stores) {
                    IRBuilder<> B(SI);
                    B.CreateCall(storeDataHook, SI->getPointerOperand());
                }
                for (AtomicRMWInst *RMW : atomics) {
                    IRBuilder<> B(RMW);
                    B.CreateCall(storeDataHook, RMW->getPointerOperand());
                }
                for (AtomicCmpXchgInst *CmpXchg : xchgs) {
                    IRBuilder<> B(CmpXchg);
                    B.CreateCall(storeDataHook, CmpXchg->getPointerOperand());
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

