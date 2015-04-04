// The Firmament project
// Copyright (c) 2014 Malte Schwarzkopf <malte.schwarzkopf@cl.cam.ac.uk>
// Copyright (c) 2015 Ionel Gog <ionel.gog@cl.cam.ac.uk>
//
// WhareMap cost model.

#include "scheduling/cost_models/wharemap_cost_model.h"

#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/common.h"
#include "base/types.h"
#include "misc/map-util.h"
#include "misc/pb_utils.h"
#include "misc/utils.h"
#include "scheduling/knowledge_base.h"
#include "scheduling/cost_models/flow_scheduling_cost_model_interface.h"
#include "scheduling/dimacs_change_arc.h"
#include "scheduling/flow_graph.h"

namespace firmament {

WhareMapCostModel::WhareMapCostModel(shared_ptr<ResourceMap_t> resource_map,
                                     shared_ptr<TaskMap_t> task_map,
                                     KnowledgeBase* kb)
  : resource_map_(resource_map),
    task_map_(task_map),
    knowledge_base_(kb) {
}

const TaskDescriptor& WhareMapCostModel::GetTask(TaskID_t task_id) {
  TaskDescriptor* td = FindPtrOrNull(*task_map_, task_id);
  CHECK_NOTNULL(td);
  return *td;
}

// The cost of leaving a task unscheduled should be higher than the cost of
// scheduling it.
Cost_t WhareMapCostModel::TaskToUnscheduledAggCost(TaskID_t task_id) {
  // TODO(ionel): Implement!
  const TaskDescriptor& td = GetTask(task_id);
  uint64_t now = GetCurrentTimestamp();
  uint64_t time_since_submit = now - td.submit_time();
  // timestamps are in microseconds, but we scale to tenths of a second here in
  // order to keep the costs small
  return (time_since_submit / 100000);
}

// The cost from the unscheduled to the sink is 0. Setting it to a value greater
// than zero affects all the unscheduled tasks. It is better to affect the cost
// of not running a task through the cost from the task to the unscheduled
// aggregator.
Cost_t WhareMapCostModel::UnscheduledAggToSinkCost(JobID_t job_id) {
  return 0ULL;
}

// The cost from the task to the cluster aggregator models how expensive is a
// task to run on any node in the cluster. The cost of the topology's arcs are
// the same for all the tasks.
Cost_t WhareMapCostModel::TaskToClusterAggCost(TaskID_t task_id) {
  // TODO(ionel): Implement!
  vector<EquivClass_t>* equiv_classes = GetTaskEquivClasses(task_id);
  CHECK_GT(equiv_classes->size(), 0);
  // Avg runtime is in milliseconds, so we convert it to tenths of a second
  uint64_t avg_runtime =
    knowledge_base_->GetAvgRuntimeForTEC(equiv_classes->front());
  delete equiv_classes;
  return (avg_runtime * 100);
}

Cost_t WhareMapCostModel::TaskToResourceNodeCost(TaskID_t task_id,
                                                 ResourceID_t resource_id) {
  // Tasks do not have preference arcs to resources.
  LOG(FATAL) << "Should not be called";
  return 0LL;
}

Cost_t WhareMapCostModel::ResourceNodeToResourceNodeCost(
    ResourceID_t source,
    ResourceID_t destination) {
  // TODO(ionel): Implement!
  return 0LL;
}

// The cost from the resource leaf to the sink is 0.
Cost_t WhareMapCostModel::LeafResourceNodeToSinkCost(ResourceID_t resource_id) {
  return 0LL;
}

Cost_t WhareMapCostModel::TaskContinuationCost(TaskID_t task_id) {
  LOG(FATAL) << "Should not be called";
  return 0LL;
}

Cost_t WhareMapCostModel::TaskPreemptionCost(TaskID_t task_id) {
  LOG(FATAL) << "Should not be called";
  return 0LL;
}

Cost_t WhareMapCostModel::TaskToEquivClassAggregator(TaskID_t task_id,
                                                     EquivClass_t tec) {
  // TODO(ionel): Implement!
  return 0LL;
}

Cost_t WhareMapCostModel::EquivClassToResourceNode(EquivClass_t tec,
                                                   ResourceID_t res_id) {
  // TODO(ionel): Implement!
  return 0LL;
}

Cost_t WhareMapCostModel::EquivClassToEquivClass(EquivClass_t tec1,
                                                 EquivClass_t tec2) {
  // TODO(ionel): Implement!
  return 0LL;
}

vector<EquivClass_t>* WhareMapCostModel::GetTaskEquivClasses(
    TaskID_t task_id) {
  vector<EquivClass_t>* equiv_classes = new vector<EquivClass_t>();
  TaskDescriptor* td_ptr = FindPtrOrNull(*task_map_, task_id);
  CHECK_NOTNULL(td_ptr);
  // We have one task agg per job. The id of the aggregator is the hash
  // of the job id.
  EquivClass_t task_agg = static_cast<EquivClass_t>(HashJobID(*td_ptr));
  equiv_classes->push_back(task_agg);
  task_aggs_.insert(task_agg);
  unordered_map<EquivClass_t, set<TaskID_t> >::iterator task_ec_it =
    task_ec_to_set_task_id_.find(task_agg);
  if (task_ec_it != task_ec_to_set_task_id_.end()) {
    task_ec_it->second.insert(task_id);
  } else {
    set<TaskID_t> task_set;
    task_set.insert(task_id);
    CHECK(InsertIfNotPresent(&task_ec_to_set_task_id_, task_agg, task_set));
  }
  return equiv_classes;
}

vector<EquivClass_t>* WhareMapCostModel::GetResourceEquivClasses(
    ResourceID_t res_id) {
  vector<EquivClass_t>* equiv_classes = new vector<EquivClass_t>();
  // Get the machine aggregator corresponding to this machine.
  EquivClass_t* ec_class = FindOrNull(machine_to_ec_, res_id);
  if (ec_class != NULL) {
    equiv_classes->push_back(*ec_class);
  }
  return equiv_classes;
}

vector<ResourceID_t>* WhareMapCostModel::GetOutgoingEquivClassPrefArcs(
    EquivClass_t tec) {
  vector<ResourceID_t>* prefered_res = new vector<ResourceID_t>();
  if (task_aggs_.find(tec) != task_aggs_.end()) {
    // tec is a task aggregator.
    // Iterate over all the machines.
    multimap<Cost_t, ResourceID_t> priority_res;
    for (unordered_map<ResourceID_t, const ResourceTopologyNodeDescriptor*,
           boost::hash<boost::uuids::uuid>>::iterator
           it = machine_to_rtnd_.begin();
         it != machine_to_rtnd_.end(); ++it) {
      Cost_t cost_to_res = EquivClassToResourceNode(tec, it->first);
      ResourceID_t res_id =
        ResourceIDFromString(it->second->resource_desc().uuid());
      if (priority_res.size() < FLAGS_num_pref_arcs_agg_to_res) {
        priority_res.insert(pair<Cost_t, ResourceID_t>(cost_to_res, res_id));
      } else {
        multimap<Cost_t, ResourceID_t>::reverse_iterator rit =
          priority_res.rbegin();
        if (cost_to_res < rit->first) {
          priority_res.erase(priority_res.find(rit->first));
          priority_res.insert(pair<Cost_t, ResourceID_t>(cost_to_res, res_id));
        }
      }
    }
    for (multimap<Cost_t, ResourceID_t>::iterator it = priority_res.begin();
         it != priority_res.end(); ++it) {
      prefered_res->push_back(it->second);
    }
  } else if (machine_aggs_.find(tec) != machine_aggs_.end()) {
    // tec is a machine aggregator.
    multimap<EquivClass_t, ResourceID_t>::iterator it =
      machine_ec_to_res_id_.find(tec);
    multimap<EquivClass_t, ResourceID_t>::iterator it_to =
      machine_ec_to_res_id_.upper_bound(tec);
    for (; it != it_to; ++it) {
      prefered_res->push_back(it->second);
    }
  } else {
    LOG(FATAL) << "Unexpected type of task equivalence aggregator";
  }
  return prefered_res;
}

vector<TaskID_t>* WhareMapCostModel::GetIncomingEquivClassPrefArcs(
    EquivClass_t tec) {
  vector<TaskID_t>* prefered_task = new vector<TaskID_t>();
  if (task_aggs_.find(tec) != task_aggs_.end()) {
    // tec is a task aggregator.
    // This is where we add preference arcs from tasks to new equiv class
    // aggregators.
    // XXX(ionel): This is very slow because it iterates over all tasks.
    for (TaskMap_t::iterator it = task_map_->begin(); it != task_map_->end();
         ++it) {
      EquivClass_t task_agg =
        static_cast<EquivClass_t>(HashJobID(*(it->second)));
      if (task_agg == tec) {
        prefered_task->push_back(it->first);
      }
    }
  } else if (machine_aggs_.find(tec) != machine_aggs_.end()) {
    // tec is a machine aggregator.
    // This is where we can add arcs form tasks to machine aggregators.
    // We do not need to add any arcs in the WhareMap cost model.
  } else {
    LOG(FATAL) << "Unexpected type of task equivalence aggregator";
  }
  return prefered_task;
}

vector<ResourceID_t>* WhareMapCostModel::GetTaskPreferenceArcs(
    TaskID_t task_id) {
  // Tasks do not have preference arcs to resources.
  vector<ResourceID_t>* prefered_res = new vector<ResourceID_t>();
  return prefered_res;
}

pair<vector<EquivClass_t>*, vector<EquivClass_t>*>
    WhareMapCostModel::GetEquivClassToEquivClassesArcs(EquivClass_t tec) {
  vector<EquivClass_t>* incoming_ec = new vector<EquivClass_t>();
  vector<EquivClass_t>* outgoing_ec = new vector<EquivClass_t>();
  if (task_aggs_.find(tec) != task_aggs_.end()) {
    // Add the machine equivalence classes to the vector.
    for (unordered_set<EquivClass_t>::iterator
           it = machine_aggs_.begin();
         it != machine_aggs_.end();
         ++it) {
      outgoing_ec->push_back(*it);
    }
  } else if (machine_aggs_.find(tec) != machine_aggs_.end()) {
    // Add the task equivalence classes to the vector.
    for (unordered_set<EquivClass_t>::iterator
           it = task_aggs_.begin();
         it != task_aggs_.end();
         ++it) {
      incoming_ec->push_back(*it);
    }
  } else {
    LOG(FATAL) << "Unexpected type of task equiv class";
  }
  return pair<vector<EquivClass_t>*,
              vector<EquivClass_t>*>(incoming_ec, outgoing_ec);
}

void WhareMapCostModel::AddMachine(
    ResourceTopologyNodeDescriptor* rtnd_ptr) {
  CHECK_EQ(rtnd_ptr->resource_desc().type(),
           ResourceDescriptor::RESOURCE_MACHINE);
  size_t hash = 42;
  BFSTraverseResourceProtobufTreeToHash(
      rtnd_ptr, &hash,
      boost::bind(&WhareMapCostModel::ComputeMachineTypeHash, this, _1, _2));
  ResourceID_t res_id = ResourceIDFromString(rtnd_ptr->resource_desc().uuid());
  // Set the number of cores for the machine.
  EquivClass_t machine_ec = static_cast<EquivClass_t>(hash);
  // Add mapping between task equiv class and resource id.
  machine_ec_to_res_id_.insert(
      pair<EquivClass_t, ResourceID_t>(machine_ec, res_id));
  // Add mapping between resource id and resource topology node.
  InsertIfNotPresent(&machine_to_rtnd_, res_id, rtnd_ptr);
  // Add mapping between resource id and machine equiv class.
  InsertIfNotPresent(&machine_to_ec_, res_id, machine_ec);
  // Add machine to the machine aggregators set.
  machine_aggs_.insert(machine_ec);
}

void WhareMapCostModel::RemoveMachine(ResourceID_t res_id) {
  EquivClass_t* machine_ec = FindOrNull(machine_to_ec_, res_id);
  CHECK_NOTNULL(machine_ec);
  // Rempve the machine from the machine ec map.
  multimap<EquivClass_t, ResourceID_t>::iterator it =
    machine_ec_to_res_id_.find(*machine_ec);
  multimap<EquivClass_t, ResourceID_t>::iterator it_to =
    machine_ec_to_res_id_.upper_bound(*machine_ec);
  uint32_t num_machines_per_ec = 0;
  for (; it != it_to; it++, num_machines_per_ec++) {
    if (it->second == res_id) {
      break;
    }
  }
  // Check we actually found the machine.
  if (it == it_to) {
    LOG(FATAL) << "Could not find the machine";
  }
  machine_ec_to_res_id_.erase(it);
  machine_to_rtnd_.erase(res_id);
  machine_to_ec_.erase(res_id);
  // Remove the machine ec from the agg set if we removed the
  // last machine of this type.
  if (num_machines_per_ec == 1) {
    machine_aggs_.erase(*machine_ec);
  }
}

void WhareMapCostModel::RemoveTask(TaskID_t task_id) {
  vector<EquivClass_t>* equiv_classes = GetTaskEquivClasses(task_id);
  for (vector<EquivClass_t>::iterator it = equiv_classes->begin();
       it != equiv_classes->end(); ++it) {
    unordered_map<EquivClass_t, set<TaskID_t> >::iterator set_it =
      task_ec_to_set_task_id_.find(*it);
    if (set_it != task_ec_to_set_task_id_.end()) {
      set_it->second.erase(task_id);
      if (set_it->second.size() == 0) {
        task_ec_to_set_task_id_.erase(*it);
        task_aggs_.erase(*it);
      }
    }
  }
}

void WhareMapCostModel::ComputeMachineTypeHash(
    const ResourceTopologyNodeDescriptor* rtnd_ptr, size_t* hash) {
  boost::hash_combine(*hash, rtnd_ptr->resource_desc().type());
}

FlowGraphNode* WhareMapCostModel::GatherStats(FlowGraphNode* accumulator,
                                              FlowGraphNode* other) {
  if (accumulator->type_.type() == FlowNodeType::ROOT_TASK ||
      accumulator->type_.type() == FlowNodeType::SCHEDULED_TASK ||
      accumulator->type_.type() == FlowNodeType::UNSCHEDULED_TASK ||
      accumulator->type_.type() == FlowNodeType::JOB_AGGREGATOR ||
      accumulator->type_.type() == FlowNodeType::SINK) {
    // Node is neither part of the topology or an equivalence class.
    // We don't have to accumulate any state.
    // Cases: 1) TASK -> EQUIV
    //        2) TASK -> RESOURCE
    return accumulator;
  }

  if (other->resource_id_.is_nil()) {
    if (accumulator->type_.type() == FlowNodeType::PU) {
      // Base case: (PU -> SINK). We are at a PU and we gather the statistics.
      ResourceStatus* rs_ptr =
        FindPtrOrNull(*resource_map_, accumulator->resource_id_);
      CHECK_NOTNULL(rs_ptr);
      ResourceDescriptor* rd_ptr = rs_ptr->mutable_descriptor();
      if (rd_ptr->has_current_running_task()) {
        TaskDescriptor* td_ptr =
          FindPtrOrNull(*task_map_, rd_ptr->current_running_task());
        if (td_ptr->has_task_type()) {
          // TODO(ionel): Gather the statistics.
          WhareMapStats* wms_ptr = rd_ptr->mutable_whare_map_stats();
          if (td_ptr->task_type() == TaskDescriptor::DEVIL) {
            wms_ptr->set_num_devils(1);
          } else if (td_ptr->task_type() == TaskDescriptor::RABBIT) {
            wms_ptr->set_num_rabbits(1);
          } else if (td_ptr->task_type() == TaskDescriptor::SHEEP) {
            wms_ptr->set_num_sheep(1);
          } else if (td_ptr->task_type() == TaskDescriptor::TURTLE) {
            wms_ptr->set_num_turtles(1);
          } else {
            LOG(FATAL) << "Unexpected task type";
          }
        } else {
          LOG(WARNING) << "Task " << td_ptr->uid() << " does not have a type";
        }
      }
    }
    return accumulator;
  }
  if (accumulator->type_.type() == FlowNodeType::EQUIVALENCE_CLASS) {
    if (!other->resource_id_.is_nil() &&
        other->type_.type() == FlowNodeType::MACHINE) {
      // Case: (EQUIV -> MACHINE).
      // We don't have to do anything.
    } else if (other->type_.type() == FlowNodeType::EQUIVALENCE_CLASS) {
      // Case: (EQUIV -> EQUIV).
      // We don't have to do anything.
    } else {
      LOG(FATAL) << "Unexpected preference arc";
    }
    // TODO(ionel): Update knowledge base.
    return accumulator;
  }
  // Case: (RESOURCE -> RESOURCE)
  ResourceStatus* acc_rs_ptr =
    FindPtrOrNull(*resource_map_, accumulator->resource_id_);
  CHECK_NOTNULL(acc_rs_ptr);
  WhareMapStats* wms_acc_ptr =
    acc_rs_ptr->mutable_descriptor()->mutable_whare_map_stats();
  ResourceStatus* other_rs_ptr =
    FindPtrOrNull(*resource_map_, other->resource_id_);
  CHECK_NOTNULL(other_rs_ptr);
  WhareMapStats* wms_other_ptr =
    other_rs_ptr->mutable_descriptor()->mutable_whare_map_stats();
  if (accumulator->type_.type() == FlowNodeType::MACHINE) {
    AccumulateWhareMapStats(wms_acc_ptr, wms_other_ptr);
    // TODO(ionel): Update knowledge base.
    return accumulator;
  }
  AccumulateWhareMapStats(wms_acc_ptr, wms_other_ptr);
  return accumulator;
}

FlowGraphNode* WhareMapCostModel::UpdateStats(FlowGraphNode* accumulator,
                                              FlowGraphNode* other) {
  if (accumulator->type_.type() == FlowNodeType::ROOT_TASK ||
      accumulator->type_.type() == FlowNodeType::SCHEDULED_TASK ||
      accumulator->type_.type() == FlowNodeType::UNSCHEDULED_TASK ||
      accumulator->type_.type() == FlowNodeType::JOB_AGGREGATOR ||
      accumulator->type_.type() == FlowNodeType::SINK) {
    // Node is neither part of the topology or an equivalence class.
    // We don't have to accumulate any state.
    // Cases: 1) TASK -> EQUIV
    //        2) TASK -> RESOURCE
    return accumulator;
  }
  if (other->resource_id_.is_nil()) {
    if (accumulator->type_.type() == FlowNodeType::PU) {
      // Base case: (PU -> SINK)
      // We don't have to do anything.
    }
    return accumulator;
  }
  if (accumulator->type_.type() == FlowNodeType::EQUIVALENCE_CLASS) {
    if (other->type_.type() == FlowNodeType::EQUIVALENCE_CLASS) {
      // Case: EQUIV -> EQUIV
    } else if (other->type_.type() == FlowNodeType::MACHINE) {
      // Case: EQUIV -> MACHINE
    } else {
      LOG(FATAL) << "Unexpected preference arc";
    }
    return accumulator;
  }
  // Case: RESOURCE -> RESOURCE
  FlowGraphArc* arc = FlowGraph::GetArc(accumulator, other);
  arc->cost_ = ResourceNodeToResourceNodeCost(accumulator->resource_id_,
                                              other->resource_id_);
  DIMACSChange *chg = new DIMACSChangeArc(*arc);
  chg->SetComment("WhareMap/UpdateStats");
  flow_graph_->AddGraphChange(chg);
  return accumulator;
}

void WhareMapCostModel::AccumulateWhareMapStats(WhareMapStats* accumulator,
                                                WhareMapStats* other) {
  accumulator->set_num_devils(accumulator->num_devils() +
                              other->num_devils());
  accumulator->set_num_rabbits(accumulator->num_rabbits() +
                               other->num_rabbits());
  accumulator->set_num_sheep(accumulator->num_sheep() +
                             other->num_sheep());
  accumulator->set_num_turtles(accumulator->num_turtles() +
                               other->num_turtles());
}

}  // namespace firmament