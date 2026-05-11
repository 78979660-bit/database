#pragma once

#include <memory>
#include <string>
#include <vector>

#ifdef ENABLE_JIT
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/ExecutionEngine/MCJIT.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm-c/ExecutionEngine.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/GenericValue.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/ADT/StringMap.h>
#endif

#include "../Expressions/AbstractExpression.h"
#include "../Expressions/ConstantValueExpression.h"
#include "../Expressions/ColumnValueExpression.h"
#include "../Expressions/ComparisonExpression.h"
#include "../Expressions/ArithmeticExpression.h"
#include "../Expressions/LogicalExpression.h"
#include "../../Type/Value.h"

namespace Database
{

    /**
     * @brief JitEngine provides dynamic compilation capabilities for expression trees.
     * By translating an expression tree (which normally requires expensive virtual function calls per row)
     * into pure machine code function via LLVM, execution speeds up tremendously for large batch scans.
     */
    class JitEngine
    {
    public:
        JitEngine()
        {
#ifdef ENABLE_JIT
            llvm::InitializeNativeTarget();
            llvm::InitializeNativeTargetAsmPrinter();
            llvm::InitializeNativeTargetAsmParser();
            LLVMLinkInMCJIT();

            context_ = std::make_unique<llvm::LLVMContext>();
            module_ = std::make_unique<llvm::Module>("DatabaseJIT", *context_);
            builder_ = std::make_unique<llvm::IRBuilder<>>(*context_);
#endif
                        }

        ~JitEngine() = default;

        llvm::Type *GetLLVMType(TypeId type)
        {
#ifdef ENABLE_JIT
            switch (type)
            {
            case TypeId::BOOLEAN:
                return llvm::Type::getInt1Ty(*context_);
            case TypeId::TINYINT:
                return llvm::Type::getInt8Ty(*context_);
            case TypeId::SMALLINT:
                return llvm::Type::getInt16Ty(*context_);
            case TypeId::INTEGER:
                return llvm::Type::getInt32Ty(*context_);
            case TypeId::BIGINT:
            case TypeId::TIMESTAMP:
                return llvm::Type::getInt64Ty(*context_);
            case TypeId::DECIMAL:
                return llvm::Type::getDoubleTy(*context_);
            case TypeId::VARCHAR:
                return llvm::PointerType::getUnqual(*context_);
            default:
                return nullptr;
                        }
#else
            return nullptr;
#endif
                        }

        /**
         * @brief Compile an AbstractExpression tree into a native C function pointer
         * that takes an array of primitive inputs (the Tuple data) and returns a primitive value.
         */
        typedef void (*CompiledExpressionFunc)(const void *row_data, void *result, uint8_t *is_null);

        // Vectorized Batch Function Signature:
        // cols: array of pointers to flat vectors for each column (e.g. cols[0] = col0_data)
        // result: pointer to flat vector output for expressions
        // null_bitmap: pointer to flat boolean output marking nulls
        // count: number of rows to process
        typedef void (*CompiledBatchFunc)(const void **cols, void *result, uint8_t *null_bitmap, uint32_t count);

        CompiledExpressionFunc CompileExpression(const AbstractExpression *expr)
        {
#ifdef ENABLE_JIT
            if (!expr || !CanJitCompile(expr))
                return nullptr;

            if (!module_)
            {
                module_ = std::make_unique<llvm::Module>("DatabaseJIT", *context_);
                        }

            // 1. Create function signature: void func(void* row_data, void* result, uint8_t* is_null)
            llvm::Type *void_type = llvm::Type::getVoidTy(*context_);
            llvm::Type *ptr_type = llvm::PointerType::getUnqual(*context_);
            llvm::FunctionType *func_type = llvm::FunctionType::get(void_type, {ptr_type, ptr_type, ptr_type}, false);

            llvm::Function *func = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage, "eval_expr", module_.get());

            auto arg_it = func->args().begin();
            llvm::Value *row_data_arg = arg_it++;
            llvm::Value *result_arg = arg_it++;
            llvm::Value *is_null_arg = arg_it++;

            llvm::BasicBlock *entry_block = llvm::BasicBlock::Create(*context_, "entry", func);
            builder_->SetInsertPoint(entry_block);

            try
            {
                std::pair<llvm::Value *, llvm::Value *> return_pair = GenerateIR(expr, row_data_arg);

                // Store result
                llvm::Type *res_type = return_pair.first->getType();
                builder_->CreateStore(return_pair.first, builder_->CreateBitCast(result_arg, llvm::PointerType::getUnqual(*context_)));

                // Store null indicator
                builder_->CreateStore(builder_->CreateZExt(return_pair.second, llvm::Type::getInt8Ty(*context_)), is_null_arg);
                        }
            catch (const std::exception &e)
            {
                return nullptr;
                        }

            // 3. Return void
            builder_->CreateRetVoid();

            // [向量化终极优化] 1. 配置 LLVM PassManager 开�?LoopVectorize �?SLPVectorize
            llvm::LoopAnalysisManager LAM;
            llvm::FunctionAnalysisManager FAM;
            llvm::CGSCCAnalysisManager CGAM;
            llvm::ModuleAnalysisManager MAM;

            llvm::PassBuilder PB;
            PB.registerModuleAnalyses(MAM);
            PB.registerCGSCCAnalyses(CGAM);
            PB.registerFunctionAnalyses(FAM);
            PB.registerLoopAnalyses(LAM);
            PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

            // OptimizationLevel::O3 invokes loop vectorization and SLP vectorization
            llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
            MPM.run(*module_, MAM);

            // [宿主�?CPU 极致利用] 2. 读取当前宿主机（物理机）支持的所有指令集特征（AVX2, AVX-512等）
            llvm::StringMap<bool> host_features = llvm::sys::getHostCPUFeatures();
            std::vector<std::string> feature_attrs;
            for (auto &f : host_features)
            {
                feature_attrs.push_back(std::string(f.second ? "+" : "-") + f.first().str());
                        }

            // 3. JIT compile the module into host machine code
            std::string err_str;
            engines_.push_back(std::unique_ptr<llvm::ExecutionEngine>(llvm::EngineBuilder(std::move(module_))
                                                                          .setErrorStr(&err_str)
                                                                          .setEngineKind(llvm::EngineKind::JIT)
                                                                          .setOptLevel(llvm::CodeGenOptLevel::Aggressive)
                                                                          .setMCPU(llvm::sys::getHostCPUName().str())
                                                                          .setMAttrs(feature_attrs)
                                                                          .create()));

            if (!engines_.back())
            {
                throw std::runtime_error("Failed to create ExecutionEngine: " + err_str);
                        }

            engines_.back()->finalizeObject();
            void *func_ptr = engines_.back()->getPointerToFunction(func);
            return reinterpret_cast<CompiledExpressionFunc>(func_ptr);
#else
            // JIT not enabled at compile time, fallback to an empty pointer
            return nullptr;
#endif
                        }

        CompiledBatchFunc CompileBatchExpression(const AbstractExpression *expr)
        {
#ifdef ENABLE_JIT
            if (!expr || !CanJitCompile(expr))
                return nullptr;

            if (!module_)
            {
                module_ = std::make_unique<llvm::Module>("DatabaseBatchJIT", *context_);
                        }

            llvm::Type *int32_type = llvm::Type::getInt32Ty(*context_);
            llvm::Type *void_ptr_type = llvm::PointerType::getUnqual(*context_);
            llvm::Type *void_ptr_ptr_type = llvm::PointerType::getUnqual(*context_);
            llvm::Type *int8_ptr_type = llvm::PointerType::getUnqual(*context_);

            llvm::FunctionType *func_type = llvm::FunctionType::get(
                llvm::Type::getVoidTy(*context_),
                {void_ptr_ptr_type, void_ptr_type, int8_ptr_type, int32_type}, false);

            llvm::Function *func = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage, "eval_batch_expr", module_.get());

            auto arg_it = func->args().begin();
            llvm::Value *cols_arg = arg_it++;
            cols_arg->setName("cols");
            llvm::Value *result_arg = arg_it++;
            result_arg->setName("result");
            llvm::Value *null_bitmap_arg = arg_it++;
            null_bitmap_arg->setName("null_bitmap");
            llvm::Value *count_arg = arg_it++;

            // [打破别名惩罚] 明确告诉 LLVM：这些指针互相独立（相当�?C �?__restrict__ 关键字）
            // 如果不加 NoAlias，自动向量化�?(Auto-Vectorizer) 会因为�?result 写入覆盖�?cols 数据�?拒绝 SIMD 展开"�?
            func->addParamAttr(0, llvm::Attribute::NoAlias);
            func->addParamAttr(1, llvm::Attribute::NoAlias);
            func->addParamAttr(2, llvm::Attribute::NoAlias);
            count_arg->setName("count");

            llvm::BasicBlock *entry_block = llvm::BasicBlock::Create(*context_, "entry", func);
            llvm::BasicBlock *cond_block = llvm::BasicBlock::Create(*context_, "loop_cond", func);
            llvm::BasicBlock *body_block = llvm::BasicBlock::Create(*context_, "loop_body", func);
            llvm::BasicBlock *exit_block = llvm::BasicBlock::Create(*context_, "exit", func);

            builder_->SetInsertPoint(entry_block);
            builder_->CreateBr(cond_block);

            // Loop Condition
            builder_->SetInsertPoint(cond_block);
            llvm::PHINode *i_phi = builder_->CreatePHI(int32_type, 2, "i");
            i_phi->addIncoming(llvm::ConstantInt::get(int32_type, 0), entry_block);
            llvm::Value *cmp = builder_->CreateICmpSLT(i_phi, count_arg, "cmp_count");
            builder_->CreateCondBr(cmp, body_block, exit_block);

            // Loop Body
            builder_->SetInsertPoint(body_block);
            std::string err_str;
            try
            {
                std::pair<llvm::Value *, llvm::Value *> expr_res = GenerateBatchIR(expr, cols_arg, i_phi);

                // Deal with data
                llvm::Value *res_ptr = builder_->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context_), result_arg, builder_->CreateMul(i_phi, llvm::ConstantInt::get(int32_type, sizeof(int32_t))), "res_ptr_bytes");
                // For simplicity, assuming output is int32 boolean or data:
                llvm::Value *res_ptr_typed = builder_->CreateBitCast(res_ptr, llvm::PointerType::getUnqual(*context_), "res_ptr_cast");

                // Align types before storing if needed (assume result expects whatever the type tree evaluates to, here likely bool/int32)
                // We'll store it as integer for now
                llvm::Type *res_type = expr_res.first->getType();
                builder_->CreateStore(expr_res.first, res_ptr_typed);

                // Deal with null
                llvm::Value *null_ptr = builder_->CreateInBoundsGEP(llvm::Type::getInt8Ty(*context_), null_bitmap_arg, i_phi, "null_ptr");
                llvm::Value *null_byte = builder_->CreateZExt(expr_res.second, llvm::Type::getInt8Ty(*context_), "null_byte");
                builder_->CreateStore(null_byte, null_ptr);
                        }
            catch (const std::exception &e)
            {
                return nullptr;
                        }

            llvm::Value *next_i = builder_->CreateAdd(i_phi, llvm::ConstantInt::get(int32_type, 1), "next_i");
            i_phi->addIncoming(next_i, body_block);
            builder_->CreateBr(cond_block);

            // Exit
            builder_->SetInsertPoint(exit_block);
            builder_->CreateRetVoid();

            // [向量化终极优化] 1. 配置 LLVM PassManager 开�?LoopVectorize �?SLPVectorize
            llvm::LoopAnalysisManager LAM;
            llvm::FunctionAnalysisManager FAM;
            llvm::CGSCCAnalysisManager CGAM;
            llvm::ModuleAnalysisManager MAM;

            llvm::PassBuilder PB;
            PB.registerModuleAnalyses(MAM);
            PB.registerCGSCCAnalyses(CGAM);
            PB.registerFunctionAnalyses(FAM);
            PB.registerLoopAnalyses(LAM);
            PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

            // OptimizationLevel::O3 invokes loop vectorization and SLP vectorization
            llvm::ModulePassManager MPM = PB.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
            MPM.run(*module_, MAM);

            // [宿主�?CPU 极致利用] 2. 读取当前宿主机（物理机）支持的所有指令集特征（AVX2, AVX-512等）
            llvm::StringMap<bool> host_features = llvm::sys::getHostCPUFeatures();
            std::vector<std::string> feature_attrs;
            for (auto &f : host_features)
            {
                feature_attrs.push_back(std::string(f.second ? "+" : "-") + f.first().str());
                        }

            engines_.push_back(std::unique_ptr<llvm::ExecutionEngine>(llvm::EngineBuilder(std::move(module_))
                                                                          .setErrorStr(&err_str)
                                                                          .setEngineKind(llvm::EngineKind::JIT)
                                                                          .setOptLevel(llvm::CodeGenOptLevel::Aggressive)
                                                                          .setMCPU(llvm::sys::getHostCPUName().str())
                                                                          .setMAttrs(feature_attrs)
                                                                          .create()));

            if (!engines_.back())
                throw std::runtime_error("Failed to create Batch ExecutionEngine: " + err_str);

            engines_.back()->finalizeObject();
            void *func_ptr = engines_.back()->getPointerToFunction(func);
            return reinterpret_cast<CompiledBatchFunc>(func_ptr);
#else
            return nullptr;
#endif
                        }

    private:
        bool CanJitCompile(const AbstractExpression *expr) const
        {
            if (!expr)
                return true;
            if (auto const_expr = dynamic_cast<const ConstantValueExpression *>(expr))
            {
                return const_expr->GetValue().GetTypeId() != TypeId::VARCHAR;
            }
            for (auto child : expr->GetChildren())
            {
                if (!CanJitCompile(child.get()))
                    return false;
                        }
            return true;
                        }

#ifdef ENABLE_JIT
        std::pair<llvm::Value *, llvm::Value *> GenerateIR(const AbstractExpression *expr, llvm::Value *row_data_arg)
        {
            llvm::Value *is_null = llvm::ConstantInt::getFalse(*context_);

            if (auto const_expr = dynamic_cast<const ConstantValueExpression *>(expr))
            {
                Value val = const_expr->GetValue();
                if (val.IsNull())
                {
                    return {llvm::ConstantInt::get(*context_, llvm::APInt(32, 0)), llvm::ConstantInt::getTrue(*context_)};
                        }
                if (val.GetTypeId() == TypeId::VARCHAR)
                {
                    // Mock VARCHAR pointer
                    return {llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_)), is_null};
                        }
                else if (val.GetTypeId() == TypeId::DECIMAL)
                {
                    return {llvm::ConstantFP::get(*context_, llvm::APFloat(0.0)), is_null}; // Mock
                        }
                int32_t val_int = val.GetAsInteger();
                return {llvm::ConstantInt::get(*context_, llvm::APInt(32, val_int, true)), is_null};
                        }
            else if (auto col_expr = dynamic_cast<const ColumnValueExpression *>(expr))
            {
                uint32_t col_idx = col_expr->GetColIdx();
                llvm::Value *idx_val = llvm::ConstantInt::get(*context_, llvm::APInt(32, col_idx, true));

                llvm::Value *val_ptr = builder_->CreateInBoundsGEP(llvm::Type::getInt32Ty(*context_), row_data_arg, idx_val, "val_ptr");
                return {builder_->CreateLoad(llvm::Type::getInt32Ty(*context_), val_ptr, "col_val"), is_null};
                        }
            else if (auto cmp_expr = dynamic_cast<const ComparisonExpression *>(expr))
            {
                auto lhs_res = GenerateIR(cmp_expr->GetChildAt(0), row_data_arg);
                auto rhs_res = GenerateIR(cmp_expr->GetChildAt(1), row_data_arg);
                llvm::Value *any_null = builder_->CreateOr(lhs_res.second, rhs_res.second, "any_null");

                llvm::Value *lhs = lhs_res.first;
                llvm::Value *rhs = rhs_res.first;

                // Example of simple Float vs Int logic
                if (lhs->getType()->isDoubleTy() || rhs->getType()->isDoubleTy())
                {
                    if (!lhs->getType()->isDoubleTy())
                        lhs = builder_->CreateSIToFP(lhs, llvm::Type::getDoubleTy(*context_));
                    if (!rhs->getType()->isDoubleTy())
                        rhs = builder_->CreateSIToFP(rhs, llvm::Type::getDoubleTy(*context_));
                    llvm::Value *cmp_res = nullptr;
                    switch (cmp_expr->GetCompType())
                    {
                    case CompType::Equal:
                        cmp_res = builder_->CreateFCmpOEQ(lhs, rhs, "cmp_eq");
                        break;
                    case CompType::NotEqual:
                        cmp_res = builder_->CreateFCmpONE(lhs, rhs, "cmp_ne");
                        break;
                    case CompType::LessThan:
                        cmp_res = builder_->CreateFCmpOLT(lhs, rhs, "cmp_lt");
                        break;
                    case CompType::LessThanOrEqual:
                        cmp_res = builder_->CreateFCmpOLE(lhs, rhs, "cmp_le");
                        break;
                    case CompType::GreaterThan:
                        cmp_res = builder_->CreateFCmpOGT(lhs, rhs, "cmp_gt");
                        break;
                    case CompType::GreaterThanOrEqual:
                        cmp_res = builder_->CreateFCmpOGE(lhs, rhs, "cmp_ge");
                        break;
                        }
                    return {builder_->CreateZExt(cmp_res, llvm::Type::getInt32Ty(*context_), "cmp_res_i32"), any_null};
                        }

                llvm::Value *cmp_res = nullptr;
                switch (cmp_expr->GetCompType())
                {
                case CompType::Equal:
                    cmp_res = builder_->CreateICmpEQ(lhs, rhs, "cmp_eq");
                    break;
                case CompType::NotEqual:
                    cmp_res = builder_->CreateICmpNE(lhs, rhs, "cmp_ne");
                    break;
                case CompType::LessThan:
                    cmp_res = builder_->CreateICmpSLT(lhs, rhs, "cmp_lt");
                    break;
                case CompType::LessThanOrEqual:
                    cmp_res = builder_->CreateICmpSLE(lhs, rhs, "cmp_le");
                    break;
                case CompType::GreaterThan:
                    cmp_res = builder_->CreateICmpSGT(lhs, rhs, "cmp_gt");
                    break;
                case CompType::GreaterThanOrEqual:
                    cmp_res = builder_->CreateICmpSGE(lhs, rhs, "cmp_ge");
                    break;
                        }
                return {builder_->CreateZExt(cmp_res, llvm::Type::getInt32Ty(*context_), "cmp_res_i32"), any_null};
                        }
            else if (auto arith_expr = dynamic_cast<const ArithmeticExpression *>(expr))
            {
                auto lhs_res = GenerateIR(arith_expr->GetChildAt(0), row_data_arg);
                auto rhs_res = GenerateIR(arith_expr->GetChildAt(1), row_data_arg);
                llvm::Value *any_null = builder_->CreateOr(lhs_res.second, rhs_res.second, "any_null");

                llvm::Value *lhs = lhs_res.first;
                llvm::Value *rhs = rhs_res.first;

                llvm::Value *res = nullptr;
                if (lhs->getType()->isDoubleTy() || rhs->getType()->isDoubleTy())
                {
                    if (!lhs->getType()->isDoubleTy())
                        lhs = builder_->CreateSIToFP(lhs, llvm::Type::getDoubleTy(*context_));
                    if (!rhs->getType()->isDoubleTy())
                        rhs = builder_->CreateSIToFP(rhs, llvm::Type::getDoubleTy(*context_));
                    switch (arith_expr->GetArithType())
                    {
                    case ArithType::Add:
                        res = builder_->CreateFAdd(lhs, rhs, "add_res");
                        break;
                    case ArithType::Subtract:
                        res = builder_->CreateFSub(lhs, rhs, "sub_res");
                        break;
                    case ArithType::Multiply:
                        res = builder_->CreateFMul(lhs, rhs, "mul_res");
                        break;
                    case ArithType::Divide:
                        res = builder_->CreateFDiv(lhs, rhs, "div_res");
                        break;
                        }
                        }
                else
                {
                    switch (arith_expr->GetArithType())
                    {
                    case ArithType::Add:
                        res = builder_->CreateAdd(lhs, rhs, "add_res");
                        break;
                    case ArithType::Subtract:
                        res = builder_->CreateSub(lhs, rhs, "sub_res");
                        break;
                    case ArithType::Multiply:
                        res = builder_->CreateMul(lhs, rhs, "mul_res");
                        break;
                    case ArithType::Divide:
                    {
                        // [JIT Magic Number Division] Branchless fast-path for constant divisors
                        if (auto const_rhs = dynamic_cast<const ConstantValueExpression *>(arith_expr->GetChildAt(1))) {
                            int32_t d = const_rhs->GetValue().GetAsInteger();
                            if (d > 1) { // Only apply for positive numbers > 1
                            uint32_t s = 0;
                            while ((1ULL << s) < (uint32_t)d)
                                s++;
                            uint32_t S = 32 + s;
                            uint64_t M = ((1ULL << S) + d - 1) / d;

                            llvm::Value *lhs_64 = builder_->CreateZExt(lhs, llvm::Type::getInt64Ty(*context_), "lhs_64");
                            llvm::Value *magic_val = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), M);
                            llvm::Value *mul_64 = builder_->CreateMul(lhs_64, magic_val, "magic_mul");
                            llvm::Value *shift_val = llvm::ConstantInt::get(llvm::Type::getInt64Ty(*context_), S);
                            llvm::Value *shr_64 = builder_->CreateLShr(mul_64, shift_val, "magic_shr");
                            res = builder_->CreateTrunc(shr_64, llvm::Type::getInt32Ty(*context_), "div_res_magic");
                            break;
                        }
                        }
                        res = builder_->CreateSDiv(lhs, rhs, "div_res");
                        break;
                        }
                        }
                        }
            return {res, any_null};
                        }
        else if (auto logic_expr = dynamic_cast<const LogicalExpression *>(expr))
        {
            auto lhs_res = GenerateIR(logic_expr->GetChildAt(0), row_data_arg);
            auto rhs_res = GenerateIR(logic_expr->GetChildAt(1), row_data_arg);
            llvm::Value *any_null = builder_->CreateOr(lhs_res.second, rhs_res.second, "any_null");

            llvm::Value *lhs_bool = builder_->CreateICmpNE(lhs_res.first, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0), "lhs_bool");
            llvm::Value *rhs_bool = builder_->CreateICmpNE(rhs_res.first, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0), "rhs_bool");
            llvm::Value *logic_res = nullptr;
            switch (logic_expr->GetLogicType())
            {
            case LogicType::AND:
                logic_res = builder_->CreateAnd(lhs_bool, rhs_bool, "and_res");
                break;
            case LogicType::OR:
                logic_res = builder_->CreateOr(lhs_bool, rhs_bool, "or_res");
                break;
                        }
            return {builder_->CreateZExt(logic_res, llvm::Type::getInt32Ty(*context_), "logic_res_i32"), any_null};
                        }
        throw std::runtime_error("Unsupported expression type in JIT GenerateIR");
                        }

    std::pair<llvm::Value *, llvm::Value *>
    GenerateBatchIR(const AbstractExpression *expr, llvm::Value *cols_arg, llvm::Value *i_val)
    {
        llvm::Value *is_null = llvm::ConstantInt::getFalse(*context_);

        if (auto const_expr = dynamic_cast<const ConstantValueExpression *>(expr))
        {
            Value val = const_expr->GetValue();
            if (val.IsNull())
            {
                return {llvm::ConstantInt::get(*context_, llvm::APInt(32, 0)), llvm::ConstantInt::getTrue(*context_)};
                        }
            if (val.GetTypeId() == TypeId::VARCHAR)
            {
                return {llvm::ConstantPointerNull::get(llvm::PointerType::getUnqual(*context_)), is_null};
                        }
            else if (val.GetTypeId() == TypeId::DECIMAL)
            {
                return {llvm::ConstantFP::get(*context_, llvm::APFloat(0.0)), is_null};
                        }
            int32_t val_int = val.GetAsInteger();
            return {llvm::ConstantInt::get(*context_, llvm::APInt(32, val_int, true)), is_null};
                        }
        else if (auto col_expr = dynamic_cast<const ColumnValueExpression *>(expr))
        {
            uint32_t col_idx = col_expr->GetColIdx();
            llvm::Value *idx_val = llvm::ConstantInt::get(*context_, llvm::APInt(32, col_idx, true));
            llvm::Value *col_ptr_ptr = builder_->CreateInBoundsGEP(llvm::PointerType::getUnqual(*context_), cols_arg, idx_val, "col_ptr_ptr");
            llvm::Value *col_ptr = builder_->CreateLoad(llvm::PointerType::getUnqual(*context_), col_ptr_ptr, "col_ptr");

            // Currently defaults to int32 retrieval since types are not strictly known without schema
            llvm::Value *val_ptr = builder_->CreateInBoundsGEP(llvm::Type::getInt32Ty(*context_), col_ptr, i_val, "val_ptr");
            return {builder_->CreateLoad(llvm::Type::getInt32Ty(*context_), val_ptr, "col_val"), is_null};
                        }
        else if (auto cmp_expr = dynamic_cast<const ComparisonExpression *>(expr))
        {
            auto lhs_res = GenerateBatchIR(cmp_expr->GetChildAt(0), cols_arg, i_val);
            auto rhs_res = GenerateBatchIR(cmp_expr->GetChildAt(1), cols_arg, i_val);
            llvm::Value *any_null = builder_->CreateOr(lhs_res.second, rhs_res.second, "any_null");

            llvm::Value *lhs = lhs_res.first;
            llvm::Value *rhs = rhs_res.first;

            if (lhs->getType()->isDoubleTy() || rhs->getType()->isDoubleTy())
            {
                if (!lhs->getType()->isDoubleTy())
                    lhs = builder_->CreateSIToFP(lhs, llvm::Type::getDoubleTy(*context_));
                if (!rhs->getType()->isDoubleTy())
                    rhs = builder_->CreateSIToFP(rhs, llvm::Type::getDoubleTy(*context_));
                llvm::Value *cmp_res = nullptr;
                switch (cmp_expr->GetCompType())
                {
                case CompType::Equal:
                    cmp_res = builder_->CreateFCmpOEQ(lhs, rhs, "cmp_eq");
                    break;
                case CompType::NotEqual:
                    cmp_res = builder_->CreateFCmpONE(lhs, rhs, "cmp_ne");
                    break;
                case CompType::LessThan:
                    cmp_res = builder_->CreateFCmpOLT(lhs, rhs, "cmp_lt");
                    break;
                case CompType::LessThanOrEqual:
                    cmp_res = builder_->CreateFCmpOLE(lhs, rhs, "cmp_le");
                    break;
                case CompType::GreaterThan:
                    cmp_res = builder_->CreateFCmpOGT(lhs, rhs, "cmp_gt");
                    break;
                case CompType::GreaterThanOrEqual:
                    cmp_res = builder_->CreateFCmpOGE(lhs, rhs, "cmp_ge");
                    break;
                        }
                return {builder_->CreateZExt(cmp_res, llvm::Type::getInt32Ty(*context_), "cmp_res_i32"), any_null};
                        }

            llvm::Value *cmp_res = nullptr;
            switch (cmp_expr->GetCompType())
            {
            case CompType::Equal:
                cmp_res = builder_->CreateICmpEQ(lhs, rhs, "cmp_eq");
                break;
            case CompType::NotEqual:
                cmp_res = builder_->CreateICmpNE(lhs, rhs, "cmp_ne");
                break;
            case CompType::LessThan:
                cmp_res = builder_->CreateICmpSLT(lhs, rhs, "cmp_lt");
                break;
            case CompType::LessThanOrEqual:
                cmp_res = builder_->CreateICmpSLE(lhs, rhs, "cmp_le");
                break;
            case CompType::GreaterThan:
                cmp_res = builder_->CreateICmpSGT(lhs, rhs, "cmp_gt");
                break;
            case CompType::GreaterThanOrEqual:
                cmp_res = builder_->CreateICmpSGE(lhs, rhs, "cmp_ge");
                break;
                        }
            return {builder_->CreateZExt(cmp_res, llvm::Type::getInt32Ty(*context_), "cmp_res_i32"), any_null};
                        }
        else if (auto arith_expr = dynamic_cast<const ArithmeticExpression *>(expr))
        {
            auto lhs_res = GenerateBatchIR(arith_expr->GetChildAt(0), cols_arg, i_val);
            auto rhs_res = GenerateBatchIR(arith_expr->GetChildAt(1), cols_arg, i_val);
            llvm::Value *any_null = builder_->CreateOr(lhs_res.second, rhs_res.second, "any_null");

            llvm::Value *lhs = lhs_res.first;
            llvm::Value *rhs = rhs_res.first;

            llvm::Value *res = nullptr;
            if (lhs->getType()->isDoubleTy() || rhs->getType()->isDoubleTy())
            {
                if (!lhs->getType()->isDoubleTy())
                    lhs = builder_->CreateSIToFP(lhs, llvm::Type::getDoubleTy(*context_));
                if (!rhs->getType()->isDoubleTy())
                    rhs = builder_->CreateSIToFP(rhs, llvm::Type::getDoubleTy(*context_));
                switch (arith_expr->GetArithType())
                {
                case ArithType::Add:
                    res = builder_->CreateFAdd(lhs, rhs, "add_res");
                    break;
                case ArithType::Subtract:
                    res = builder_->CreateFSub(lhs, rhs, "sub_res");
                    break;
                case ArithType::Multiply:
                    res = builder_->CreateFMul(lhs, rhs, "mul_res");
                    break;
                case ArithType::Divide:
                    res = builder_->CreateFDiv(lhs, rhs, "div_res");
                    break;
                        }
                        }
            else
            {
                switch (arith_expr->GetArithType())
                {
                case ArithType::Add:
                    res = builder_->CreateAdd(lhs, rhs, "add_res");
                    break;
                case ArithType::Subtract:
                    res = builder_->CreateSub(lhs, rhs, "sub_res");
                    break;
                case ArithType::Multiply:
                    res = builder_->CreateMul(lhs, rhs, "mul_res");
                    break;
                case ArithType::Divide:
                    res = builder_->CreateSDiv(lhs, rhs, "div_res");
                    break;
                        }
                        }
            return {res, any_null};
                        }
        else if (auto logic_expr = dynamic_cast<const LogicalExpression *>(expr))
        {
            auto lhs_res = GenerateBatchIR(logic_expr->GetChildAt(0), cols_arg, i_val);
            auto rhs_res = GenerateBatchIR(logic_expr->GetChildAt(1), cols_arg, i_val);
            llvm::Value *any_null = builder_->CreateOr(lhs_res.second, rhs_res.second, "any_null");

            llvm::Value *lhs_bool = builder_->CreateICmpNE(lhs_res.first, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0), "lhs_bool");
            llvm::Value *rhs_bool = builder_->CreateICmpNE(rhs_res.first, llvm::ConstantInt::get(llvm::Type::getInt32Ty(*context_), 0), "rhs_bool");
            llvm::Value *logic_res = nullptr;
            switch (logic_expr->GetLogicType())
            {
            case LogicType::AND:
                logic_res = builder_->CreateAnd(lhs_bool, rhs_bool, "and_res");
                break;
            case LogicType::OR:
                logic_res = builder_->CreateOr(lhs_bool, rhs_bool, "or_res");
                break;
                        }
            return {builder_->CreateZExt(logic_res, llvm::Type::getInt32Ty(*context_), "logic_res_i32"), any_null};
                        }
        throw std::runtime_error("Unsupported expression type in JIT GenerateBatchIR");
                        }
    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module> module_;
    std::unique_ptr<llvm::IRBuilder<>> builder_;
    std::vector<std::unique_ptr<llvm::ExecutionEngine>> engines_;
#endif
};

} // namespace Database
