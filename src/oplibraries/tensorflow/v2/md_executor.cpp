/* Copyright 2015 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "md_executor.h"
#include "md_executor_impl.h"

#include "oplibraries/tensorflow/v2/exectask.h"
#include "execution/devices.h"
#include "execution/executionengine.h"
#include "platform/logging.h"
#include "utils/threadutils.h"

#include "oplibraries/tensorflow/tensorflow_headers.h"

#include <deque>
#include <memory>
#include <string>
#include <unordered_map>

namespace tf = tensorflow;
namespace gtl = tf::gtl;
using namespace tf::remote;

namespace {
// 1-D, 0 element tensor.
static const auto* const kEmptyTensor = new tf::Tensor;

bool IsInitializationOp(const tf::Node* node) {
    return node->op_def().allows_uninitialized_input();
}
} // namespace

tf::Status NewMultiDeviceExecutor(const tf::MultiDeviceExecutorParams &params, const tf::Graph *graph,
                                  tf::Executor **executor)
{
    auto impl = new ExecutorImpl(params, graph);
    auto s = impl->Initialize();
    if (s.ok()) {
        *executor = impl;
    } else {
        delete impl;
    }
    return s;
}

ExecutorImpl::ExecutorImpl(const tf::MultiDeviceExecutorParams &p, const tf::Graph *g)
    : params_(p)
    , graph_(g)
    , gview_()
{
    CHECK(p.find_kernel != nullptr);
    CHECK(p.create_kernel != nullptr);
}

ExecutorImpl::~ExecutorImpl()
{
    for (auto fiter : frame_info_) {
        delete fiter.second;
    }
    delete graph_;
}

void GetMaxPendingCounts(const tf::Node *n, int *max_pending, int *max_dead_count)
{
    const int num_in_edges = n->in_edges().size();
    int initial_count;
    if (IsMerge(n)) {
        // merge waits all control inputs so we initialize the pending
        // count to be the number of control edges.
        int32_t num_control_edges = 0;
        for (const auto *edge : n->in_edges()) {
            if (edge->IsControlEdge()) {
                num_control_edges++;
            }
        }
        // Use bit 0 to indicate if we are waiting for a ready live data input.
        initial_count = 1 + (num_control_edges << 1);
    } else {
        initial_count = num_in_edges;
    }

    *max_pending = initial_count;
    *max_dead_count = num_in_edges;
}

tf::Status ExecutorImpl::Initialize()
{
    gview_.Initialize(graph_);

    // Build the information about frames in this subgraph.
    ControlFlowInfo cf_info;
    TF_RETURN_IF_ERROR(BuildControlFlowInfo(graph_, &cf_info));

    // Cache this value so we make this virtual function call once, rather
    // that O(# steps * # nodes per step) times.
    //device_record_tensor_accesses_ = params_.device->RequiresRecordingAccessedTensors();

    for (auto &it : cf_info.unique_frame_names) {
        EnsureFrameInfo(it)->nodes = new std::vector<const tf::Node *>;
    }

    // Preprocess every node in the graph to create an instance of op
    // kernel for each node.
    for (const auto *n : graph_->nodes()) {
        const int id = n->id();
        const auto &frame_name = cf_info.frame_names[id];
        auto frame_info = EnsureFrameInfo(frame_name);

        // See if this node is a root node, and if so, add to root_nodes_.
        const int num_in_edges = n->in_edges().size();
        if (num_in_edges == 0) {
            root_nodes_.push_back(n);
        }

        auto item = gview_.node(id);
        item->node = n;

        item->input_start = frame_info->total_inputs;
        frame_info->total_inputs += n->num_inputs();

//         item->kernel_is_expensive = item->kernel->IsExpensive();
        // TODO: Mark all kernel as expensive to put them in our threadpool.
        item->kernel_is_expensive = true;
        item->is_merge = IsMerge(n);
        item->is_enter = IsEnter(n);
        item->is_exit = IsExit(n);
        item->is_control_trigger = IsControlTrigger(n);
        item->is_sink = IsSink(n);
        item->is_enter_exit_or_next_iter = (IsEnter(n) || IsExit(n) || IsNextIteration(n));

        // Compute the maximum values we'll store for this node in the
        // pending counts data structure, and allocate a handle in
        // that frame's pending counts data structure that has enough
        // space to store these maximal count values.
        int max_pending, max_dead;
        GetMaxPendingCounts(n, &max_pending, &max_dead);
        item->pending_id = frame_info->pending_counts_layout.CreateHandle(max_pending, max_dead);

        // Initialize static information about the frames in the graph.
        frame_info->nodes->push_back(n);
        if (IsEnter(n)) {
            std::string enter_name;
            TF_RETURN_IF_ERROR(GetNodeAttr(n->def(), "frame_name", &enter_name));
            EnsureFrameInfo(enter_name)->input_count++;
        }
    }

    // Initialize PendingCounts only after item->pending_id is initialized for
    // all nodes.
    InitializePending(graph_, cf_info);

    return tf::Status::OK();
}

tf::Status ExecutorImpl::BuildControlFlowInfo(const tf::Graph *g, ControlFlowInfo *cf_info)
{
    const int num_nodes = g->num_node_ids();
    cf_info->frame_names.resize(num_nodes);

    std::vector<tf::Node *> parent_nodes;
    parent_nodes.resize(num_nodes);
    std::vector<bool> visited;
    visited.resize(num_nodes);

    std::string frame_name;
    std::deque<tf::Node *> ready;

    // Initialize with the root nodes.
    for (auto *n : g->nodes()) {
        if (n->in_edges().empty()) {
            visited[n->id()] = true;
            cf_info->unique_frame_names.insert(frame_name);
            ready.push_back(n);
        }
    }

    while (!ready.empty()) {
        auto *curr_node = ready.front();
        int curr_id = curr_node->id();
        ready.pop_front();

        tf::Node *parent = nullptr;
        if (IsEnter(curr_node)) {
            // Enter a child frame.
            TF_RETURN_IF_ERROR(GetNodeAttr(curr_node->def(), "frame_name", &frame_name));
            parent = curr_node;
        } else if (IsExit(curr_node)) {
            // Exit to the parent frame.
            parent = parent_nodes[curr_id];
            frame_name = cf_info->frame_names[parent->id()];
            parent = parent_nodes[parent->id()];
        } else {
            parent = parent_nodes[curr_id];
            frame_name = cf_info->frame_names[curr_id];
        }

        for (const auto *out_edge : curr_node->out_edges()) {
            auto out = out_edge->dst();
            int out_id = out->id();

            // Add to ready queue if not visited.
            bool is_visited = visited[out_id];
            if (!is_visited) {
                ready.push_back(out);
                visited[out_id] = true;

                // Process the node 'out'.
                cf_info->frame_names[out_id] = frame_name;
                parent_nodes[out_id] = parent;
                cf_info->unique_frame_names.insert(frame_name);
            }
        }
    }

    return tf::Status::OK();
}

void ExecutorImpl::InitializePending(const tf::Graph *graph, const ControlFlowInfo &cf_info)
{
    for (auto &it : cf_info.unique_frame_names) {
        auto finfo = EnsureFrameInfo(it);
        DCHECK_EQ(finfo->pending_counts, nullptr);
        auto counts = new tf::PendingCounts(finfo->pending_counts_layout);
        finfo->pending_counts = counts;
    }
    for (const auto *n : graph->nodes()) {
        const int id = n->id();
        const auto &name = cf_info.frame_names[id];
        int max_pending, max_dead;
        GetMaxPendingCounts(n, &max_pending, &max_dead);
        auto item = gview_.node(id);
        auto counts = EnsureFrameInfo(name)->pending_counts;
        counts->set_initial_count(item->pending_id, max_pending);
    }
}

ExecutorState::ExecutorState(const tf::Executor::Args &args, ExecutorImpl *impl)
    : vlog_(VLOG_IS_ON(1))
    , step_id_(args.step_id)
    , rendezvous_(args.rendezvous)
    , session_state_(args.session_state)
    , tensor_store_(args.tensor_store)
    , step_container_(args.step_container)
    , stats_collector_(args.stats_collector)
    , call_frame_(args.call_frame)
    , impl_(impl)
    , cancellation_manager_(args.cancellation_manager)
    , runner_(args.runner)
    , sync_on_finish_(args.sync_on_finish)
    , num_outstanding_ops_(0)
{
    // We start the entire execution in iteration 0 of the root frame
    // so let us create the root frame and the state for iteration 0.
    // We assume root_frame_->frame_name.empty().
    root_frame_ = new FrameState(impl_, 1);
    root_frame_->frame_id = 0; // must be 0
    root_frame_->InitializeFrameInfo(root_frame_->frame_name);

    // Initialize iteration 0.
    root_frame_->iterations.resize(root_frame_->max_parallel_iterations);
    root_frame_->iterations[0] =
        new IterationState(root_frame_->pending_counts, root_frame_->total_input_tensors);

    outstanding_frames_.insert({root_frame_->frame_name, root_frame_});
}

ExecutorState::~ExecutorState()
{
    for (auto name_frame : outstanding_frames_) {
        delete name_frame.second;
    }
    for (auto it : m_deviceContextMaps) {
        for (auto c : it.second) {
            c->Unref();
        }
    }
}

void ExecutorState::RunAsync(tf::Executor::DoneCallback done)
{
    TRACE("ExecutorState::RunAsync");

    TaggedNodeSeq ready;

    // Initialize the ready queue.
    for (const auto *n : impl_->root_nodes_) {
        DCHECK_EQ(n->in_edges().size(), 0);
        ready.push_back(TaggedNode{n, root_frame_, 0, false});
    }
    if (ready.empty()) {
        done(tf::Status::OK());
    } else {
        num_outstanding_ops_ = ready.size();
        root_frame_->iterations[0]->outstanding_ops = ready.size();
        done_cb_ = done;
        // Schedule to run all the ready ops in thread pool.
        ScheduleReady(ready, nullptr);
    }
}

void ExecutorState::Process(TaggedNode tagged_node, int64_t scheduled_usec)
{
    const GraphView &gview = impl_->gview_;
    TaggedNodeSeq ready;
    TaggedNodeReadyQueue inline_ready;

    // Parameters passed to OpKernel::Compute.
    TensorValueVec inputs;
    DeviceContextVec input_device_contexts;
    AllocatorAttributeVec input_alloc_attrs;

    tf::OpKernelContext::Params params;
    params.step_id = step_id_;
    params.rendezvous = rendezvous_;
    params.session_state = session_state_;
    params.tensor_store = tensor_store_;
    params.cancellation_manager = cancellation_manager_;
    params.call_frame = call_frame_;
    params.step_container = step_container_;
    params.inputs = &inputs;
    params.input_device_contexts = &input_device_contexts;
    params.input_alloc_attrs = &input_alloc_attrs;
    params.runner = &runner_;

    tf::NodeExecStats *stats = nullptr;
    EntryVector outputs;
    bool completed = false;
    inline_ready.push_back(tagged_node);
    while (!inline_ready.empty()) {
        tagged_node = inline_ready.front();
        inline_ready.pop_front();
        auto node = tagged_node.node;
        FrameState *input_frame = tagged_node.input_frame;
        int64_t input_iter = tagged_node.input_iter;
        const size_t id = node->id();
        const NodeItem &item = *gview.node(id);

        TRACE("Get a new node from inline_ready queue");
        // TODO(misard) Replace with a finer-grain enabling flag once we
        // add better optional debugging support.
        if (vlog_ && VLOG_IS_ON(1)) {
            tf::mutex_lock l(input_frame->mu);
            input_frame->GetIteration(input_iter)->mark_started(item.pending_id);
        }

        utils::semaphore se;
        auto nodeTask = std::make_unique<ExecTask>(this, &se,
                                                   tagged_node, ready, inline_ready, stats, params,
                                                   scheduled_usec, outputs,
                                                   inputs, input_device_contexts, input_alloc_attrs,
                                                   completed);
        ExecutionEngine::instance().enqueue<::google::protobuf::Message>(std::move(nodeTask))
        .fail([&](std::exception_ptr) -> ProtoPtr {
            se.notify();
            return {};
        });

        se.wait();
    } // while !inline_ready.empty()

    TRACE("inline ready queue empty");
    // This thread of computation is done if completed = true.
    if (completed)
        Finish();
}

tf::Status ExecutorState::SetupKernel(TaggedNode node, const DeviceItem &ditem, tf::OpKernel **op_kernel)
{
    auto &ndef = node.node->def();

    // First check if we already created the kernel on some device
    tf::OpKernel *kernel = nullptr;
    const tf::Device *device = nullptr;
    auto ok = impl_->params_.find_kernel(ndef, &device, &kernel);

    if (ok.ok() && kernel) {
        // we saw this kernel before, check if the device match
        if (!device) {
            ERR("We've created the kernel, but don't remember its device: {}", ndef);
            auto s = tf::errors::Internal("We've created the kernel, but don't remember its device");
            *op_kernel = nullptr;
            return AttachDef(s, ndef);
        }
        if (device == ditem.device) {
            // We are on the same device, good.
            *op_kernel = kernel;
            return tf::Status::OK();
        }
        ERR("Stateful kernel can not be moved: previously created on {}, now requested on {}",
            device->name(), ditem.device->name());
        return tf::errors::Internal("Stateful kernel can not be moved: previously created on ",
                                    device->name(), ", now requested on ", ditem.device->name());
    } else if (!ok.ok()) {
        ERR("Failed to create kernel with status {} for NodeDef: {}", ok, ndef);
        // continue to create the kernel
    }

    INFO("Creating a kernel for device: {}", ditem.device->name());
    ok = impl_->params_.create_kernel(ndef, ditem.device, ditem.function_library, &kernel);
    if (!ok.ok()) {
        *op_kernel = nullptr;
        ok = AttachDef(ok, ndef);
        ERR("Executor failed to create kernel: {}", ok);
        return ok;
    }
    CHECK(kernel);
    *op_kernel = kernel;
    return tf::Status::OK();
}

tf::DeviceContext * ExecutorState::FindDeviceContext(size_t id, tf::Device* device)
{
    auto it = m_deviceContextMaps.end();
    {
        tensorflow::mutex_lock l(mu_);

        it = m_deviceContextMaps.find(device);
        if (it == m_deviceContextMaps.end()) {
            tensorflow::DeviceContextMap contexts;
            auto ok = device->FillContextMap(impl_->graph_, &contexts);
            if (!ok.ok()) {
                ERR("Filling contextmap failed: {}", ok);
            }
            std::tie(it, std::ignore) = m_deviceContextMaps.emplace(device, std::move(contexts));
        }
    }
    if (it != m_deviceContextMaps.end() && id < it->second.size()) {
        return it->second[id];
    }
    return nullptr;
}

tf::FunctionLibraryRuntime *ExecutorState::FindFunctionLibrary(tf::Device *dev)
{
    tf::mutex_lock l(mu_);

    auto &ptr = m_fruntimes[dev];
    if (!ptr) {
        ptr.reset(impl_->params_.create_fruntime(dev));
    }
    return ptr.get();
}

namespace {
bool onSameDevice(tensorflow::Device *devA, const tensorflow::AllocatorAttributes &attrA,
                  tensorflow::Device *devB, const tensorflow::AllocatorAttributes &attrB)
{
    if (devA == devB) {
        return attrA.on_host() == attrB.on_host();
    } else {
        return attrA.on_host() && attrB.on_host();
    }
}
} // namespace

tf::Status ExecutorState::PrepareInputs(const NodeItem &item, tf::OpKernel *kernel, tf::Device *device,
                                        tf::DeviceContext *device_context,
                                        Entry *first_input, TensorValueVec *inputs,
                                        DeviceContextVec *input_device_contexts,
                                        AllocatorAttributeVec *input_alloc_attrs, bool *is_input_dead)
{
    auto node = item.node;

    inputs->clear();
    inputs->resize(item.num_inputs);
    input_device_contexts->clear();
    input_device_contexts->resize(item.num_inputs);
    input_alloc_attrs->clear();
    input_alloc_attrs->resize(item.num_inputs);

    *is_input_dead = false;

    bool is_merge = item.is_merge;
    for (int i = 0; i < item.num_inputs; ++i) {
        const bool expect_ref = IsRefType(item.input_type(i));
        Entry *entry = first_input + i;

        // i-th input.
        auto inp = &(*inputs)[i];

        // Only merge and transfer nodes can have no-value inputs.
        if (!entry->has_value) {
            if (!is_merge) {
                DCHECK(IsTransferNode(node));
                DCHECK(!entry->val_field_is_set);
                entry->has_value = true;
                entry->val_field_is_set = true;
                entry->val.Init(*kEmptyTensor);
                inp->tensor = entry->val.get();
                *is_input_dead = true;
            }
            continue;
        }

        // Handle every combination of input and op types
        // ----------------------------------------------
        //    Entry   |   OpInput   |   Device   |   Result   |
        // 1  noref         ref          same        reject
        // 2  noref         ref          diff        reject
        // 3   ref          ref          diff        reject
        // 4   ref          ref          same        get ref

        // 5  noref        noref         same        get val
        // 6   ref         noref         same         deref
        // 7  noref        noref         diff        devcopy
        // 8   ref         noref         diff        devcopy

        tensorflow::AllocatorAttributes expected;
        if (kernel->input_memory_types()[i] == tensorflow::HOST_MEMORY) {
            expected.set_on_host(true);
        }
        bool on_same_device = onSameDevice(entry->device, entry->alloc_attr, device, expected);

        if (expect_ref) {
            if (entry->ref == nullptr) {
                // case 1, 2
                ERR("{}-th input expects a ref type: {}", i, node->def());
                return AttachDef(tf::errors::InvalidArgument(i, "-th input expects a ref type"),
                                node->def());
            }

            if (!on_same_device) {
                // case 3
                ERR("Operation {} expects an reference, but input[{}] is on different device.",
                    kernel->name(), i);
                return AttachDef(tf::errors::InvalidArgument(i, "-th input got a ref on different device"),
                                 node->def());
            }
            // case 4
            inp->mutex_if_ref = entry->ref_mu;
            inp->tensor = entry->ref;
        } else {
            if (entry->ref == nullptr) {
                // case 5
                inp->tensor = entry->val.get();
            } else {
                if (!entry->ref->IsInitialized() && !IsInitializationOp(item.node)) {
                    return AttachDef(
                        tf::errors::FailedPrecondition("Attempting to use uninitialized value ",
                                                       kernel->def().input(i)),
                                     kernel->def());
                }
                // case 6
                // Automatically deref the tensor ref when the op expects a
                // tensor but is given a ref to a tensor.  Need to deref it
                // under the mutex.
                {
                    tf::mutex_lock l(*(entry->ref_mu));
                    DCHECK(!entry->val_field_is_set);
                    entry->val.Init(*entry->ref);
                    entry->val_field_is_set = true;
                }
                entry->ref = nullptr;
                entry->ref_mu = nullptr;

                inp->tensor = entry->val.get();
            }
            if (!on_same_device) {
                // case 7, 8
                // Operation and input on different device,
                // do a copy tensor to ensure input tensor is on the same device
                tf::Tensor copy(device->GetAllocator(expected),
                                inp->tensor->dtype(),
                                inp->tensor->shape());

                INFO("Copying from device {} to device {} to prepare {}-th input for op {}. "
                    " target tensor buffer addr: {:x}",
                    entry->device->name(), device->name(), i, kernel->name(),
                    as_hex(copy.tensor_data().data()));

                tf::Status ok;
                auto dstDevCtx = device_context;
                if (!dstDevCtx) {
                    // Copied from OpKernelContext::op_device_context
                    auto* dev_info = device->tensorflow_gpu_device_info();
                    if (dev_info) dstDevCtx = dev_info->default_context;
                }
                INFO("Src dev context {}, dst dev context {}",
                        as_hex(entry->device_context), as_hex(dstDevCtx));

                tf::Notification n;
                tf::CopyTensor::ViaDMA(tf::strings::StrCat(i, "-th input of ", kernel->name()),
                                       entry->device_context, dstDevCtx,
                                       entry->device, device, entry->alloc_attr,
                                       expected, inp->tensor, &copy,
                                       [&n, &ok](auto status) {
                                            ok = status;
                                            n.Notify();
                                       });
                n.WaitForNotification();

                if (!ok.ok()) {
                    ERR("Copying from device {} to device {} failed when preparing {}-th input "
                        "for op {}: {}",
                        entry->device->name(), device->name(), i, kernel->name(), ok);
                    return ok;
                }

                *inp = {nullptr, &copy};
                entry->alloc_attr = expected;
                entry->device_context = dstDevCtx;
            }
        }

        (*input_device_contexts)[i] = entry->device_context;
        (*input_alloc_attrs)[i] = entry->alloc_attr;

    } // for (int i = 0; i < item.num_inputs; ++i) {
    return tf::Status::OK();
}

tf::Status ExecutorState::ProcessOutputs(const NodeItem &item, tf::OpKernelContext *ctx, tf::Device *device,
                                         EntryVector *outputs, tf::NodeExecStats *stats)
{
    auto node = item.node;
    DCHECK_EQ(0, outputs->size());
    outputs->resize(item.num_outputs);

    auto s = ctx->status();
    if (!s.ok()) {
        s = AttachDef(s, node->def());
        // TODO(misard) Replace with a finer-grain enabling flag once we
        // add better optional debugging support.
        if (vlog_ && VLOG_IS_ON(1)) {
            LOG(WARNING) << this << " Compute status: " << s;
            DumpState();
        }
        return s;
    }

    // Get the device_context for this node id, if it exists.
    auto device_context = ctx->op_device_context();

    // Experimental: debugger (tfdb) access to intermediate node completion.
    if (item.num_outputs == 0 && impl_->params_.node_outputs_cb != nullptr) {
        // If the node has no output, invoke the callback with output slot set to
        // -1, signifying that this is a no-output node.
        s.Update(impl_->params_.node_outputs_cb(item.node->name(), -1, nullptr, false, ctx));
    }

    for (int i = 0; i < item.num_outputs; ++i) {
        auto val = ctx->release_output(i);
        if (*ctx->is_output_dead() || val.tensor == nullptr) {
            // Unless it's a Switch or a Recv, the node must produce a
            // tensor value at i-th output.
            if (!IsSwitch(node) && !IsRecv(node)) {
                s.Update(tf::errors::Internal("Missing ", i, "-th output from ", SummarizeNodeDef(node->def())));
            }
        } else {
            Entry *out = &((*outputs)[i]);

            // Set the device of the output entry.
            out->device = device;

            // Set the device context of the output entry.
            out->device_context = device_context;

            // Set the allocator attributes of the output entry.
            out->alloc_attr = ctx->output_alloc_attr(i);

            // Sanity check of output tensor types.
            auto dtype = val->dtype();
            if (val.is_ref())
                dtype = MakeRefType(dtype);
            if (dtype == item.output_type(i)) {
                if (stats && val.tensor->IsInitialized()) {
                    nodestats::SetOutput(stats, i, val.tensor);
                }
                if (val.is_ref()) {
                    out->has_value = true;
                    out->ref = val.tensor;
                    out->ref_mu = val.mutex_if_ref;

                    // Experimental: debugger (tfdb) access to intermediate node
                    // outputs.
                    if (impl_->params_.node_outputs_cb != nullptr) {
                        s.Update(impl_->params_.node_outputs_cb(item.node->name(), i, out->ref, true, ctx));
                    }
                } else {
                    // NOTE that std::move is used here, so val.tensor goes to
                    // uninitialized state (val.tensor->IsInitialized return false).
                    DCHECK(!out->val_field_is_set);
                    out->has_value = true;
                    out->val_field_is_set = true;
                    out->val.Init(std::move(*val.tensor));

                    // Experimental: debugger access to intermediate node outputs.
                    if (impl_->params_.node_outputs_cb != nullptr) {
                        s.Update(
                            impl_->params_.node_outputs_cb(item.node->name(), i, out->val.get(), false, ctx));
                    }
                }
            } else {
                s.Update(tf::errors::Internal("Output ", i, " of type ", DataTypeString(dtype),
                                          " does not match declared output type ",
                                          DataTypeString(item.output_type(i)), " for node ",
                                          SummarizeNodeDef(node->def())));
            }
        }
        if (!val.is_ref()) {
            // If OpKernelContext returns outputs via pass-by-value, we
            // don't need this trouble.
            delete val.tensor;
        }
    }
    return s;
}

void ExecutorState::PropagateOutputs(const TaggedNode &tagged_node, const NodeItem *item,
                                     EntryVector *outputs, TaggedNodeSeq *ready)
{
    auto node = tagged_node.node;
    FrameState *input_frame = tagged_node.input_frame;
    int64_t input_iter = tagged_node.input_iter;
    const bool is_dead = tagged_node.is_dead;

    TRACE("Propagate outputs for node: {}", node->name());
    // Propagates outputs along out edges, and puts newly ready nodes
    // into the ready queue.
    ready->clear();
    bool is_frame_done = false;
    FrameState *output_frame = input_frame;
    int64_t output_iter = input_iter;

    if (!item->is_enter_exit_or_next_iter) {
        // Fast path for nodes types that don't need special handling
        DCHECK_EQ(input_frame, output_frame);
        // Normal path for most nodes
        tf::mutex_lock l(input_frame->mu);
        output_frame->ActivateNodes(item, is_dead, output_iter, outputs, ready);
        is_frame_done = input_frame->DecrementOutstandingOpsLocked(&impl_->gview_, input_iter, ready);
    } else if (item->is_enter) {
        bool is_constant;
        auto s = GetNodeAttr(node->def(), "is_constant", &is_constant);
        DCHECK(s.ok()) << s;
        FindOrCreateChildFrame(input_frame, input_iter, node, &output_frame);
        output_iter = 0;
        {
            const NodeItem *item = impl_->gview_.node(node->id());
            tf::mutex_lock l(output_frame->mu);
            if (is_constant) {
                // Propagate to all active iterations if this is a loop invariant.
                output_frame->AddLoopInv(item, (*outputs)[0], ready);
            } else {
                output_frame->ActivateNodes(item, is_dead, output_iter, outputs, ready);
            }
            output_frame->num_pending_inputs--;
        }
        is_frame_done = input_frame->DecrementOutstandingOps(&impl_->gview_, input_iter, ready);
    } else if (item->is_exit) {
        if (is_dead) {
            tf::mutex_lock l(input_frame->mu);
            // Stop and remember this node if it is a dead exit.
            if (input_iter == input_frame->iteration_count) {
                input_frame->dead_exits.push_back(node);
            }
            is_frame_done = input_frame->DecrementOutstandingOpsLocked(&impl_->gview_, input_iter, ready);
        } else {
            output_frame = input_frame->parent_frame;
            output_iter = input_frame->parent_iter;
            {
                tf::mutex_lock l(output_frame->mu);
                output_frame->ActivateNodes(item, is_dead, output_iter, outputs, ready);
            }
            is_frame_done = input_frame->DecrementOutstandingOps(&impl_->gview_, input_iter, ready);
        }
    } else {
        DCHECK(IsNextIteration(node));
        tf::mutex_lock l(input_frame->mu);
        if (is_dead) {
            // Stop the deadness propagation.
            output_frame = nullptr;
        } else {
            if (input_iter == input_frame->iteration_count
                && input_frame->num_outstanding_iterations == input_frame->max_parallel_iterations) {
                // Reached the maximum for parallel iterations.
                input_frame->next_iter_roots.push_back({node, (*outputs)[0]});
                output_frame = nullptr;
            } else {
                // If this is a new iteration, start it.
                if (input_iter == input_frame->iteration_count) {
                    input_frame->IncrementIteration(&impl_->gview_, ready);
                }
                output_iter = input_iter + 1;
            }
        }
        if (output_frame != nullptr) {
            // This is the case when node is not Enter, Exit, or NextIteration.
            DCHECK(input_frame == output_frame);
            output_frame->ActivateNodes(item, is_dead, output_iter, outputs, ready);
        }
        is_frame_done = input_frame->DecrementOutstandingOpsLocked(&impl_->gview_, input_iter, ready);
    }

    TRACE("After propagate the ready queue has size: {}", ready->size());
    for (auto &n : *ready) {
        TRACE("    in ready queue: {}", n.node->name());
    }

    // At this point, this node is completely done. We also know if the
    // completion of this node makes its frame completed.
    if (is_frame_done) {
        TRACE("Deleting frames");
        FrameState *parent_frame = input_frame->parent_frame;
        int64_t parent_iter = input_frame->parent_iter;
        DeleteFrame(input_frame, ready);
        TRACE("Frame deleted");
        if (parent_frame != nullptr) {
            // The completion of frame may cause completions in its parent frame.
            // So clean things up recursively.
            TRACE("Cleanup frame iterations");
            CleanupFramesIterations(parent_frame, parent_iter, ready);
            TRACE("Cleanup frame iterations finished");
        }
    }
}

bool ExecutorState::NodeDone(const tf::Status &s, const tf::Node *node, const tf::Device *device,
                             const TaggedNodeSeq &ready,
                             tf::NodeExecStats *stats, TaggedNodeReadyQueue *inline_ready)
{
    if (stats) {
        nodestats::SetAllEnd(stats);
        if (!nodestats::SetTimelineLabel(stats, node)) {
            // Only record non-transfer nodes.
            stats_collector_->Save(device->name(), stats);
        } else {
            delete stats;
        }
    }

    bool abort_run = false;
    if (!s.ok()) {
        // Some error happened. This thread of computation is done.
        TRACE("Try get lock for error handle");
        tf::mutex_lock l(mu_);
        TRACE("Error handle");
        if (status_.ok()) {
            abort_run = true;
            status_ = s;
        }
    }
    if (abort_run) {
//         TRACEPRINTF("StartAbort: %s", s.ToString().c_str());
        if (rendezvous_) {
            rendezvous_->StartAbort(s);
        }
        if (cancellation_manager_) {
            cancellation_manager_->StartCancel();
        }
    }

    TRACE("NodeDone ready size: {}", ready.size());
    TRACE("NodeDone s: {}", s);

    bool completed = false;
    int ready_size = ready.size();
    if (ready_size == 0 || !s.ok()) {
        auto ops = num_outstanding_ops_.fetch_sub(1);
        TRACE("NodeDone num_outstanding_ops_: {}", ops);
        completed = (ops == 1);
    } else if (ready_size > 1) {
        auto ops = num_outstanding_ops_.fetch_add(ready_size - 1, std::memory_order_relaxed);
        TRACE("NodeDone num_outstanding_ops_: {}", ops);
    }

    // Schedule the ready nodes in 'ready'.
    if (s.ok()) {
        ScheduleReady(ready, inline_ready);
    }
    TRACE("NodeDone completed: ", completed);
    return completed;
}

void ExecutorState::ScheduleReady(const TaggedNodeSeq &ready, TaggedNodeReadyQueue *inline_ready)
{
    if (ready.empty()) {
        TRACE("ScheduleReady on an empty ready queue");
        return;
    }
    TRACE("ScheduleReady");

    int64_t scheduled_usec = 0;
    if (stats_collector_) {
        scheduled_usec = nodestats::NowInUsec();
    }
    if (inline_ready == nullptr) {
        // Schedule to run all the ready ops in thread pool.
        TRACE("Schedule to run all the ready ops in thread pool.");
        for (auto &tagged_node : ready) {
            TRACE("Schedule to run the ready op: {}", tagged_node.node->name());
            runner_([=]() { Process(tagged_node, scheduled_usec); });
        }
        TRACE("All ops in ready queue sent to thread pool");
        return;
    }
    auto &gview = impl_->gview_;
    const TaggedNode *curr_expensive_node = nullptr;
    for (auto &tagged_node : ready) {
        const NodeItem &item = *gview.node(tagged_node.node->id());
        TRACE("Visit node {}", item.node->name());
        if (tagged_node.is_dead || !item.kernel_is_expensive) {
            // Inline this inexpensive node.
            inline_ready->push_back(tagged_node);
        } else {
            if (curr_expensive_node) {
                // Dispatch to another thread since there is plenty of work to
                // do for this thread.
                runner_(std::bind(&ExecutorState::Process, this, *curr_expensive_node, scheduled_usec));
            }
            curr_expensive_node = &tagged_node;
        }
    }
    if (curr_expensive_node) {
        if (inline_ready->empty()) {
            // Tail recursion optimization
            inline_ready->push_back(*curr_expensive_node);
        } else {
            // There are inline nodes to run already. We dispatch this expensive
            // node to other thread.
            runner_(std::bind(&ExecutorState::Process, this, *curr_expensive_node, scheduled_usec));
        }
    }
}

const tf::Tensor *ExecutorState::GetTensorValueForDump(const Entry &input)
{
    if (!input.has_value) {
        return kEmptyTensor;
    } else if (input.ref == nullptr) {
        return input.val.get();
    } else {
        return input.ref;
    }
}

void ExecutorState::DumpPendingNodeState(const int node_id, const Entry *input_vector,
                                         const bool show_nodes_with_no_ready_inputs)
{
    auto &node_item = *impl_->gview_.node(node_id);
    auto &node = *node_item.node;
    const auto input_base = node_item.input_start;
    if (!show_nodes_with_no_ready_inputs) {
        bool has_ready_input = false;
        for (int i = 0; i < node.num_inputs(); ++i) {
            auto &input = input_vector[input_base + i];
            auto tensor = GetTensorValueForDump(input);
            if (tensor->IsInitialized()) {
                has_ready_input = true;
                break;
            }
        }
        if (!has_ready_input) {
            return;
        }
    }
    WARN("    Pending Node: {}", node.DebugString());
    for (int i = 0; i < node.num_inputs(); ++i) {
        auto &input = input_vector[input_base + i];
        auto *tensor = GetTensorValueForDump(input);
        if (tensor->IsInitialized()) {
            WARN("      Input {}: Tensor<type: {} shape: {}>",
                 i, DataTypeString(tensor->dtype()), tensor->shape().DebugString());
        } else {
            WARN("      Input {}: not present", i);
        }
    }
}

void ExecutorState::DumpActiveNodeState(const int node_id, const Entry *input_vector)
{
    auto &node_item = *impl_->gview_.node(node_id);
    auto &node = *node_item.node;
    WARN("    Active Node: {}", node.DebugString());
    const int input_base = node_item.input_start;
    for (int i = 0; i < node.num_inputs(); ++i) {
        auto &input = input_vector[input_base + i];
        auto *tensor = GetTensorValueForDump(input);
        if (tensor->IsInitialized()) {
            WARN("      Input {}: Tensor<type: {} shape: {}>",
                 i, DataTypeString(tensor->dtype()), tensor->shape().DebugString());
        } else {
            WARN("      Input {}: not present", i);
        }
    }
}

void ExecutorState::DumpIterationState(const FrameState *frame, IterationState *iteration)
{
    auto *nodes = frame->nodes;
    // Dump any waiting nodes that are holding on to tensors.
    for (auto node : *nodes) {
        int node_id = node->id();
        auto pending_id = impl_->gview_.node(node_id)->pending_id;
        if (iteration->node_state(pending_id) == tf::PendingCounts::PENDING_NOTREADY
            || iteration->node_state(pending_id) == tf::PendingCounts::PENDING_READY) {
            DumpPendingNodeState(node_id, iteration->input_tensors, false);
        }
    }
    // Then the active nodes.
    for (auto node : *nodes) {
        int node_id = node->id();
        auto pending_id = impl_->gview_.node(node_id)->pending_id;
        if (iteration->node_state(pending_id) == tf::PendingCounts::STARTED) {
            DumpActiveNodeState(node_id, iteration->input_tensors);
        }
    }
    // Show all input tensors in use.
    int total_input_tensors = frame->total_input_tensors;
    size_t total_bytes = 0;
    for (int i = 0; i < total_input_tensors; ++i) {
        auto &input = iteration->input_tensors[i];
        auto *tensor = GetTensorValueForDump(input);
        if (tensor->IsInitialized()) {
            WARN("    Input {}: Tensor<type: {} shape: {}, bytes: {}>",
                 i, DataTypeString(tensor->dtype()), tensor->shape().DebugString(),
                 tensor->TotalBytes());
            total_bytes += tensor->TotalBytes();
        }
    }
    WARN("    Total bytes {}", total_bytes);
}

void ExecutorState::DumpState()
{
    tf::mutex_lock l(mu_);
    if (!dumped_on_error_) {
        WARN("Dumping state");
        for (auto &frame : outstanding_frames_) {
            WARN(frame.first);
            FrameState *frame_state = frame.second;
            tf::mutex_lock frame_lock(frame_state->mu);
            for (IterationState *iteration : frame_state->iterations) {
                WARN("  Iteration:");
                DumpIterationState(frame_state, iteration);
            }
        }
        dumped_on_error_ = true;
    }
}

void ExecutorState::Finish()
{
    TRACE("ExecutorState::Finish try lock in thread");
    mu_.lock();
    auto status = status_;
    auto done_cb = std::move(done_cb_);
    auto runner = std::move(runner_);
    mu_.unlock();
    TRACE("ExecutorState::Finish after lock");
    if (sync_on_finish_ && status.ok()) {
        // Block until the device has finished all queued operations. For
        // devices like GPUs that continue to execute Ops after their Compute
        // methods have completed, this ensures that control is not returned to
        // the user until the step (and its side-effects) has actually completed.
        for (auto d : used_devices_) {
            status.Update(d->Sync());
        }
    }
    TRACE("ExecutorState about to delete this");
    delete this;
    CHECK(done_cb != nullptr);
    runner([=]() { done_cb(status); });
}

void ExecutorState::FindOrCreateChildFrame(FrameState *frame, int64_t iter, const tf::Node *node,
                                           FrameState **child)
{
    // Get the child frame name.
    std::string enter_name;
    auto s = GetNodeAttr(node->def(), "frame_name", &enter_name);
    DCHECK(s.ok()) << s;
    const std::string child_name = MakeFrameName(frame, iter, enter_name);

    {
        tf::mutex_lock executor_lock(mu_);
        auto it = outstanding_frames_.find(child_name);
        if (it != outstanding_frames_.end()) {
            *child = it->second;
            return;
        }
    }

    // Need to create a new frame instance.
    // Note that this new frame instance is created without any locks.
    TRACE("Create frame: {}", child_name);

    int parallel_iters;
    s = GetNodeAttr(node->def(), "parallel_iterations", &parallel_iters);
    DCHECK(s.ok()) << s;
    FrameState *temp = new FrameState(impl_, parallel_iters);
    temp->frame_name = child_name;
    temp->frame_id = tf::Hash64(child_name);
    temp->parent_frame = frame;
    temp->parent_iter = iter;
    temp->InitializeFrameInfo(enter_name);

    // 'iterations' is a fixed-length circular buffer.
    temp->iterations.resize(temp->max_parallel_iterations + 1);
    // Initialize iteration 0.
    temp->iterations[0] = new IterationState(temp->pending_counts, temp->total_input_tensors);

    {
        tf::mutex_lock executor_lock(mu_);
        auto it = outstanding_frames_.find(child_name);
        if (it != outstanding_frames_.end()) {
            *child = it->second;
        } else {
            tf::mutex_lock frame_lock(frame->mu);
            frame->GetIteration(iter)->outstanding_frame_count++;
            outstanding_frames_[child_name] = temp;
            *child = temp;
            temp = nullptr;
        }
    }
    delete temp; // Not used so delete it.
}

void ExecutorState::DeleteFrame(FrameState *frame, TaggedNodeSeq *ready)
{
    // First, propagate dead_exits (if any) to the parent frame.
    FrameState *parent_frame = frame->parent_frame;
    int64_t parent_iter = frame->parent_iter;
    if (parent_frame != nullptr) {
        tf::mutex_lock paranet_frame_lock(parent_frame->mu);
        // Propagate all the dead exits to the parent frame.
        for (auto *node : frame->dead_exits) {
            auto parent_iter_state = parent_frame->GetIteration(parent_iter);
            for (auto *e : node->out_edges()) {
                auto *dst_node = e->dst();

                auto dst_pending_id = impl_->gview_.node(dst_node->id())->pending_id;

                // TODO(yuanbyu): We don't need this if we require the subgraph
                // given to an executor not to contain a sink node.
                if (dst_node->IsSink())
                    continue;

                bool dst_dead = true;
                bool dst_ready = false;
                // We know this is a dead input to dst.
                if (IsMerge(dst_node)) {
                    if (e->IsControlEdge()) {
                        parent_iter_state->decrement_pending(dst_pending_id, 2);
                        int count = parent_iter_state->pending(dst_pending_id);
                        int dead_cnt = parent_iter_state->dead_count(dst_pending_id);
                        dst_dead = (dead_cnt == dst_node->num_inputs());
                        dst_ready = (count == 0) || ((count == 1) && dst_dead);
                    } else {
                        parent_iter_state->increment_dead_count(dst_pending_id);
                        const int dead_cnt = parent_iter_state->dead_count(dst_pending_id);
                        dst_dead = (dead_cnt == dst_node->num_inputs());
                        dst_ready = (parent_iter_state->pending(dst_pending_id) == 1) && dst_dead;
                    }
                } else {
                    parent_iter_state->increment_dead_count(dst_pending_id);
                    dst_ready = (parent_iter_state->decrement_pending(dst_pending_id, 1) == 0);
                }
                if (dst_ready) {
                    if (IsControlTrigger(dst_node))
                        dst_dead = false;
                    ready->push_back(TaggedNode(dst_node, parent_frame, parent_iter, dst_dead));
                    parent_iter_state->outstanding_ops++;
                }
            }
        }
    }

    // Delete the frame.
    auto &frame_name = frame->frame_name;
    TRACE("Delete frame {}", frame_name);
    {
        tf::mutex_lock executor_lock(mu_);
        outstanding_frames_.erase(frame_name);
    }
    delete frame;
}

void ExecutorState::CleanupFramesIterations(FrameState *frame, int64_t iter, TaggedNodeSeq *ready)
{
    bool is_frame_done = false;
    {
        tf::mutex_lock frame_lock(frame->mu);
        frame->GetIteration(iter)->outstanding_frame_count--;
        is_frame_done = frame->CleanupIterations(&impl_->gview_, iter, ready);
    }
    if (is_frame_done) {
        auto parent_frame = frame->parent_frame;
        auto parent_iter = frame->parent_iter;
        DeleteFrame(frame, ready);
        if (parent_frame != nullptr) {
            // The completion of frame may cause completions in its parent frame.
            // So clean things up recursively.
            CleanupFramesIterations(parent_frame, parent_iter, ready);
        }
    }
}

void ExecutorState::FrameState::ActivateNodes(const NodeItem *item, const bool is_dead, int64_t iter,
                                              EntryVector *outputs, TaggedNodeSeq *ready)
{
    auto &gview = executor->gview_;
    auto iter_state = GetIteration(iter);
    auto num_output_edges = item->num_output_edges;
    auto edges = item->output_edge_list();
    auto input_tensors = iter_state->input_tensors;
    for (int out_index = 0; out_index < num_output_edges; out_index++) {
        auto &e = edges[out_index];
        auto dst_id = e.dst_id;
        auto *dst_item = gview.node(dst_id);
        auto dst_pending_id = dst_item->pending_id;
        auto src_slot = e.output_slot;

        // TODO(yuanbyu): We don't need this if we require the subgraph
        // given to an executor not to contain a sink node.
        if (dst_item->is_sink)
            continue;

        bool dst_dead = false;
        bool dst_ready = false;
        // True iff this input for dst is needed. We only set this input for
        // dst if this flag is true. This is needed to make the thread safety
        // analysis happy.
        const bool is_control_edge = (src_slot == tf::Graph::kControlSlot);
        bool dst_need_input = !is_control_edge;
        if (dst_item->is_merge) {
            // A merge node is ready if all control inputs have arrived and either
            // a) a live data input becomes available or b) all data inputs are
            // dead. For Merge, pending's LSB is set iff a live data input has
            // arrived.
            if (is_control_edge) {
                iter_state->decrement_pending(dst_pending_id, 2);
                int count = iter_state->pending(dst_pending_id);
                int dead_cnt = iter_state->dead_count(dst_pending_id);
                dst_dead = (dead_cnt == dst_item->num_inputs);
                dst_ready = (count == 0) || ((count == 1) && dst_dead);
            } else {
                if ((*outputs)[src_slot].has_value) {
                    // This is a live data input.
                    int count = iter_state->pending(dst_pending_id);
                    iter_state->mark_live(dst_pending_id);
                    // Only the first live edge sets the input and (potentially)
                    // triggers execution. The low bit of count is set if and
                    // only if no live input has been used yet (mark_live clears
                    // it). The node should be started if and only if this is
                    // the first live input and there are no pending control
                    // edges, i.e. count == 1.
                    dst_ready = (count == 1);
                    dst_need_input = ((count & 0x1) == 1);
                } else {
                    // This is a dead data input. Note that dst_node is dead if node is
                    // a dead enter. We need this to handle properly a while loop on
                    // the untaken branch of a conditional.
                    // TODO(yuanbyu): This is a bit hacky, but a good solution for
                    // now.
                    iter_state->increment_dead_count(dst_pending_id);
                    const int dead_cnt = iter_state->dead_count(dst_pending_id);
                    dst_dead = (dead_cnt == dst_item->num_inputs) || item->is_enter;
                    dst_ready = (iter_state->pending(dst_pending_id) == 1) && dst_dead;
                    dst_need_input = false;
                }
            }
        } else {
            bool increment_dead = (is_dead || (!is_control_edge && !(*outputs)[src_slot].has_value));
            int pending, dead;
            iter_state->adjust_for_activation(dst_pending_id, increment_dead, &pending, &dead);
            dst_dead = (dead > 0);
            dst_ready = (pending == 0);
        }

        if (dst_need_input) {
            const int dst_slot = e.input_slot;
            const int dst_loc = dst_item->input_start + dst_slot;
            if (e.is_last) {
                input_tensors[dst_loc] = std::move((*outputs)[src_slot]);
            } else {
                input_tensors[dst_loc] = (*outputs)[src_slot];
            }
        }

        // Add dst to the ready queue if it's ready
        if (dst_ready) {
            if (dst_item->is_control_trigger)
                dst_dead = false;
            ready->push_back(TaggedNode(dst_item->node, this, iter, dst_dead));
            iter_state->outstanding_ops++;
        }
    }
}

void ExecutorState::FrameState::ActivateNexts(const GraphView *gview, int64_t iter, TaggedNodeSeq *ready)
{
    // Propagate the deferred NextIteration nodes to the new iteration.
    for (auto &node_entry : next_iter_roots) {
        auto *node = node_entry.first;
        auto &entry = node_entry.second;
        auto is_dead = !entry.has_value;
        auto *item = gview->node(node->id());
        EntryVector outputs{entry};
        ActivateNodes(item, is_dead, iter, &outputs, ready);
    }
    next_iter_roots.clear();
}

void ExecutorState::FrameState::ActivateLoopInvs(const GraphView *gview, int64_t iter, TaggedNodeSeq *ready)
{
    // Propagate loop invariants to the new iteration.
    for (auto &node_entry : inv_values) {
        auto *node = node_entry.first;
        auto &entry = node_entry.second;
        auto is_dead = !entry.has_value;
        auto *item = gview->node(node->id());
        EntryVector outputs{entry};
        ActivateNodes(item, is_dead, iter, &outputs, ready);
    }
}

void ExecutorState::FrameState::AddLoopInv(const NodeItem *item, const Entry &entry, TaggedNodeSeq *ready)
{
    // Store this value.
    inv_values.push_back({item->node, entry});

    // Make this value available to all iterations.
    bool is_dead = !entry.has_value;
    for (int i = 0; i <= iteration_count; ++i) {
        EntryVector outputs{entry};
        ActivateNodes(item, is_dead, i, &outputs, ready);
    }
}

bool ExecutorState::FrameState::IsIterationDone(int64_t iter)
{
    auto iter_state = GetIteration(iter);
    if (iter_state->outstanding_ops == 0 && iter_state->outstanding_frame_count == 0) {
        if (iter == 0) {
            // The enclosing frame has no pending input.
            return num_pending_inputs == 0;
        } else {
            // The preceding iteration is deleted (and therefore done).
            return (GetIteration(iter - 1) == nullptr);
        }
    }
    return false;
}

void ExecutorState::FrameState::IncrementIteration(const GraphView *gview, TaggedNodeSeq *ready)
{
    iteration_count++;
    int64_t next_iter = iteration_count;

    // Initialize the next iteration.
    auto iter_state = new IterationState(pending_counts, total_input_tensors);
    SetIteration(next_iter, iter_state);
    num_outstanding_iterations++;
    dead_exits.clear();

    // Activate the successors of the deferred roots in the new iteration.
    ActivateNexts(gview, next_iter, ready);

    // Activate the loop invariants in the new iteration.
    ActivateLoopInvs(gview, next_iter, ready);
}

bool ExecutorState::FrameState::CleanupIterations(const GraphView *gview, int64_t iter, TaggedNodeSeq *ready)
{
    auto curr_iter = iter;
    while (curr_iter <= iteration_count && IsIterationDone(curr_iter)) {
        // Delete the iteration curr_iter.
        delete GetIteration(curr_iter);
        SetIteration(curr_iter, nullptr);
        --num_outstanding_iterations;
        ++curr_iter;

        // When one iteration is completed, we check for deferred iteration,
        // and start it if there is one.
        if (!next_iter_roots.empty()) {
            IncrementIteration(gview, ready);
        }
    }
    return IsFrameDone();
}

void ExecutorImpl::RunAsync(const Args &args, DoneCallback done)
{
    (new ExecutorState(args, this))->RunAsync(done);
}
