set(LLVM_TARGET_DEFINITIONS LLVMOps.td)
mlir_tablegen(LLVMOps.h.inc -gen-op-decls)
mlir_tablegen(LLVMOps.cpp.inc -gen-op-defs)
mlir_tablegen(LLVMOpsEnums.h.inc -gen-enum-decls)
mlir_tablegen(LLVMOpsEnums.cpp.inc -gen-enum-defs)
add_public_tablegen_target(MLIRLLVMOpsIncGen)
set(LLVM_TARGET_DEFINITIONS NVVMOps.td)
mlir_tablegen(NVVMOps.h.inc -gen-op-decls)
mlir_tablegen(NVVMOps.cpp.inc -gen-op-defs)
add_public_tablegen_target(MLIRNVVMOpsIncGen)
set(LLVM_TARGET_DEFINITIONS LLVMOps.td)
mlir_tablegen(LLVMConversions.inc -gen-llvmir-conversions)
add_public_tablegen_target(MLIRLLVMConversionsIncGen)
set(LLVM_TARGET_DEFINITIONS NVVMOps.td)
mlir_tablegen(NVVMConversions.inc -gen-llvmir-conversions)
add_public_tablegen_target(MLIRNVVMConversionsIncGen)
