/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file src/relay/backend/contrib/ethosn/codegen.cc
 * \brief The Relay -> Ethos-N command stream compiler.
 */
#include <tvm/relay/expr_functor.h>
#include <tvm/runtime/module.h>

#include "capabilities.h"
#include "codegen_ethosn.h"
#include "ethosn_api.h"

namespace tvm {
namespace relay {
namespace contrib {
namespace ethosn {

sl::TensorInfo GetTensorInfo(std::map<Expr, std::vector<sl::TensorInfo>> tensor_table,
                             const Call& call) {
  if (tensor_table.find(call) != tensor_table.end()) return tensor_table[call][0];

  return sl::TensorInfo();
}

bool IsEthosnOp(const Call& call, const std::string& op_name) {
  if (call->op->IsInstance<OpNode>()) {
    Op op = Downcast<Op>(call->op);
    CHECK(op.defined());
    return op == Op::Get(op_name);
  } else {
    return false;
  }
}

std::map<Expr, std::vector<sl::TensorInfo>> InferTensorsVisitor::Infer(const Expr& expr) {
  tensor_table_.clear();
  CHECK(expr->checked_type().defined());
  size_t output_size = 1;
  if (auto tuple = expr->checked_type().as<TupleTypeNode>()) {
    output_size = tuple->fields.size();
  }
  for (size_t i = 0; i < output_size; i++) {
    tensor_table_[expr].push_back(sl::TensorInfo({1, 1, 1, 1}, sl::DataType::UINT8_QUANTIZED,
                                                 sl::DataFormat::NHWC, sl::QuantizationInfo()));
  }
  VisitInferred(expr);
  return tensor_table_;
}

void InferTensorsVisitor::InferCall(const CallNode* cn) {
  EthosnError err;
  Call call = GetRef<Call>(cn);
  // Determine call -> NPU mapping
  if (IsEthosnOp(call, "qnn.concatenate")) {
    ConcatenateParams params;
    err = EthosnAPI::Concatenate(call, &params);
    tensor_table_[cn->args[0]] = params.input_infos;
  } else if (IsEthosnOp(call, "split")) {
    SplitParams params;
    params.input_info = GetTensorInfo(tensor_table_, call);
    err = EthosnAPI::Split(call, &params);
    tensor_table_[cn->args[0]] = {params.input_info};
  } else {
    err = EthosnError("unknown operator");
  }
  if (err) {
    ReportFatalError(call, err);
  }
}

// This will only visit an expression if the expression's tensor info
// has already been entirely inferred.
// An example where this is important is a tuple node where each
// get item node will only infer one field of the tuple's expression info.
// We don't want to traverse the tuple until all of its fields have been inferred.
void InferTensorsVisitor::VisitInferred(const Expr& expr) {
  if (tensor_table_.find(expr) != tensor_table_.end()) {
    for (const auto& tensor_info : tensor_table_[expr]) {
      if (tensor_info == sl::TensorInfo()) return;
    }
    VisitExpr(expr);
  }
}

void InferTensorsVisitor::VisitExpr_(const CallNode* cn) {
  InferCall(cn);
  // Pre-order visitor
  for (const auto& arg : cn->args) {
    VisitInferred(arg);
  }
}

void InferTensorsVisitor::VisitExpr_(const TupleNode* tn) {
  auto tuple = GetRef<Tuple>(tn);
  CHECK(tensor_table_.find(tuple) != tensor_table_.end());
  for (size_t i = 0; i < tn->fields.size(); i++) {
    tensor_table_[tn->fields[i]] = {tensor_table_[tuple][i]};
  }
  // Pre-order visitor
  for (const auto& field : tn->fields) {
    VisitExpr(field);
  }
}

void InferTensorsVisitor::VisitExpr_(const TupleGetItemNode* tgn) {
  // Don't assume it must be targeting a TupleNode
  // Vars and calls can still have TupleType
  auto tg = GetRef<TupleGetItem>(tgn);
  CHECK(tensor_table_.find(tg) != tensor_table_.end());
  auto tuple = tg->tuple;
  auto type = tuple->checked_type().as<TupleTypeNode>();
  int index = tg->index;
  // Resize the tensor infos to the tuple size if not already done
  if (tensor_table_.find(tuple) == tensor_table_.end()) {
    tensor_table_[tuple].resize(type->fields.size());
  }
  tensor_table_[tuple][index] = tensor_table_[tg][0];
  // Pre-order visitor
  VisitInferred(tuple);
}

sl::TensorsAndId MakeOps(const sl::TensorAndId<sl::Operand>& op) {
  sl::TensorsAndId ops;
  ops.tensors = {op.tensor};
  ops.operationId = op.operationId;
  return ops;
}

NetworkWithIDs ConstructNetworkVisitor::Construct(const Function& func) {
  // Initialise everything
  NetworkWithIDs network_with_ids;
  network_ = sl::CreateNetwork();
  network_with_ids.network = network_;
  operand_table_.clear();

  // Infer tensor information
  tensor_table_ = InferTensors(this->mod_, this->var_, func->body);
  // Add the inputs in the order they appear in the parameters
  unsigned int idx = 0;
  for (const auto& param : func->params) {
    for (const auto& tensor_info : tensor_table_[param]) {
      auto tensor_and_id = AddInput(network_, tensor_info);
      operand_table_[param].push_back(tensor_and_id.tensor);
      id_table_[param].push_back(std::make_pair(tensor_and_id.operationId, 0));
      network_with_ids.input_ids[tensor_and_id.operationId] = idx++;
    }
  }
  // Add the function body
  VisitExpr(func->body);
  // Add the outputs
  idx = 0;
  for (const auto& layer : operand_table_[func->body]) {
    AddOutput(network_, *layer);
    network_with_ids.output_ids[id_table_[func->body][idx]] = idx;
    idx++;
  }
  return network_with_ids;
}

sl::TensorsAndId ConstructNetworkVisitor::HandleCall(const CallNode* cn) {
  EthosnError err;
  Call call = GetRef<Call>(cn);
  sl::TensorAndId<sl::Operand> tensor;
  sl::TensorsAndId tensors;
  // Determine call -> NPU mapping
  if (IsEthosnOp(call, "qnn.concatenate")) {
    if ((err = MakeConcatenateLayer(call, &tensor))) ReportFatalError(call, err);
    return MakeOps(tensor);
  } else if (IsEthosnOp(call, "split")) {
    if ((err = MakeSplitLayer(call, &tensors))) ReportFatalError(call, err);
    return tensors;
  } else {
    ReportFatalError(call, EthosnError("unknown operator"));
    return {};
  }
}

void ConstructNetworkVisitor::VisitExpr_(const CallNode* cn) {
  auto operand = HandleCall(cn);
  operand_table_[GetRef<Call>(cn)] = operand.tensors;
  for (size_t i = 0; i < operand.tensors.size(); i++) {
    id_table_[GetRef<Call>(cn)].push_back(std::make_pair(operand.operationId, i));
  }
}

void ConstructNetworkVisitor::VisitExpr_(const TupleNode* op) {
  Tuple tuple = GetRef<Tuple>(op);
  for (const auto& arg : tuple->fields) {
    // The fields in a tuple should not themselves be tuples
    // Nested tuples are not supported
    if (operand_table_[arg].size() == 1) {
      operand_table_[tuple].push_back(operand_table_[arg][0]);
      id_table_[tuple].push_back(id_table_[arg][0]);
    } else {
      operand_table_[tuple].push_back(nullptr);
      id_table_[tuple].push_back(std::make_pair(0, 0));
    }
  }
}

void ConstructNetworkVisitor::VisitExpr_(const TupleGetItemNode* tg) {
  Expr tuple = tg->tuple;
  operand_table_[GetRef<TupleGetItem>(tg)] = {operand_table_[tuple][tg->index]};
  id_table_[GetRef<TupleGetItem>(tg)] = {id_table_[tuple][tg->index]};
}

void ConstructNetworkVisitor::VisitLeaf(const Expr& expr) {
  // Don't traverse into functions, they're not supported
  if (!expr->IsInstance<FunctionNode>()) MixedModeVisitor::VisitLeaf(expr);
}

EthosnError ConstructNetworkVisitor::MakeConcatenateLayer(const Call& call,
                                                          sl::TensorAndId<sl::Operand>* out) {
  ConcatenateParams params;
  if (auto err = EthosnAPI::Concatenate(call, &params)) {
    return err;
  }

  std::vector<sl::Operand*> layers;
  auto ops = operand_table_[call->args[0]];

  for (const auto& op : ops) {
    layers.emplace_back(op.get());
  }
  try {
    *out = AddConcatenation(network_, layers, params.concat_info);
  } catch (const sl::NotSupportedException& e) {
    return EthosnError(e.what());
  }
  return EthosnError();
}

EthosnError ConstructNetworkVisitor::MakeSplitLayer(const Call& call, sl::TensorsAndId* outs) {
  SplitParams params;
  params.input_info = GetTensorInfo(tensor_table_, call);
  if (auto err = EthosnAPI::Split(call, &params)) {
    return err;
  }

  auto input = operand_table_[call->args[0]][0];

  try {
    *outs = AddSplit(network_, *input, params.split_info);
  } catch (const sl::NotSupportedException& e) {
    return EthosnError(e.what());
  }
  return EthosnError();
}

runtime::Module EthosnCompiler::CreateRuntimeModule(const ObjectRef& ref) {
  std::vector<runtime::ethosn::OrderedCompiledNetwork> cmms;
  if (ref->IsInstance<FunctionNode>()) {
    IRModule mod;
    Function func = Downcast<Function>(ref);
    auto name_node = func->GetAttr<String>(tvm::attr::kGlobalSymbol);
    CHECK(name_node.defined()) << "Failed to retrieved external symbol.";
    GlobalVar gvar = GlobalVar(name_node.value());
    mod->Add(gvar, func);
    Function mod_func = Downcast<Function>(mod->functions.at(gvar));
    cmms.emplace_back(CompileEthosnFunc(mod, gvar, mod_func));
  } else {
    LOG(FATAL) << "The input ref is expected to be a Relay function";
  }
  auto n = make_object<runtime::ethosn::EthosnModule>(&cmms);
  return runtime::Module(n);
}

runtime::ethosn::OrderedCompiledNetwork EthosnCompiler::CompileEthosnFunc(const IRModule& mod,
                                                                          const GlobalVar& gvar,
                                                                          const Function& func) {
  // Construct the network
  auto network_with_ids = ConstructNetwork(mod, gvar, func);
  // Now set the required build flags
  sl::CompilationOptions options = CreateOptions();
  // Finally compile the network
  std::vector<std::unique_ptr<sl::CompiledNetwork>> compiled_networks =
      sl::Compile(*network_with_ids.network, options);
  CHECK_GE(compiled_networks.size(), 1) << "Ethos-N compiler failed to compile network";
  auto compiled_network = std::move(compiled_networks[0]);
  // Determine the order that the inputs/outputs are in and how that corresponds to the
  // order that the TVM runtime will expect them in
  auto input_output_order = GetInputOutputOrder(network_with_ids, compiled_network);
  // Use the order information to create an 'ordered' network with includes how to map
  // the inputs/outputs from the TVM runtime to the inputs/outputs of the compiled network
  runtime::ethosn::OrderedCompiledNetwork ordered_network;
  ordered_network.name = gvar->name_hint;
  ordered_network.cmm = std::move(compiled_network);
  ordered_network.inputs = input_output_order.first;
  ordered_network.outputs = input_output_order.second;
  return ordered_network;
}

sl::CompilationOptions EthosnCompiler::CreateOptions() {
  auto ctx = transform::PassContext::Current();
  auto cfg = ctx->GetConfig<EthosnCompilerConfig>("relay.ext.ethos-n.options");
  if (!cfg.defined()) {
    cfg = AttrsWithDefaultValues<EthosnCompilerConfig>();
  }

  sl::CompilationOptions options(variants[cfg.value()->variant]);
  options.m_Strategy0 = cfg.value()->strategy0;
  options.m_Strategy1 = cfg.value()->strategy1;
  options.m_Strategy3 = cfg.value()->strategy3;
  options.m_Strategy4 = cfg.value()->strategy4;
  options.m_Strategy6 = cfg.value()->strategy6;
  options.m_Strategy7 = cfg.value()->strategy7;
  options.m_DebugInfo.m_DumpRam = cfg.value()->dump_ram;
  options.m_DebugInfo.m_InitialSramDump = cfg.value()->initial_sram_dump;
  options.m_BlockConfig16x16 = cfg.value()->block_config_16x16;
  options.m_BlockConfig32x8 = cfg.value()->block_config_32x8;
  options.m_BlockConfig8x32 = cfg.value()->block_config_8x32;
  options.m_BlockConfig8x8 = cfg.value()->block_config_8x8;
  options.m_EnableIntermediateCompression = cfg.value()->enable_intermediate_compression;
  options.m_DisableWinograd = cfg.value()->disable_winograd;
  options.m_DebugInfo.m_DumpDebugFiles = cfg.value()->dump_debug_files;
  options.m_DebugInfo.m_DebugDir = cfg.value()->debug_dir;
  options.m_EnableCascading = cfg.value()->enable_cascading;
  return options;
}

std::pair<std::vector<uint32_t>, std::vector<uint32_t>> EthosnCompiler::GetInputOutputOrder(
    NetworkWithIDs network, const std::unique_ptr<sl::CompiledNetwork>& compiled_network) {
  std::vector<sl::InputBufferInfo> input_infos = compiled_network->GetInputBufferInfos();
  std::vector<sl::OutputBufferInfo> output_infos = compiled_network->GetOutputBufferInfos();
  std::vector<uint32_t> input_order;
  std::vector<uint32_t> output_order;
  // Find the order of the inputs in the compiled network
  for (const auto& input_info : input_infos) {
    input_order.push_back(network.input_ids[input_info.m_SourceOperationId]);
  }
  // Find the order of the outputs in the compiled network
  for (const auto& output_info : output_infos) {
    auto output_id =
        std::make_pair(output_info.m_SourceOperationId, output_info.m_SourceOperationOutputIndex);
    output_order.push_back(network.output_ids[output_id]);
  }
  return std::make_pair(input_order, output_order);
}

}  // namespace ethosn
}  // namespace contrib
}  // namespace relay
}  // namespace tvm